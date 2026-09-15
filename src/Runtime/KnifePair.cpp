#include "Runtime/KnifePair.hpp"

namespace nskinzcs2::runtime
{
namespace
{
bool Valid(const KnifePairState& state) noexcept
{
    return FindKnifeModelAsset(state.world_definition) && FindKnifeModelAsset(state.hud_definition) &&
        FindKnifeModelAsset(state.item_definition) &&
        state.world_model && state.hud_model && state.arms_model && state.animation_type &&
        state.arms_model != state.world_model && state.arms_model != state.hud_model;
}
}

bool KnifePairAdapter::BindingAlive(const KnifeModelBinding& binding) noexcept
{
    return binding.Valid() && (!captured_ || binding == binding_) && access_.Alive(binding);
}

bool KnifePairAdapter::CanMutate() noexcept
{
    return captured_ && BindingAlive(binding_) && access_.SafePoint(binding_);
}

bool KnifePairAdapter::Capture(const KnifeModelBinding& binding) noexcept
{
    // Never discard an existing restore point to accept another capture.
    if (captured_ || !BindingAlive(binding) || !access_.SafePoint(binding)) { return false; }
    KnifePairState first, second;
    if (!access_.Read(binding, first) || !access_.Read(binding, second) || first != second || !Valid(first) ||
        first.world_definition != first.hud_definition || first.item_definition != first.world_definition ||
        !first.secondary_matches_hud || !first.attachments_valid)
    { return false; }
    const auto* source = FindKnifeModelAsset(first.world_definition);
    if (!source || access_.ExistingAnimationType(*source) != first.animation_type || !access_.Alive(binding))
    { return false; }
    binding_ = binding;
    original_ = first;
    captured_ = true;
    attempted_ = applied_ = maintenance_faulted_ = world_prepared_ = false;
    target_ = nullptr;
    target_symbol_ = 0;
    return true;
}

bool KnifePairAdapter::ReadOwned(KnifePairState& state) noexcept
{
    state = {};
    if (!captured_ || !BindingAlive(binding_) || !access_.Read(binding_, state) || !Valid(state) ||
        state.arms_model != original_.arms_model) { return false; }
    // A competing model/animation writer must not be overwritten by rollback.
    const auto target_id = target_ ? target_->definition : original_.world_definition;
    const auto permitted = [target_id](int current, int source) { return current == source || current == target_id; };
    return permitted(state.world_definition, original_.world_definition) &&
        permitted(state.hud_definition, original_.hud_definition) &&
        permitted(state.item_definition, original_.item_definition) &&
        (state.animation_type == original_.animation_type ||
            (target_symbol_ && state.animation_type == target_symbol_)) && access_.Alive(binding_);
}

bool KnifePairAdapter::PairReady(const KnifePairState& state, int definition, std::uintptr_t symbol) const noexcept
{
    return Valid(state) && state.world_definition == definition && state.hud_definition == definition &&
        state.item_definition == definition &&
        state.arms_model == original_.arms_model && state.animation_type == symbol &&
        state.secondary_matches_hud && state.attachments_valid;
}

bool KnifePairAdapter::ApplyPair(const KnifeModelBinding& binding, const KnifeModelAsset& asset,
    KnifeModelBinding& after) noexcept
{
    after = binding;
    return PrepareWorld(binding, asset) && (applied_ || FinishHud());
}

bool KnifePairAdapter::PrepareWorld(const KnifeModelBinding& binding, const KnifeModelAsset& asset) noexcept
{
    if (!captured_ || attempted_ || binding != binding_ || !CanMutate()) { return false; }
    // Accept the canonical asset table, not a caller-supplied arbitrary path.
    target_ = FindKnifeModelAsset(asset.definition);
    if (!target_ || target_->model != asset.model || target_->skeleton != asset.skeleton ||
        target_->animation_class != asset.animation_class ||
        target_->graph_animation_class != asset.graph_animation_class) { target_ = nullptr; return false; }
    attempted_ = true;
    target_symbol_ = access_.ExistingAnimationType(*target_);
    KnifePairState current;
    if (!target_symbol_ || !ReadOwned(current) || current != original_) { return false; }
    if (target_->definition == original_.world_definition) { committed_ = current; applied_ = true; return true; }

    // Select the target item/VData before the native model notification creates
    // its replacement HUD. This keeps deploy/inspect and HUD identity aligned
    // with the explicit target model from the first rebuilt viewmodel onward.
    if (!CanMutate() || !access_.CompareItemDefinition(binding_, original_.item_definition, target_->definition) ||
        !CanMutate() || !access_.SetBladeModel(binding_, KnifeBladeRole::World, *target_) ||
        !ReadOwned(current) || current.world_definition != target_->definition ||
        current.hud_definition != original_.hud_definition || current.item_definition != target_->definition) { return false; }
    world_prepared_ = true;
    return true;
}

bool KnifePairAdapter::AdoptPrearmedWorld(const KnifeModelBinding& binding,
    const KnifeModelAsset& source, const KnifeModelAsset& target) noexcept
{
    if (captured_ || !BindingAlive(binding) || !access_.SafePoint(binding)) return false;
    const auto* canonical_source = FindKnifeModelAsset(source.definition);
    target_ = FindKnifeModelAsset(target.definition);
    if (canonical_source == nullptr || target_ == nullptr || source.definition == target.definition ||
        canonical_source->model != source.model || canonical_source->skeleton != source.skeleton ||
        canonical_source->animation_class != source.animation_class ||
        canonical_source->graph_animation_class != source.graph_animation_class ||
        target_->model != target.model || target_->skeleton != target.skeleton ||
        target_->animation_class != target.animation_class ||
        target_->graph_animation_class != target.graph_animation_class)
    { target_ = nullptr; return false; }

    const auto source_symbol = access_.ExistingAnimationType(*canonical_source);
    target_symbol_ = access_.ExistingAnimationType(*target_);
    KnifePairState current, confirmed;
    if (!source_symbol || !target_symbol_ || !access_.Read(binding, current) ||
        !access_.Read(binding, confirmed) || current != confirmed ||
        current.world_definition != target_->definition ||
        current.item_definition != target_->definition ||
        (current.hud_definition != canonical_source->definition &&
         current.hud_definition != target_->definition) ||
        (current.animation_type != source_symbol && current.animation_type != target_symbol_) ||
        (current.hud_definition == target_->definition && !current.secondary_matches_hud) ||
        !current.attachments_valid)
    { target_ = nullptr; target_symbol_ = 0; return false; }

    // Keep a logical source restore point. Model resources are borrowed live
    // identities; restoration resolves the canonical source path instead of
    // replaying these target pointers.
    binding_ = binding;
    original_ = current;
    original_.world_definition = canonical_source->definition;
    original_.hud_definition = canonical_source->definition;
    original_.item_definition = canonical_source->definition;
    original_.animation_type = source_symbol;
    original_.secondary_matches_hud = true;
    captured_ = attempted_ = world_prepared_ = true;
    applied_ = maintenance_faulted_ = false;

    if (!access_.AdoptPreparedWorld(binding_, *canonical_source, *target_, current))
    {
        Abandon();
        return false;
    }
    // From this point a failure must retain captured_ so the lifecycle invokes
    // the native retirement ledger; never silently abandon adopted writes.
    return FinishHud();
}

bool KnifePairAdapter::CompletePreparedWorld(const KnifeModelBinding& replacement, const KnifeModelAsset& asset) noexcept
{
    if (!captured_ || !world_prepared_ || applied_ || !target_ || asset.definition != target_->definition ||
        asset.model != target_->model || asset.skeleton != target_->skeleton || asset.animation_class != target_->animation_class ||
        asset.graph_animation_class != target_->graph_animation_class ||
        !replacement.Valid() || replacement.scene_epoch != binding_.scene_epoch || replacement.xuid != binding_.xuid ||
        replacement.pawn != binding_.pawn || replacement.world != binding_.world || replacement.arms != binding_.arms ||
        replacement.weapon_graph_controller != binding_.weapon_graph_controller ||
        replacement.body_animation_controller != binding_.body_animation_controller ||
        replacement.hud_weapon == binding_.hud_weapon || !access_.Alive(replacement) || !access_.SafePoint(replacement)) return false;
    // Only the proven replacement HUD/graph is rebound. Original source models
    // and the restore ledger remain owned by the same world-knife transaction.
    binding_ = replacement;
    return FinishHud();
}

bool KnifePairAdapter::FinishHud() noexcept
{
    KnifePairState current;
    if (!world_prepared_ || !CanMutate() || !ReadOwned(current) || current.world_definition != target_->definition) return false;
    if (current.item_definition != target_->definition &&
        (!CanMutate() || !access_.CompareItemDefinition(binding_, original_.item_definition, target_->definition))) return false;
    if (current.hud_definition != target_->definition &&
        (!CanMutate() || !access_.SetBladeModel(binding_, KnifeBladeRole::Hud, *target_))) return false;
    if (!ReadOwned(current) ||
        current.world_definition != target_->definition ||
        current.hud_definition != target_->definition) { return false; }
    // A pre-armed target definition commonly makes Source 2 construct the
    // target HUD and its matching secondary skeleton natively. Recollecting an
    // already-correct graph is redundant and can invalidate the live M9 HUD
    // while its outer arms update is still returning. Recollect only after an
    // actual HUD model change (or other proven skeleton mismatch).
    if (!current.secondary_matches_hud)
    {
        if (!CanMutate() || !access_.Recollect(binding_) || !ReadOwned(current) ||
            !current.secondary_matches_hud) return false;
    }
    if (!current.attachments_valid) return false;
    if (!CanMutate() || !access_.CompareAnimationType(binding_, current.animation_type, target_symbol_) ||
        !ReadOwned(current) || !PairReady(current, target_->definition, target_symbol_)) { return false; }
    committed_ = current;
    applied_ = true;
    world_prepared_ = false;
    return true;
}

bool KnifePairAdapter::VerifyPair(const KnifeModelBinding& binding, const KnifeModelAsset& asset) noexcept
{
    KnifePairState current;
    return applied_ && !maintenance_faulted_ && target_ && asset.definition == target_->definition &&
        binding == binding_ && ReadOwned(current) && current == committed_ &&
        PairReady(current, target_->definition, target_symbol_);
}

bool KnifePairAdapter::Restore(const KnifeModelBinding& binding) noexcept
{
    if (!captured_ || binding != binding_ || !CanMutate()) { return false; }
    applied_ = false; // A partial restore must never keep maintaining the target animation.
    KnifePairState current;
    if (!ReadOwned(current)) { return false; }
    if (current.item_definition != original_.item_definition &&
        (!access_.CompareItemDefinition(binding_, current.item_definition, original_.item_definition) || !ReadOwned(current)))
        return false;
    bool models_changed = false;
    for (const auto role : {KnifeBladeRole::World, KnifeBladeRole::Hud})
    {
        if (!CanMutate() || !ReadOwned(current)) { return false; }
        const auto source = role == KnifeBladeRole::World ? original_.world_definition : original_.hud_definition;
        const auto actual = role == KnifeBladeRole::World ? current.world_definition : current.hud_definition;
        if (source != actual)
        {
            if (!access_.SetBladeModel(binding_, role, *FindKnifeModelAsset(source))) { return false; }
            models_changed = true;
        }
    }
    if (!CanMutate() || !ReadOwned(current)) { return false; }
    if (models_changed || !current.secondary_matches_hud)
    {
        if (!access_.Recollect(binding_) || !ReadOwned(current)) { return false; }
    }
    if (!CanMutate() || !access_.CompareAnimationType(binding_, current.animation_type, original_.animation_type) ||
        !ReadOwned(current) || !PairReady(current, original_.world_definition, original_.animation_type)) { return false; }
    // Resource bindings may legitimately differ after a native re-load. The
    // resolved source model/skeleton and the unchanged arms are what matters.
    Abandon();
    return true;
}

void KnifePairAdapter::Abandon() noexcept
{
    // No native calls and no stale entity dereferences. Engine SetModel owns
    // references; this object owns only values and canonical static paths.
    binding_ = {};
    original_ = committed_ = {};
    target_ = nullptr;
    target_symbol_ = 0;
    captured_ = attempted_ = applied_ = maintenance_faulted_ = world_prepared_ = false;
}

bool KnifePairAdapter::AfterNativeUpdate(const KnifeModelRequest& request, const KnifeModelBinding& binding) noexcept
{
    if (!applied_ || maintenance_faulted_) { return false; }
    const auto fail = [this]() { maintenance_faulted_ = true; return false; };
    if (!target_ || request.xuid != binding_.xuid || request.definition != target_->definition ||
        binding != binding_ || !CanMutate()) { return fail(); }
    int item_definition = 0;
    if (!access_.ReadItemDefinition(binding_, item_definition)) return fail();
    if (item_definition != target_->definition)
    {
        if (item_definition != original_.item_definition || !CanMutate() ||
            !access_.CompareItemDefinition(binding_, original_.item_definition, target_->definition)) return fail();
    }
    if (!access_.Maintain(binding_, committed_, original_.animation_type, target_symbol_)) { return fail(); }
    // Pure scalar maintenance. Even after 100k callbacks, zero model/resource
    // calls here; failure is latched rather than retried every frame.
    return true;
}

bool KnifePairAccess::Maintain(const KnifeModelBinding& binding, const KnifePairState& committed,
    std::uintptr_t source, std::uintptr_t target) noexcept
{
    KnifePairState current;
    if (!Alive(binding) || !SafePoint(binding) || !Read(binding, current)) return false;
    auto expected = committed; expected.animation_type = current.animation_type;
    if (current != expected || !current.secondary_matches_hud || !current.attachments_valid) return false;
    return
        (current.animation_type == source || current.animation_type == target) &&
        CompareAnimationType(binding, current.animation_type, target);
}
}
