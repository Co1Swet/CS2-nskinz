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
        if (stable_wearable_available)
        {
            return GloveCarrierKind::Wearable;
        }

        return pawn_glove_available
            ? GloveCarrierKind::PawnEconGlovesRenderFallback
            : GloveCarrierKind::None;
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

    const auto uses_render_fallback =
        inputs.carrier_kind == GloveCarrierKind::PawnEconGlovesRenderFallback;
    if (inputs.carrier_kind != GloveCarrierKind::Wearable && !uses_render_fallback)
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

    if (!inputs.render_binding_valid && !inputs.allow_render_binding_after_apply)
    {
        return {false, GloveCompatibilityReason::MissingRenderBinding};
    }

    if (uses_render_fallback && inputs.allow_render_binding_after_apply)
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
    if (inputs.mode == GloveSlotMode::FinishOnly)
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
