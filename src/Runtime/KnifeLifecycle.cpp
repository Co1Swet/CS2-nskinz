#include "Runtime/KnifeLifecycle.hpp"

namespace nskinzcs2::runtime
{
bool ShouldWaitForHolsteredKnifePrearm(const bool native_read, const bool prearmed_handoff,
    const bool has_transaction, const int source_definition, const int target_definition,
    const bool serialization_bridge_ready) noexcept
{
    return native_read && !prearmed_handoff && !has_transaction && !serialization_bridge_ready &&
        source_definition != 0 && target_definition != 0 &&
        source_definition != target_definition;
}

bool ShouldPreserveKnifePrearmOnRefresh(const bool cosmetic_refresh, const bool owned_same_scene,
    const KnifeModelRequest leased, const KnifeModelRequest requested) noexcept
{
    return cosmetic_refresh && owned_same_scene && leased.Enabled() && leased == requested &&
        FindKnifeModelAsset(leased.definition) != nullptr;
}

bool ShouldKeepKnifeUpdateListener(const bool request_enabled, const bool offline_demo,
    const bool seeking, const bool authorized, const bool native_read,
    const bool has_transaction, const bool prearm_active) noexcept
{
    // A render-pass probe can briefly lose the replacement HUD while Source 2
    // rebuilds it.  The already-published callback filter is still exact (full
    // world/controller/arms identities), so keep it alive for an owned pair or
    // a holstered pre-arm. Clearing it in that transient gap lets the native
    // updater publish the recorded T/CT weapon_type immediately before graph
    // evaluation, which is the intermittent source-knife animation leak.
    return request_enabled && offline_demo && !seeking && authorized &&
        (native_read || has_transaction || prearm_active);
}

namespace
{
bool Same(const KnifeNativeContext& a, const KnifeNativeContext& b) noexcept
{
    return a.binding == b.binding && a.source_definition == b.source_definition && a.target_definition == b.target_definition &&
        a.resources.world == b.resources.world && a.resources.hud == b.resources.hud && a.resources.arms == b.resources.arms &&
        a.model_names == b.model_names && a.hud_body_controller == b.hud_body_controller && a.slot_symbol == b.slot_symbol &&
        a.symbols.weapon_type_name == b.symbols.weapon_type_name && a.symbols.source_type == b.symbols.source_type &&
        a.symbols.target_type == b.symbols.target_type && a.world_prearmed == b.world_prearmed &&
        a.client == b.client && a.client_size == b.client_size;
}
bool Matches(const KnifeUpdateEvent& event, const KnifeModelBinding& b) noexcept
{
    if (event.phase == KnifeUpdatePhase::BeforeArms || event.phase == KnifeUpdatePhase::AfterArms)
        return event.arms == b.arms.address;
    return event.arms == b.arms.address && event.controller == b.weapon_graph_controller &&
        event.world == b.world.address;
}
bool SameWorld(const KnifeModelBinding& a, const KnifeModelBinding& b) noexcept
{
    return a.scene_epoch == b.scene_epoch && a.xuid == b.xuid && a.pawn == b.pawn && a.world == b.world;
}
}

void KnifeLifecycle::Request(KnifeModelRequest request) noexcept
{
    if (wanted_ != request)
    {
        wanted_ = request; staged_.reset();
        handoff_retry_.reset(); handoff_retry_count_ = 0; handoff_retry_after_ = 0;
        if (!access_) phase_ = request.Enabled() ? KnifeModelPhase::WaitingBinding : KnifeModelPhase::Disabled;
    }
}

void KnifeLifecycle::Stage(KnifeModelRequest request, const KnifeNativeContext* context, std::uint64_t now) noexcept
{
    Request(request);
    if (context && request.Enabled() && FindKnifeModelAsset(request.definition) &&
        context->binding.xuid == request.xuid && context->target_definition == request.definition)
    {
        if (handoff_retry_ && (!SameWorld(handoff_retry_->binding, context->binding) ||
            handoff_retry_->source_definition != context->source_definition ||
            handoff_retry_->target_definition != context->target_definition))
        { handoff_retry_.reset(); handoff_retry_count_ = 0; handoff_retry_after_ = 0; }
        staged_ = *context; staged_at_ = now;
    }
    else staged_.reset();
}

bool KnifeLifecycle::CurrentRequest() const noexcept
{
    return active_ && wanted_.Enabled() && wanted_.xuid == active_->binding.xuid &&
        wanted_.definition == active_->target_definition && (waiting_hud_ || !staged_ || Same(*active_, *staged_));
}

bool KnifeLifecycle::Owns(const KnifeModelRequest& request, const KnifeModelBinding& binding) const noexcept
{
    return access_ && active_ && !pending_retire_ && phase_ == KnifeModelPhase::Applied &&
        wanted_ == request && active_->binding == binding && request.definition == active_->target_definition;
}

bool KnifeLifecycle::PreparingWorld(const KnifeModelRequest& request, const KnifeModelBinding& binding) const noexcept
{
    return waiting_hud_ && active_ && !pending_retire_ && wanted_ == request && request.definition == active_->target_definition &&
        SameWorld(active_->binding, binding) && active_->binding.arms == binding.arms &&
        active_->binding.weapon_graph_controller == binding.weapon_graph_controller &&
        active_->binding.body_animation_controller == binding.body_animation_controller;
}

void KnifeLifecycle::ClearTransaction() noexcept
{
    if (pair_) pair_->Abandon();
    pair_.reset(); access_.reset(); active_.reset(); pending_retire_ = waiting_hud_ = false;
    prepared_updates_ = 0;
}

void KnifeLifecycle::ResetForEntityRebuild() noexcept
{
    ClearTransaction();
    staged_.reset();
    failed_.reset();
    handoff_retry_.reset();
    staged_at_ = 0;
    next_verify_ = 0;
    prepared_at_ = 0;
    handoff_retry_after_ = 0;
    prepared_updates_ = 0;
    handoff_retry_count_ = 0;
    pending_retire_ = false;
    retired_in_boundary_ = false;
    waiting_hud_ = false;
    failed_world_ = false;
    phase_ = wanted_.Enabled() ? KnifeModelPhase::WaitingBinding : KnifeModelPhase::Disabled;
}

bool KnifeLifecycle::Retire() noexcept
{
    if (!access_) return true;
    const auto result = access_->RetireDetached();
    if (result == KnifeRetireResult::Waiting) { phase_ = KnifeModelPhase::WaitingSafePoint; return false; }
    if (result == KnifeRetireResult::Failed)
    {
        if (phase_ != KnifeModelPhase::Faulted) ++statistics_.faults;
        phase_ = KnifeModelPhase::Faulted;
        // Keep the ledger to prevent a new target from hiding unresolved cleanup.
        // The port latches native retries off; a new scene epoch can discard it.
        pending_retire_ = true; return false;
    }
    if (result == KnifeRetireResult::Restored) ++statistics_.restored;
    else ++statistics_.discarded;
    retired_in_boundary_ = true;
    ClearTransaction();
    phase_ = wanted_.Enabled() ? KnifeModelPhase::WaitingBinding : KnifeModelPhase::Disabled;
    return true;
}

void KnifeLifecycle::OnUpdate(const KnifeUpdateEvent& event, std::uint64_t now) noexcept
{
    if (event.phase == KnifeUpdatePhase::BeforeArms || event.phase == KnifeUpdatePhase::AfterArms)
    {
        if (event.phase == KnifeUpdatePhase::BeforeArms)
        {
            retired_in_boundary_ = false;
            if (access_ && (pending_retire_ || !CurrentRequest() ||
                (waiting_hud_ ? !host_.WorldBindingAlive(*active_) : !access_->Alive(active_->binding))))
            { pending_retire_ = true; Retire(); }
        }
        // A holstered knife is already the target world model/definition.  Its
        // newly created HUD and graph are adopted either from the nested weapon
        // callback or immediately after the outer arms updater rebuilt them.
        // BeforeArms can still expose the outgoing pistol HUD/graph.  Never
        // adopt a target knife pair there.  The nested BeforeWeapon boundary
        // has the exact knife controller/world arguments; AfterArms remains a
        // strict readback fallback for a natively rebuilt target pair.
        if (event.phase == KnifeUpdatePhase::BeforeArms ||
            access_ || !staged_ || !staged_->world_prearmed) return;
    }
    else
    {
        if (event.phase != KnifeUpdatePhase::BeforeWeapon && event.phase != KnifeUpdatePhase::AfterWeapon) return;
        // Only a fully pre-armed world may commit before the native weapon
        // update. Ordinary model changes retain the conservative AfterWeapon
        // boundary.
        if (event.phase == KnifeUpdatePhase::BeforeWeapon &&
            (access_ || !staged_ || !staged_->world_prearmed)) return;
    }
    if (access_)
    {
        if (pending_retire_ || !CurrentRequest()) { pending_retire_ = true; Retire(); return; }
        if (waiting_hud_)
        {
            if (Matches(event, active_->binding)) ++prepared_updates_;
            const bool fresh = staged_ && now >= staged_at_ && now - staged_at_ <= 1000;
            if (fresh && PreparingWorld(wanted_, staged_->binding) &&
                staged_->binding.hud_weapon != active_->binding.hud_weapon && Matches(event, staged_->binding) &&
                access_->RebindPreparedHud(*staged_))
            {
                active_ = *staged_;
                const auto& target = *FindKnifeModelAsset(active_->target_definition);
                if (!pair_->CompletePreparedWorld(active_->binding, target) || !pair_->VerifyPair(active_->binding, target))
                {
                    failed_ = *active_; failed_world_ = true; pending_retire_ = true; ++statistics_.faults;
                    Retire(); phase_ = KnifeModelPhase::Faulted; return;
                }
                waiting_hud_ = false; ++statistics_.hud_handoffs; ++statistics_.applied;
                handoff_retry_.reset(); handoff_retry_count_ = 0; handoff_retry_after_ = 0;
                next_verify_ = now + 500; phase_ = KnifeModelPhase::Applied;
                return;
            }
            // Native deletion is deferred; the old HUD may look valid for a few
            // callbacks. Never finish on it, nor restore/reload the world each frame.
            // Demo pause/slow-motion can advance wall time while no native
            // weapon updates occur. Require enough matching update boundaries
            // before declaring the successor missing. A bounded retry handles
            // a HUD that became visible just after the last render-pass poll.
            if (now >= prepared_at_ && now - prepared_at_ > 3000 && prepared_updates_ >= 32)
            {
                const auto timed_out = *active_;
                if (!handoff_retry_ || !SameWorld(handoff_retry_->binding, timed_out.binding) ||
                    handoff_retry_->source_definition != timed_out.source_definition ||
                    handoff_retry_->target_definition != timed_out.target_definition)
                { handoff_retry_ = timed_out; handoff_retry_count_ = 0; }
                ++handoff_retry_count_;
                const bool recoverable = handoff_retry_count_ <= 2;
                if (!recoverable) { failed_ = timed_out; failed_world_ = true; }
                pending_retire_ = true;
                if (!Retire()) return;
                if (recoverable)
                {
                    ++statistics_.handoff_retries;
                    failed_.reset(); failed_world_ = false; handoff_retry_after_ = now + 250;
                    phase_ = KnifeModelPhase::WaitingBinding;
                }
                else { ++statistics_.faults; phase_ = KnifeModelPhase::Faulted; }
            }
            return;
        }
        if (!Matches(event, active_->binding)) return;
        const auto request = KnifeModelRequest {active_->binding.xuid, active_->target_definition};
        const bool maintained = pair_->AfterNativeUpdate(request, active_->binding);
        if (maintained) ++statistics_.maintenance;
        const bool verified = maintained && (now < next_verify_ || pair_->VerifyPair(active_->binding, *FindKnifeModelAsset(request.definition)));
        if (!verified)
        {
            failed_ = *active_; failed_world_ = false; pending_retire_ = true; ++statistics_.faults;
            Retire(); phase_ = KnifeModelPhase::Faulted;
        }
        else { if (now >= next_verify_) next_verify_ = now + 500; phase_ = KnifeModelPhase::Applied; }
        return;
    }
    if (retired_in_boundary_ || !staged_ || !wanted_.Enabled() || now < staged_at_ ||
        now - staged_at_ > 1000 || now < handoff_retry_after_ || !Matches(event, staged_->binding)) return;
    if (failed_ && (Same(*failed_, *staged_) || (failed_world_ && SameWorld(failed_->binding, staged_->binding) &&
        failed_->target_definition == staged_->target_definition))) { phase_ = KnifeModelPhase::Faulted; return; }
    if (staged_->source_definition == staged_->target_definition && !staged_->world_prearmed)
    { phase_ = KnifeModelPhase::NativeModel; return; }
    if (!host_.AtSafePoint(staged_->binding) || !host_.BindingAlive(*staged_) || !host_.WorldBindingAlive(*staged_)) return;
    active_ = *staged_;
    access_.emplace(*active_, host_);
    pair_.emplace(*access_);
    if (!access_->EvidenceVerified())
    { failed_ = *active_; failed_world_ = false; ClearTransaction(); ++statistics_.faults; phase_ = KnifeModelPhase::Faulted; return; }
    ++statistics_.attempts;
    if (active_->world_prearmed)
    {
        const auto* source = FindKnifeModelAsset(active_->source_definition);
        const auto* target = FindKnifeModelAsset(active_->target_definition);
        if (source == nullptr || target == nullptr ||
            !pair_->AdoptPrearmedWorld(active_->binding, *source, *target) ||
            !pair_->VerifyPair(active_->binding, *target))
        {
            failed_ = *active_; failed_world_ = true; pending_retire_ = pair_->Captured();
            ++statistics_.faults;
            if (pending_retire_) Retire(); else ClearTransaction();
            phase_ = KnifeModelPhase::Faulted;
            return;
        }
        ++statistics_.prearmed_adoptions;
        ++statistics_.applied;
        next_verify_ = now + 500;
        phase_ = KnifeModelPhase::Applied;
        return;
    }
    if (!pair_->Capture(active_->binding))
    { failed_ = *active_; failed_world_ = false; ClearTransaction(); ++statistics_.faults; phase_ = KnifeModelPhase::Faulted; return; }
    const auto& target = *FindKnifeModelAsset(active_->target_definition);
    if (!pair_->PrepareWorld(active_->binding, target))
    {
        failed_ = *active_; failed_world_ = true; pending_retire_ = true; ++statistics_.faults;
        Retire(); phase_ = KnifeModelPhase::Faulted; return;
    }
    ++statistics_.world_prepared;
    waiting_hud_ = true; prepared_at_ = now; prepared_updates_ = 0;
    phase_ = KnifeModelPhase::WaitingHudRebuild;
}
}
