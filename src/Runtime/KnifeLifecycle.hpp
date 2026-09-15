#pragma once
#include "Runtime/KnifeNative.hpp"
#include "Runtime/KnifeUpdateProbe.hpp"
#include <optional>

namespace nskinzcs2::runtime
{
bool ShouldWaitForHolsteredKnifePrearm(bool native_read, bool prearmed_handoff,
    bool has_transaction, int source_definition, int target_definition,
    bool serialization_bridge_ready = false) noexcept;

// Cosmetic-only refreshes must not roll back a still-owned model lease.
// Scene changes and changed/disabled model requests always retire normally.
bool ShouldPreserveKnifePrearmOnRefresh(bool cosmetic_refresh, bool owned_same_scene,
    KnifeModelRequest leased, KnifeModelRequest requested) noexcept;
bool ShouldKeepKnifeUpdateListener(bool request_enabled, bool offline_demo,
    bool seeking, bool authorized, bool native_read, bool has_transaction,
    bool prearm_active) noexcept;

struct KnifeLifecycleStatistics
{
    std::uint64_t attempts = 0, applied = 0, restored = 0, discarded = 0, faults = 0, maintenance = 0;
    std::uint64_t world_prepared = 0, hud_handoffs = 0, handoff_retries = 0;
    std::uint64_t prearmed_adoptions = 0;
    bool operator==(const KnifeLifecycleStatistics&) const = default;
};

// One game-thread-owned session. No cosmetics/config revision is part of its
// identity. The host mints a lease only for the dynamic native callback scope.
class KnifeLifecycle
{
public:
    explicit KnifeLifecycle(KnifeNativeHost& host) noexcept : host_(host) {}
    void Request(KnifeModelRequest request) noexcept;
    void Stage(KnifeModelRequest, const KnifeNativeContext*, std::uint64_t now) noexcept;
    void OnUpdate(const KnifeUpdateEvent&, std::uint64_t now) noexcept;
    bool Owns(const KnifeModelRequest&, const KnifeModelBinding&) const noexcept;
    bool PreparingWorld(const KnifeModelRequest&, const KnifeModelBinding&) const noexcept;
    bool HasTransaction() const noexcept { return access_.has_value(); }
    // A demo seek/full update destroys the Pawn/world/HUD generation outside
    // the native update boundary. Discard every pointer-bearing lease and
    // per-binding failure latch without writing to the retiring entities.
    void ResetForEntityRebuild() noexcept;
    const KnifeModelRequest& Requested() const noexcept { return wanted_; }
    const KnifeNativeContext* Active() const noexcept { return active_ ? &*active_ : nullptr; }
    KnifeModelPhase Phase() const noexcept { return phase_; }
    const KnifeLifecycleStatistics& Statistics() const noexcept { return statistics_; }
private:
    bool CurrentRequest() const noexcept;
    bool Retire() noexcept;
    void ClearTransaction() noexcept;
    KnifeNativeHost& host_;
    KnifeModelRequest wanted_;
    std::optional<KnifeNativeContext> staged_, active_, failed_, handoff_retry_;
    std::optional<KnifeNativePairAccess> access_;
    std::optional<KnifePairAdapter> pair_;
    std::uint64_t staged_at_ = 0, next_verify_ = 0, prepared_at_ = 0, handoff_retry_after_ = 0;
    std::uint32_t prepared_updates_ = 0, handoff_retry_count_ = 0;
    bool pending_retire_ = false, retired_in_boundary_ = false, waiting_hud_ = false, failed_world_ = false;
    KnifeModelPhase phase_ = KnifeModelPhase::Disabled;
    KnifeLifecycleStatistics statistics_;
};
}
