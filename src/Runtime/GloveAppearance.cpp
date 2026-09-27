#include "Runtime/GloveAppearance.hpp"

namespace nskinzcs2::runtime
{
SyntheticEconItemIdentity BuildSyntheticGloveEconItemIdentity(
    const std::uint64_t xuid,
    const int definition_index) noexcept
{
    constexpr std::uint32_t synthetic_namespace = 0xC5210000u;
    constexpr std::uint64_t fnv_offset = 1469598103934665603ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;
    constexpr std::uint64_t glove_slot_salt = 0x474C4F56455F4944ull; // "GLOVE_ID"

    const auto normalized_definition =
        static_cast<std::uint32_t>(definition_index > 0 ? definition_index : 0) & 0xFFFFu;
    SyntheticEconItemIdentity identity;
    identity.high = synthetic_namespace | normalized_definition;

    std::uint64_t hash = fnv_offset;
    const auto mix = [&hash](const std::uint64_t value)
    {
        hash ^= value;
        hash *= fnv_prime;
    };
    mix(glove_slot_salt);
    mix(xuid);
    mix(normalized_definition);

    identity.low = static_cast<std::uint32_t>(hash ^ (hash >> 32u));
    if (identity.low == 0 || identity.low == 0xFFFFFFFFu)
    {
        identity.low ^= 0x6E536B7Au; // "nSkz"
    }
    if (identity.low == 0 || identity.low == 0xFFFFFFFFu)
    {
        identity.low = 1u;
    }
    return identity;
}

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
        // Build 2000918's native visual updater reads pawn m_EconGloves at
        // +0x1770 and materialises its own client-only output at +0x15F8. It
        // never reads m_hMyWearables. Prefer that single authoritative input
        // even for an initially empty stock T/CT slot; retain a published
        // wearable only as a compatibility fallback when the Pawn view is
        // genuinely unavailable.
        if (pawn_glove_available) return GloveCarrierKind::PawnEconGloves;
        return stable_wearable_available ? GloveCarrierKind::Wearable : GloveCarrierKind::None;
    case GloveSlotMode::None:
    default:
        return GloveCarrierKind::None;
    }
}

bool CanUsePawnEconGlovesCarrier(
    const GloveSlotMode mode,
    const bool definition_readable,
    const int current_definition_index,
    const bool current_definition_is_glove,
    const bool requested_definition_is_glove) noexcept
{
    if (!definition_readable)
    {
        return false;
    }

    if (mode == GloveSlotMode::FinishOnly)
    {
        return current_definition_index > 0 && current_definition_is_glove;
    }

    if (mode == GloveSlotMode::ModelAndFinish)
    {
        // The embedded view may be definition 0 for recorded stock hands.
        // Only replace that empty value or an existing glove definition; a
        // positive non-glove definition indicates the wrong schema object.
        return requested_definition_is_glove &&
            (current_definition_index == 0 || current_definition_is_glove);
    }

    return false;
}

std::uint64_t BuildGloveWearableCreationEpoch(
    const std::uint64_t provider_generation,
    const std::uint32_t pawn_handle,
    const std::uintptr_t pawn_identity) noexcept
{
    // FNV-1a over fixed-width values is deterministic and cheap. The full
    // Source 2 handle includes its serial, while the pointer disambiguates a
    // rebuilt pawn even if an entity slot is reused unusually quickly.
    std::uint64_t epoch = 1469598103934665603ull;
    const auto mix = [&epoch](const std::uint64_t value)
    {
        epoch ^= value;
        epoch *= 1099511628211ull;
    };
    mix(provider_generation);
    mix(static_cast<std::uint64_t>(pawn_handle));
    mix(static_cast<std::uint64_t>(pawn_identity));
    return epoch;
}

std::uint64_t GloveWearableRetryDelayMillis(const std::uint32_t failure_count) noexcept
{
    if (failure_count == 0)
    {
        return 0;
    }

    return 125ull << std::min<std::uint32_t>(failure_count - 1u, 4u);
}

GloveAppearanceRequest BuildGloveAppearanceRequest(const nskinzcs2::model::GloveProfile& profile) noexcept
{
    GloveAppearanceRequest request;
    request.xuid = profile.xuid;
    request.mode = ResolveGloveSlotMode(profile);
    request.model_definition = profile.model_definition;
    request.paint_kit = profile.paint_kit;
    request.explicit_vanilla_finish = profile.explicit_vanilla_finish;
    request.quality = profile.quality;
    request.seed = profile.seed;
    request.wear = nskinzcs2::model::NormalizeCosmeticWear(profile.wear);
    return request;
}

int ResolveDefaultGloveDefinitionForTeam(const int team_number) noexcept
{
    switch (team_number)
    {
    case 2:
        return 5028;
    case 3:
        return 5029;
    default:
        return 0;
    }
}

bool IsStockTeamGloveRestoreRequest(
    const GloveAppearanceRequest& request,
    const int team_number) noexcept
{
    const auto stock_definition = ResolveDefaultGloveDefinitionForTeam(team_number);
    return request.mode == GloveSlotMode::ModelAndFinish &&
        request.explicit_vanilla_finish &&
        request.paint_kit == 0 &&
        stock_definition != 0 &&
        request.model_definition == stock_definition;
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

    if (inputs.carrier_kind == GloveCarrierKind::PawnEconGloves ||
        inputs.carrier_kind == GloveCarrierKind::PawnEconGlovesRenderFallback)
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

        if (inputs.mode == GloveSlotMode::ModelAndFinish && inputs.pawn_econ_gloves_reapply_path)
        {
            // The item-view readback and dirty flags prove that the request
            // was accepted, not that CS2 rebuilt the visible glove family.
            // Runtime polling must still match the native world output and
            // the first-person HUD model before reporting visual success.
            return {false, GloveRenderVerificationReason::NativeVisualRefreshPending};
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
    case GloveRenderVerificationReason::NativeVisualRefreshPending:
        return L"native glove visual refresh pending";
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
