#pragma once

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

namespace nskinzcs2::runtime
{
inline constexpr std::uintptr_t kKnifeNativeUpdateRva = 0x8E5390;
inline constexpr std::uintptr_t kKnifeNativeUpdateCall1Rva = 0x104E40E;
inline constexpr std::uintptr_t kKnifeNativeUpdateCall2Rva = 0x104E432;
inline constexpr std::uintptr_t kKnifeNativeGraphEvaluationCallRva = 0x104E4CF;
inline constexpr std::uintptr_t kKnifeArmsUpdateRva = 0x104E310;
inline constexpr std::uintptr_t kKnifeArmsUpdateCallRva = 0x104E282;
bool MatchesKnifeArmsUpdateEvidence(std::span<const std::uint8_t> entry, std::span<const std::uint8_t> call) noexcept;
bool MatchesKnifeUpdateCallEvidence(std::span<const std::uint8_t> entry,
    std::span<const std::uint8_t> call1, std::span<const std::uint8_t> call2,
    std::span<const std::uint8_t> evaluation) noexcept;

struct KnifeUpdateTiming
{
    std::uint64_t matched = 0, call1 = 0, call2 = 0, same_thread = 0;
    std::uint64_t source_verified = 0, rejected = 0;
    std::uint64_t boundaries = 0, nested_updates = 0;
    bool operator==(const KnifeUpdateTiming&) const = default;
};

enum class KnifeUpdatePhase { BeforeArms, AfterWeapon, AfterArms };
struct KnifeUpdateEvent
{
    KnifeUpdatePhase phase;
    std::uintptr_t arms = 0, controller = 0, world = 0;
};

// Forwards native arguments unchanged and exposes a verified dynamic scope.
// The listener owns lifecycle/authorization; timing counts do not grant writes.
class KnifeUpdateProbe
{
public:
    bool Install(HMODULE client) noexcept;
    using Listener = void(*)(const KnifeUpdateEvent&) noexcept;
    void Listen(DWORD game_thread, Listener listener) noexcept;
    void Publish(DWORD game_thread, std::uintptr_t controller, std::uintptr_t world,
        std::uintptr_t graph, std::uintptr_t source_symbol) noexcept;
    void Clear() noexcept;
    void Disable() noexcept;
    KnifeUpdateTiming Snapshot() const noexcept;
    bool Installed() const noexcept { return enabled_.load(std::memory_order_acquire); }

private:
    using Update = void(__fastcall*)(void*, void*);
    using ArmsUpdate = void(__fastcall*)(void*, bool);
    static void __fastcall Hook(void* controller, void* world);
    static void __fastcall ArmsHook(void* arms, bool force);
    void Sample(void* controller, void* world, std::uintptr_t caller) noexcept;
    static std::atomic<KnifeUpdateProbe*> owner_;
    std::mutex lifecycle_mutex_; // Install/stop only; never acquired by Hook.
    std::atomic<Update> original_ {nullptr};
    std::atomic<ArmsUpdate> arms_original_ {nullptr};
    std::atomic<Listener> listener_ {nullptr};
    std::atomic<DWORD> listener_thread_ {0};
    static thread_local std::uintptr_t boundary_arms_;
    static thread_local unsigned boundary_depth_;
    std::uintptr_t client_ = 0;
    void* target_ = nullptr;
    void* arms_target_ = nullptr;
    bool attempted_ = false; // Game-thread installation, not retried on failure.
    std::atomic_bool enabled_ {false};
    std::atomic_bool stopped_ {false};
    std::atomic<std::uint64_t> generation_ {0};
    std::atomic<DWORD> thread_ {0};
    std::atomic<std::uintptr_t> controller_ {0}, world_ {0}, graph_ {0}, source_ {0};
    std::atomic<std::uint64_t> matched_ {0}, call1_ {0}, call2_ {0}, same_thread_ {0}, source_verified_ {0}, rejected_ {0};
    std::atomic<std::uint64_t> boundaries_ {0}, nested_updates_ {0};
};
}
