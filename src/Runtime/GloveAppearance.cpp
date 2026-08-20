#include "Runtime/GloveAppearance.hpp"

namespace nskinzcs2::runtime
{
GloveSlotMode ResolveGloveSlotMode(const nskinzcs2::model::GloveProfile& profile) noexcept
{
    return profile.mode == nskinzcs2::model::GloveProfileMode::ModelAndFinish
        ? GloveSlotMode::ModelAndFinish
        : GloveSlotMode::FinishOnly;
}

bool GloveProfileRequiresModelDefinition(const GloveSlotMode mode) noexcept
{
    return mode == GloveSlotMode::ModelAndFinish;
}

GloveCarrierKind ResolveCanonicalGloveCarrierKind(
    const GloveSlotMode mode,
    const bool stable_wearable_available,
    const bool pawn_glove_available) noexcept
{
    switch (mode)
    {
    case GloveSlotMode::FinishOnly:
        return pawn_glove_available ? GloveCarrierKind::PawnEconGloves : GloveCarrierKind::None;
    case GloveSlotMode::ModelAndFinish:
        return pawn_glove_available
            ? GloveCarrierKind::PawnEconGloves
            : (stable_wearable_available ? GloveCarrierKind::Wearable : GloveCarrierKind::None);
    case GloveSlotMode::None:
    default:
        return GloveCarrierKind::None;
    }
}

GloveAppearanceRequest BuildGloveAppearanceRequest(const nskinzcs2::model::GloveProfile& profile) noexcept
{
    GloveAppearanceRequest request;
    request.xuid = profile.xuid;
    request.mode = ResolveGloveSlotMode(profile);
    request.model_definition = profile.model_definition;
    request.paint_kit = profile.paint_kit;
    request.quality = profile.quality;
    request.seed = profile.seed;
    request.wear = profile.wear;
    return request;
}

int SelectReusableWearableHandleSlot(
    const std::uint32_t* handles,
    const std::size_t handle_count,
    const std::uint32_t requested_handle) noexcept
{
    constexpr std::uint32_t invalid_handle = 0xFFFFFFFFu;
    constexpr std::uint32_t entity_index_mask = 0x7FFFu;
    if (handles == nullptr || handle_count == 0 ||
        requested_handle == 0 || requested_handle == invalid_handle ||
        (requested_handle & entity_index_mask) == 0)
    {
        return -1;
    }

    int reusable_slot = -1;
    for (std::size_t index = 0; index < handle_count; ++index)
    {
        const auto handle = handles[index];
        if (handle == requested_handle)
        {
            return static_cast<int>(index);
        }

        const auto slot_is_invalid =
            handle == 0 || handle == invalid_handle || (handle & entity_index_mask) == 0;
        if (reusable_slot < 0 && slot_is_invalid)
        {
            reusable_slot = static_cast<int>(index);
        }
    }

    return reusable_slot;
}

bool ShouldInitializeWearableModel(
    const GloveSlotMode mode,
    const bool real_wearable,
    const int wearable_entity_index,
    const bool target_model_known,
    const std::uint64_t live_model_handle,
    const bool formal_bind_context_verified) noexcept
{
    return mode == GloveSlotMode::ModelAndFinish &&
        real_wearable &&
        wearable_entity_index >= 0 &&
        target_model_known &&
        live_model_handle == 0 &&
        formal_bind_context_verified;
}

GloveCompatibilityResult EvaluateGloveCompatibility(const GloveCompatibilityInputs& inputs) noexcept
{
    if (!inputs.carrier_valid)
    {
        return {false, GloveCompatibilityReason::MissingCarrier};
    }

    if (inputs.mode == GloveSlotMode::None)
    {
        return {false, GloveCompatibilityReason::MissingCarrier};
    }

    if (inputs.mode == GloveSlotMode::FinishOnly)
    {
        if (inputs.carrier_kind != GloveCarrierKind::PawnEconGloves)
        {
            return {false, GloveCompatibilityReason::MissingCarrier};
        }

        if (inputs.compatible_paint_definition != 0 &&
            inputs.current_definition_index != 0 &&
            inputs.compatible_paint_definition != inputs.current_definition_index)
        {
            return {false, GloveCompatibilityReason::PaintIncompatible};
        }

        if (inputs.current_definition_index == 0)
        {
            return {false, GloveCompatibilityReason::MissingModelIdentity};
        }

        return {true, GloveCompatibilityReason::Compatible};
    }

    if (inputs.carrier_kind == GloveCarrierKind::PawnEconGloves)
    {
        if (inputs.requested_model_definition == 0)
        {
            return {false, GloveCompatibilityReason::MissingModelDefinition};
        }

        if (inputs.compatible_paint_definition != 0 &&
            inputs.compatible_paint_definition != inputs.requested_model_definition)
        {
            return {false, GloveCompatibilityReason::PaintIncompatible};
        }

        if (!inputs.target_model_known)
        {
            return {false, GloveCompatibilityReason::MissingModelIdentity};
        }

        return {true, GloveCompatibilityReason::Compatible};
    }

    if (inputs.carrier_kind != GloveCarrierKind::Wearable)
    {
        return {false, GloveCompatibilityReason::MissingCarrier};
    }

    if (inputs.requested_model_definition == 0)
    {
        return {false, GloveCompatibilityReason::MissingModelDefinition};
    }

    if (inputs.compatible_paint_definition != 0 &&
        inputs.compatible_paint_definition != inputs.requested_model_definition)
    {
        return {false, GloveCompatibilityReason::PaintIncompatible};
    }

    const auto allow_wearable_identity_after_apply =
        inputs.allow_render_binding_after_apply &&
        inputs.carrier_kind == GloveCarrierKind::Wearable;

    if (!inputs.render_binding_valid && !allow_wearable_identity_after_apply)
    {
        return {false, GloveCompatibilityReason::MissingRenderBinding};
    }

    // A freshly created, live CEconWearable has a valid entity identity and
    // ItemView before CS2 has assigned its model state.  The definition write
    // plus the normal pawn glove refresh is what establishes that model.  Only
    // permit this deferred identity path for a real Wearable whose requested
    // model is known; post-apply verification still requires the world and
    // first-person model identities to match the requested glove family.
    if (allow_wearable_identity_after_apply)
    {
        if (!inputs.target_model_known)
        {
            return {false, GloveCompatibilityReason::MissingModelIdentity};
        }

        return {true, GloveCompatibilityReason::Compatible};
    }

    if (!inputs.first_person_arms_skeleton_identity_resolved ||
        !inputs.target_view_skeleton_identity_resolved ||
        !inputs.target_model_known)
    {
        return {false, GloveCompatibilityReason::MissingModelIdentity};
    }

    if (!inputs.target_view_matches_first_person_skeleton)
    {
        return {false, GloveCompatibilityReason::SkeletonMismatch};
    }

    return {true, GloveCompatibilityReason::Compatible};
}

const wchar_t* GloveCompatibilityReasonText(const GloveCompatibilityReason reason) noexcept
{
    switch (reason)
    {
    case GloveCompatibilityReason::Compatible:
        return L"compatible";
    case GloveCompatibilityReason::MissingCarrier:
        return L"missing glove carrier";
    case GloveCompatibilityReason::MissingModelDefinition:
        return L"missing model definition";
    case GloveCompatibilityReason::PaintIncompatible:
        return L"paint incompatible with glove model";
    case GloveCompatibilityReason::MissingRenderBinding:
        return L"missing glove render binding";
    case GloveCompatibilityReason::MissingModelIdentity:
        return L"missing glove model identity";
    case GloveCompatibilityReason::SkeletonMismatch:
        return L"skeleton mismatch";
    default:
        return L"unknown";
    }
}

GloveRenderVerificationResult EvaluateGloveRenderVerification(const GloveRenderVerificationInputs& inputs) noexcept
{
    if (inputs.mode == GloveSlotMode::FinishOnly || inputs.pawn_econ_gloves_reapply_path)
    {
        if (!inputs.carrier_field_readback_matched)
        {
            return {false, GloveRenderVerificationReason::CarrierFieldMismatch};
        }

        if (!inputs.reapply_triggered)
        {
            return {false, GloveRenderVerificationReason::ReapplyNotTriggered};
        }

        return {true, GloveRenderVerificationReason::Verified};
    }

    if (!inputs.live_render_state_resolved)
    {
        return {false, GloveRenderVerificationReason::MissingLiveRenderState};
    }

    if (inputs.mode == GloveSlotMode::ModelAndFinish &&
        (!inputs.has_first_person_render || !inputs.first_person_render_state_resolved))
    {
        return {false, GloveRenderVerificationReason::MissingHudRenderState};
    }

    if (inputs.requested_model_changed &&
        !(inputs.live_model_binding_changed || inputs.first_person_model_binding_changed || inputs.first_person_binding_changed))
    {
        return {false, GloveRenderVerificationReason::ModelBindingUnchanged};
    }

    if (inputs.requested_finish_changed &&
        !(inputs.live_material_binding_changed ||
          inputs.first_person_material_binding_changed ||
          inputs.first_person_binding_changed ||
          inputs.finish_binding_confirmed))
    {
        return {false, GloveRenderVerificationReason::MaterialBindingUnchanged};
    }

    if (inputs.requested_finish_changed && !inputs.live_material_resource_matches_target)
    {
        return {false, GloveRenderVerificationReason::MaterialBindingMismatch};
    }

    if (inputs.mode == GloveSlotMode::ModelAndFinish &&
        inputs.has_first_person_render &&
        inputs.requested_finish_changed &&
        !inputs.first_person_material_resource_matches_target)
    {
        return {false, GloveRenderVerificationReason::MaterialBindingMismatch};
    }

    return {true, GloveRenderVerificationReason::Verified};
}

const wchar_t* GloveRenderVerificationReasonText(const GloveRenderVerificationReason reason) noexcept
{
    switch (reason)
    {
    case GloveRenderVerificationReason::Verified:
        return L"verified";
    case GloveRenderVerificationReason::CarrierFieldMismatch:
        return L"glove carrier fields did not match the requested finish";
    case GloveRenderVerificationReason::ReapplyNotTriggered:
        return L"glove reapply was not triggered";
    case GloveRenderVerificationReason::MissingLiveRenderState:
        return L"missing live glove render state";
    case GloveRenderVerificationReason::MissingHudRenderState:
        return L"missing first-person glove render state";
    case GloveRenderVerificationReason::ModelBindingUnchanged:
        return L"glove model binding did not change";
    case GloveRenderVerificationReason::MaterialBindingUnchanged:
        return L"glove material binding did not change";
    case GloveRenderVerificationReason::MaterialBindingMismatch:
        return L"glove material binding did not match the requested target";
    default:
        return L"unknown";
    }
}
}
