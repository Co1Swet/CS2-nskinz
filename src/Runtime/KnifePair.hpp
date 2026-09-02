#pragma once

#include "Runtime/KnifeModel.hpp"

namespace nskinzcs2::runtime
{
enum class KnifeBladeRole { World, Hud }; // No arms/glove setter exists here.

// Borrowed identities, NOT owning resource handles. Restore resolves immutable
// asset paths through the engine's SetModel, never stores these pointers back.
struct KnifePairState
{
    // Match the live MODEL PATH to the canonical asset table independently
    // from the transaction-owned item definition recorded below.
    int world_definition = 0, hud_definition = 0;
    std::uintptr_t world_model = 0, hud_model = 0, arms_model = 0;
    std::uintptr_t animation_type = 0;
    bool secondary_matches_hud = false;
    bool attachments_valid = false;
    // The live world CEconItemView definition is part of the model lease. It
    // selects the native animation/VData branch and the weapon HUD identity.
    // Paint attributes and material owners deliberately remain outside it.
    int item_definition = 0;
    bool operator==(const KnifePairState&) const = default;
};

// The platform port must use the verified native string SetModel wrapper and
// explicit secondary-skeleton recollector. Same-slot re-registration is not a
// recollection. Every method revalidates serials, scene epoch and graph identity.
// SafePoint means after THIS knife's native parameter update, before graph
// evaluation, on the verified game thread; a timer/render poll is not sufficient.
class KnifePairAccess
{
public:
    virtual ~KnifePairAccess() = default;
    virtual bool Alive(const KnifeModelBinding&) noexcept = 0;
    virtual bool SafePoint(const KnifeModelBinding&) noexcept = 0;
    virtual bool Read(const KnifeModelBinding&, KnifePairState&) noexcept = 0;
    virtual std::uintptr_t ExistingAnimationType(const KnifeModelAsset&) noexcept = 0;
    virtual bool ReadItemDefinition(const KnifeModelBinding&, int&) noexcept = 0;
    virtual bool CompareItemDefinition(const KnifeModelBinding&, int expected, int desired) noexcept = 0;
    virtual bool SetBladeModel(const KnifeModelBinding&, KnifeBladeRole, const KnifeModelAsset&) noexcept = 0;
    virtual bool Recollect(const KnifeModelBinding&) noexcept = 0;
    // Re-probe the ID node and compare expected before changing it. No stale
    // node address, string allocation, shared VData or fixed sequence writes.
    // Return true only after verifying the resulting value on the same node.
    virtual bool CompareAnimationType(const KnifeModelBinding&, std::uintptr_t expected,
        std::uintptr_t desired) noexcept = 0;
    virtual bool Maintain(const KnifeModelBinding&, const KnifePairState& committed,
        std::uintptr_t source, std::uintptr_t target) noexcept;
};

// Concrete paired transaction; portable for fault-injection tests. This does not
// grant an unverified native port permission to mutate a game process.
class KnifePairAdapter final : public KnifeModelAdapter
{
public:
    explicit KnifePairAdapter(KnifePairAccess& access) noexcept : access_(access) {}
    bool BindingAlive(const KnifeModelBinding&) noexcept override;
    bool Capture(const KnifeModelBinding&) noexcept override;
    bool ApplyPair(const KnifeModelBinding&, const KnifeModelAsset&, KnifeModelBinding& after) noexcept override;
    // CS2's world-model notification queues the old HUD for deletion. Production
    // must wait for its successor instead of committing that doomed entity.
    bool PrepareWorld(const KnifeModelBinding&, const KnifeModelAsset&) noexcept;
    bool CompletePreparedWorld(const KnifeModelBinding& replacement, const KnifeModelAsset&) noexcept;
    bool VerifyPair(const KnifeModelBinding&, const KnifeModelAsset&) noexcept override;
    bool Restore(const KnifeModelBinding&) noexcept override;
    void Abandon() noexcept override;

    // Invoked ONLY after the native weapon updater resets weapon_type. Steady
    // animation maintenance must never SetModel or recollect resources per frame.
    // Request/binding drift latches maintenance off until the owner retires it.
    bool AfterNativeUpdate(const KnifeModelRequest&, const KnifeModelBinding&) noexcept;
    bool Captured() const noexcept { return captured_; }
    bool MaintenanceFaulted() const noexcept { return maintenance_faulted_; }

private:
    bool ReadOwned(KnifePairState&) noexcept;
    bool PairReady(const KnifePairState&, int definition, std::uintptr_t symbol) const noexcept;
    bool CanMutate() noexcept;
    bool FinishHud() noexcept;
    KnifePairAccess& access_;
    KnifeModelBinding binding_;
    KnifePairState original_, committed_;
    const KnifeModelAsset* target_ = nullptr;
    std::uintptr_t target_symbol_ = 0;
    bool captured_ = false, attempted_ = false, applied_ = false;
    bool maintenance_faulted_ = false;
    bool world_prepared_ = false;
};
}
