#include "Runtime/KnifeUpdateProbe.hpp"
#include "Runtime/KnifeGraph.hpp"
#include "MinHook.h"

#include <algorithm>
#include <intrin.h>
#include <limits>

namespace nskinzcs2::runtime
{
std::atomic<KnifeUpdateProbe*> KnifeUpdateProbe::owner_ {nullptr};
thread_local std::uintptr_t KnifeUpdateProbe::boundary_arms_ = 0;
thread_local unsigned KnifeUpdateProbe::boundary_depth_ = 0;

bool MatchesKnifeArmsUpdateEvidence(std::span<const std::uint8_t> entry, std::span<const std::uint8_t> call) noexcept
{
    constexpr std::array<std::uint8_t, 12> expected_entry {
        0x48,0x8B,0xC4,0x48,0x89,0x68,0x20,0x56,0x48,0x83,0xEC,0x70 };
    constexpr std::array<std::uint8_t, 5> expected_call {0xE8,0x95,0,0,0};
    return std::ranges::equal(entry, expected_entry) && std::ranges::equal(call, expected_call);
}

bool MatchesKnifeUpdateCallEvidence(std::span<const std::uint8_t> entry,
    std::span<const std::uint8_t> call1, std::span<const std::uint8_t> call2,
    std::span<const std::uint8_t> evaluation) noexcept
{
    constexpr std::array<std::uint8_t, 19> e {
        0x40,0x55,0x53,0x56,0x57,0x41,0x57,0x48,0x8D,0x6C,0x24,0xC9,0x48,0x81,0xEC,0xA0,0,0,0 };
    constexpr std::array<std::uint8_t, 11> c1 {
        0x48,0x8B,0xD7,0x49,0x8B,0xCE,0xE8,0xD7,0xE9,0x83,0xFF };
    constexpr std::array<std::uint8_t, 11> c2 {
        0x48,0x8B,0xD7,0x49,0x8B,0xCE,0xE8,0xB3,0xE9,0x83,0xFF };
    constexpr std::array<std::uint8_t, 5> eval {0xE8,0xCC,0x67,0x2B,0xFF};
    return std::ranges::equal(entry, e) && std::ranges::equal(call1, c1) &&
        std::ranges::equal(call2, c2) && std::ranges::equal(evaluation, eval);
}

bool ShouldDispatchKnifeWeaponListener(const std::uintptr_t expected_controller,
    const std::uintptr_t expected_world, const std::uintptr_t listener_world,
    const std::uintptr_t actual_controller, const std::uintptr_t actual_world) noexcept
{
    if (!actual_controller || !actual_world) return false;
    // A published active binding is stronger than the pre-arm world filter:
    // never accept a different controller merely because it shares a world.
    if (expected_controller || expected_world)
        return expected_controller && expected_world &&
            actual_controller == expected_controller && actual_world == expected_world;
    return listener_world && actual_world == listener_world;
}

bool ShouldDispatchKnifeArmsListener(const std::uintptr_t listener_arms,
    const std::uintptr_t actual_arms) noexcept
{
    return listener_arms && actual_arms && listener_arms == actual_arms;
}

namespace
{
bool ImageEvidence(std::uintptr_t client) noexcept
{
    if (!client) { return false; }
    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(client);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) { return false; }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(client + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.SizeOfImage <= kKnifeNativeGraphEvaluationCallRva + 5) { return false; }
        return MatchesKnifeArmsUpdateEvidence({reinterpret_cast<const std::uint8_t*>(client + kKnifeArmsUpdateRva), 12},
            {reinterpret_cast<const std::uint8_t*>(client + kKnifeArmsUpdateCallRva), 5}) &&
            MatchesKnifeUpdateCallEvidence({reinterpret_cast<const std::uint8_t*>(client + kKnifeNativeUpdateRva), 19},
            {reinterpret_cast<const std::uint8_t*>(client + kKnifeNativeUpdateCall1Rva), 11},
            {reinterpret_cast<const std::uint8_t*>(client + kKnifeNativeUpdateCall2Rva), 11},
            {reinterpret_cast<const std::uint8_t*>(client + kKnifeNativeGraphEvaluationCallRva), 5});
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadSource(std::uintptr_t controller, std::uintptr_t graph, std::uintptr_t source) noexcept
{
    if (!controller || !graph || !source || controller > std::numeric_limits<std::uintptr_t>::max() - 0x138 ||
        graph > std::numeric_limits<std::uintptr_t>::max() - 0x20) { return false; }
    __try
    {
        if (*reinterpret_cast<const std::uintptr_t*>(controller + kKnifeWeaponTypeGraphOffset) != graph) { return false; }
        const auto index = *reinterpret_cast<const std::int16_t*>(controller + kKnifeWeaponTypeIndexOffset);
        const auto count = *reinterpret_cast<const std::int32_t*>(graph + 8);
        if (index < 0 || count < 1 || count > 32768 || index >= count) { return false; }
        const auto table = *reinterpret_cast<const std::uintptr_t*>(graph + 16);
        if (!table || table > std::numeric_limits<std::uintptr_t>::max() - static_cast<std::uintptr_t>(index) * 8) { return false; }
        const auto node = *reinterpret_cast<const std::uintptr_t*>(table + static_cast<std::uintptr_t>(index) * 8);
        return node && node <= std::numeric_limits<std::uintptr_t>::max() - 0x20 &&
            *reinterpret_cast<const std::uintptr_t*>(node + 24) == source &&
            *reinterpret_cast<const std::uintptr_t*>(controller + kKnifeWeaponTypeGraphOffset) == graph;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool KnifeUpdateProbe::Install(HMODULE client) noexcept
{
    if (stopped_.load(std::memory_order_acquire)) { return false; }
    if (Installed()) { return client_ == reinterpret_cast<std::uintptr_t>(client); }
    std::lock_guard lock(lifecycle_mutex_);
    if (stopped_.load(std::memory_order_acquire)) { return false; }
    if (Installed()) { return client_ == reinterpret_cast<std::uintptr_t>(client); }
    if (attempted_) { return false; }
    attempted_ = true;
    const auto base = reinterpret_cast<std::uintptr_t>(client);
    if (!ImageEvidence(base)) { return false; }
    KnifeUpdateProbe* expected = nullptr;
    if (!owner_.compare_exchange_strong(expected, this) && expected != this) { return false; }
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { return false; }
    void* original = nullptr;
    void* target = reinterpret_cast<void*>(base + kKnifeNativeUpdateRva);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook), &original) != MH_OK) { return false; }
    void* arms_original = nullptr;
    void* arms_target = reinterpret_cast<void*>(base + kKnifeArmsUpdateRva);
    if (MH_CreateHook(arms_target, reinterpret_cast<void*>(&ArmsHook), &arms_original) != MH_OK) return false;
    // Publish before enabling; the first callback must always have a trampoline.
    client_ = base;
    original_.store(reinterpret_cast<Update>(original), std::memory_order_release);
    target_ = target;
    arms_original_.store(reinterpret_cast<ArmsUpdate>(arms_original), std::memory_order_release);
    arms_target_ = arms_target;
    const auto arms_status = MH_EnableHook(arms_target_);
    const auto status = MH_EnableHook(target_);
    const bool ok = (arms_status == MH_OK || arms_status == MH_ERROR_ENABLED) && (status == MH_OK || status == MH_ERROR_ENABLED);
    if (!ok) { MH_DisableHook(target_); MH_DisableHook(arms_target_); }
    enabled_.store(ok, std::memory_order_release);
    return Installed();
}

void KnifeUpdateProbe::Publish(DWORD game_thread, std::uintptr_t controller, std::uintptr_t world,
    std::uintptr_t graph, std::uintptr_t source_symbol) noexcept
{
    generation_.fetch_add(1, std::memory_order_acq_rel);
    thread_.store(game_thread); controller_.store(controller); world_.store(world);
    graph_.store(graph); source_.store(source_symbol);
    generation_.fetch_add(1, std::memory_order_release);
}

void KnifeUpdateProbe::Clear() noexcept
{
    Publish(0, 0, 0, 0, 0);
    listener_arms_filter_.store(0, std::memory_order_release);
    listener_world_filter_.store(0, std::memory_order_release);
}

void KnifeUpdateProbe::Listen(const DWORD game_thread, Listener listener,
    const std::uintptr_t arms_filter, const std::uintptr_t world_filter) noexcept
{
    listener_arms_filter_.store(arms_filter, std::memory_order_release);
    listener_world_filter_.store(world_filter, std::memory_order_release);
    listener_thread_.store(game_thread, std::memory_order_release);
    listener_.store(listener, std::memory_order_release);
}

void KnifeUpdateProbe::Disable() noexcept
{
    stopped_.store(true, std::memory_order_release);
    std::lock_guard lock(lifecycle_mutex_);
    enabled_.store(false, std::memory_order_release);
    if (target_) { MH_DisableHook(target_); }
    if (arms_target_) { MH_DisableHook(arms_target_); }
    // Do NOT free the trampoline while a callback could still be returning from
    // the native updater. One bounded allocation lives until process exit; this
    // module has no hot-unload API. No per-player/seek allocation is retained.
}

void __fastcall KnifeUpdateProbe::Hook(void* controller, void* world)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto* self = owner_.load(std::memory_order_acquire);
    // Owner/original are published before MH_EnableHook and never cleared.
    const auto original = self->original_.load(std::memory_order_acquire);
    const auto listener = self->listener_.load(std::memory_order_acquire);
    const auto expected_controller = self->controller_.load(std::memory_order_acquire);
    const auto expected_world = self->world_.load(std::memory_order_acquire);
    const auto listener_world = self->listener_world_filter_.load(std::memory_order_acquire);
    const bool native_scope = self->Installed() && boundary_arms_ && listener &&
        self->listener_thread_.load() == GetCurrentThreadId() &&
        (caller == self->client_ + kKnifeNativeUpdateCall1Rva + 11 ||
         caller == self->client_ + kKnifeNativeUpdateCall2Rva + 11);
    const bool has_target_filter =
        (expected_controller && expected_world) || listener_world;
    if (native_scope && !has_target_filter)
    {
        ++self->unfiltered_weapon_updates_;
        self->last_unfiltered_arms_.store(boundary_arms_, std::memory_order_relaxed);
        self->last_unfiltered_controller_.store(
            reinterpret_cast<std::uintptr_t>(controller),
            std::memory_order_relaxed);
        self->last_unfiltered_world_.store(
            reinterpret_cast<std::uintptr_t>(world),
            std::memory_order_relaxed);
    }
    const bool dispatch = native_scope &&
        ShouldDispatchKnifeWeaponListener(expected_controller, expected_world, listener_world,
            reinterpret_cast<std::uintptr_t>(controller), reinterpret_cast<std::uintptr_t>(world));
    // The pre-armed world definition/model is already the target before this
    // native update starts. Give the scoped listener one verified boundary to
    // pair the cached HUD skeleton and weapon_type before Source 2 selects the
    // first deploy sequence. The post event remains the readback/maintenance
    // boundary and preserves the old fallback when pre-validation is not ready.
    if (dispatch)
    {
        ++self->nested_updates_;
        listener({KnifeUpdatePhase::BeforeWeapon, boundary_arms_,
            reinterpret_cast<std::uintptr_t>(controller), reinterpret_cast<std::uintptr_t>(world)});
    }
    original(controller, world);
    if (self->controller_.load(std::memory_order_relaxed))
        self->Sample(controller, world, caller);
    if (dispatch && self->Installed())
    {
        ++self->nested_updates_;
        listener({KnifeUpdatePhase::AfterWeapon, boundary_arms_, reinterpret_cast<std::uintptr_t>(controller),
            reinterpret_cast<std::uintptr_t>(world)});
    }
}

void __fastcall KnifeUpdateProbe::ArmsHook(void* arms, bool force)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    auto* self = owner_.load(std::memory_order_acquire);
    const auto original = self->arms_original_.load(std::memory_order_acquire);
    const auto listener = self->listener_.load(std::memory_order_acquire);
    const auto previous = boundary_arms_;
    const auto active_controller = self->controller_.load(std::memory_order_acquire);
    const auto active_world = self->world_.load(std::memory_order_acquire);
    const auto listener_world = self->listener_world_filter_.load(std::memory_order_acquire);
    const bool has_listener_target =
        (active_controller && active_world) || listener_world;
    const bool native_scope = boundary_depth_ == 0 && self->Installed() && listener && arms &&
        self->listener_thread_.load() == GetCurrentThreadId() && caller == self->client_ + kKnifeArmsUpdateCallRva + 5;
    const bool scoped = native_scope && has_listener_target;
    const auto listener_arms = self->listener_arms_filter_.load(std::memory_order_acquire);
    // A holstered pre-arm lease does not know the future arms identity yet.
    // Let the outer BeforeArms boundary reach the listener when an exact world
    // lease exists; the callback must still prove Pawn, active handle, arms,
    // HUD child and graph before it can authorize a write. Published active
    // bindings continue to require their exact arms filter.
    const bool dispatch_arms = scoped &&
        (ShouldDispatchKnifeArmsListener(listener_arms, reinterpret_cast<std::uintptr_t>(arms)) ||
         (!listener_arms && listener_world));
    boundary_arms_ = native_scope ? reinterpret_cast<std::uintptr_t>(arms) : 0;
    ++boundary_depth_;
    if (scoped) ++self->boundaries_;
    if (native_scope && !has_listener_target)
    {
        ++self->unfiltered_boundaries_;
        self->last_unfiltered_arms_.store(
            reinterpret_cast<std::uintptr_t>(arms),
            std::memory_order_relaxed);
    }
    if (dispatch_arms) listener({KnifeUpdatePhase::BeforeArms, boundary_arms_});
    original(arms, force);
    if (dispatch_arms && self->Installed()) listener({KnifeUpdatePhase::AfterArms, boundary_arms_});
    boundary_arms_ = previous;
    --boundary_depth_;
}

void KnifeUpdateProbe::Sample(void* controller, void* world, std::uintptr_t caller) noexcept
{
    if (!Installed()) { return; }
    const auto generation = generation_.load(std::memory_order_acquire);
    if (generation & 1) { return; }
    const auto expected_controller = controller_.load();
    const auto expected_world = world_.load();
    const auto graph = graph_.load();
    const auto source = source_.load();
    const auto game_thread = thread_.load();
    if (!expected_controller || reinterpret_cast<std::uintptr_t>(controller) != expected_controller ||
        reinterpret_cast<std::uintptr_t>(world) != expected_world ||
        generation != generation_.load(std::memory_order_acquire)) { return; }
    ++matched_;
    const bool first = caller == client_ + kKnifeNativeUpdateCall1Rva + 11;
    const bool second = caller == client_ + kKnifeNativeUpdateCall2Rva + 11;
    if (first) { ++call1_; }
    if (second) { ++call2_; }
    const bool correct_thread = game_thread && game_thread == GetCurrentThreadId();
    if (correct_thread) { ++same_thread_; }
    if ((first || second) && correct_thread && ReadSource(expected_controller, graph, source)) { ++source_verified_; }
    else { ++rejected_; }
}

KnifeUpdateTiming KnifeUpdateProbe::Snapshot() const noexcept
{
    return {matched_.load(), call1_.load(), call2_.load(), same_thread_.load(), source_verified_.load(), rejected_.load(),
        boundaries_.load(), nested_updates_.load(),
        unfiltered_boundaries_.load(), unfiltered_weapon_updates_.load(),
        last_unfiltered_arms_.load(), last_unfiltered_controller_.load(), last_unfiltered_world_.load()};
}
}
