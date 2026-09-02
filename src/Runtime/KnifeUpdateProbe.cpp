#include "Runtime/KnifeUpdateProbe.hpp"
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
    constexpr std::array<std::uint8_t, 5> expected_call {0xE8,0x89,0,0,0};
    return std::ranges::equal(entry, expected_entry) && std::ranges::equal(call, expected_call);
}

bool MatchesKnifeUpdateCallEvidence(std::span<const std::uint8_t> entry,
    std::span<const std::uint8_t> call1, std::span<const std::uint8_t> call2,
    std::span<const std::uint8_t> evaluation) noexcept
{
    constexpr std::array<std::uint8_t, 19> e {
        0x40,0x55,0x53,0x56,0x57,0x41,0x57,0x48,0x8D,0x6C,0x24,0xC9,0x48,0x81,0xEC,0xA0,0,0,0 };
    constexpr std::array<std::uint8_t, 11> c1 {
        0x48,0x8B,0xD7,0x49,0x8B,0xCE,0xE8,0x77,0x6F,0x89,0xFF };
    constexpr std::array<std::uint8_t, 11> c2 {
        0x48,0x8B,0xD7,0x49,0x8B,0xCE,0xE8,0x53,0x6F,0x89,0xFF };
    constexpr std::array<std::uint8_t, 5> eval {0xE8,0xCC,0xC8,0x32,0xFF};
    return std::ranges::equal(entry, e) && std::ranges::equal(call1, c1) &&
        std::ranges::equal(call2, c2) && std::ranges::equal(evaluation, eval);
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
    if (!controller || !graph || !source || controller > std::numeric_limits<std::uintptr_t>::max() - 0x100 ||
        graph > std::numeric_limits<std::uintptr_t>::max() - 0x20) { return false; }
    __try
    {
        if (*reinterpret_cast<const std::uintptr_t*>(controller + 0xF0) != graph) { return false; }
        const auto index = *reinterpret_cast<const std::int16_t*>(controller + 0xF8);
        const auto count = *reinterpret_cast<const std::int32_t*>(graph + 8);
        if (index < 0 || count < 1 || count > 32768 || index >= count) { return false; }
        const auto table = *reinterpret_cast<const std::uintptr_t*>(graph + 16);
        if (!table || table > std::numeric_limits<std::uintptr_t>::max() - static_cast<std::uintptr_t>(index) * 8) { return false; }
        const auto node = *reinterpret_cast<const std::uintptr_t*>(table + static_cast<std::uintptr_t>(index) * 8);
        return node && node <= std::numeric_limits<std::uintptr_t>::max() - 0x20 &&
            *reinterpret_cast<const std::uintptr_t*>(node + 24) == source &&
            *reinterpret_cast<const std::uintptr_t*>(controller + 0xF0) == graph;
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

void KnifeUpdateProbe::Clear() noexcept { Publish(0, 0, 0, 0, 0); }

void KnifeUpdateProbe::Listen(DWORD game_thread, Listener listener) noexcept
{
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
    original(controller, world);
    self->Sample(controller, world, caller);
    const auto listener = self->listener_.load(std::memory_order_acquire);
    if (self->Installed() && boundary_arms_ && listener && self->listener_thread_.load() == GetCurrentThreadId() &&
        (caller == self->client_ + kKnifeNativeUpdateCall1Rva + 11 || caller == self->client_ + kKnifeNativeUpdateCall2Rva + 11))
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
    const bool scoped = boundary_depth_ == 0 && self->Installed() && listener && arms &&
        self->listener_thread_.load() == GetCurrentThreadId() && caller == self->client_ + kKnifeArmsUpdateCallRva + 5;
    boundary_arms_ = scoped ? reinterpret_cast<std::uintptr_t>(arms) : 0;
    ++boundary_depth_;
    if (scoped) { ++self->boundaries_; listener({KnifeUpdatePhase::BeforeArms, boundary_arms_}); }
    original(arms, force);
    if (scoped && self->Installed()) listener({KnifeUpdatePhase::AfterArms, boundary_arms_});
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
        boundaries_.load(), nested_updates_.load()};
}
}
