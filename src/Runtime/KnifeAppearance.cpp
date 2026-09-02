#include "Runtime/KnifeAppearance.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

namespace
{
constexpr std::array<int, 22> kSupportedKnifeDefinitions {{
    42,
    59,
    500,
    503,
    505,
    506,
    507,
    508,
    509,
    512,
    514,
    515,
    516,
    517,
    518,
    519,
    520,
    521,
    522,
    523,
    525,
    526,
}};

constexpr int kKnifeDefaultCtDefinition = 42;
constexpr int kKnifeDefaultTDefinition = 59;

std::uint32_t FloatBits(const float value) noexcept
{
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void HashCombine(std::uint64_t& seed, const std::uint64_t value) noexcept
{
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
}

bool IsSupportedKnifeDefinitionImpl(const int definition_index) noexcept
{
    return std::find(
               kSupportedKnifeDefinitions.begin(),
               kSupportedKnifeDefinitions.end(),
               definition_index) != kSupportedKnifeDefinitions.end();
}

int ResolveKnifeRuntimeModelDefinition(const nskinzcs2::model::KnifeProfile& profile) noexcept
{
    if (profile.mode != nskinzcs2::model::KnifeProfileMode::ModelAndFinish)
    {
        return 0;
    }

    if (IsSupportedKnifeDefinitionImpl(profile.model_definition) &&
        profile.model_definition != kKnifeDefaultCtDefinition &&
        profile.model_definition != kKnifeDefaultTDefinition)
    {
        return profile.model_definition;
    }

    return 0;
}
}

namespace nskinzcs2::runtime
{
KnifeSlotMode ResolveKnifeSlotMode(const nskinzcs2::model::KnifeProfile& profile) noexcept
{
    if (!profile.enabled || profile.xuid == 0)
    {
        return KnifeSlotMode::None;
    }

    const auto runtime_model_definition = ResolveKnifeRuntimeModelDefinition(profile);
    if (profile.mode == nskinzcs2::model::KnifeProfileMode::ModelAndFinish &&
        runtime_model_definition != 0)
    {
        return KnifeSlotMode::ModelAndFinish;
    }

    return KnifeSlotMode::FinishOnly;
}

bool KnifeProfileRequiresModelDefinition(const KnifeSlotMode mode) noexcept
{
    return mode == KnifeSlotMode::ModelAndFinish;
}

KnifeAppearanceRequest BuildKnifeAppearanceRequest(const nskinzcs2::model::KnifeProfile& profile) noexcept
{
    KnifeAppearanceRequest request;
    request.xuid = profile.xuid;
    request.mode = ResolveKnifeSlotMode(profile);
    request.model_definition =
        request.mode == KnifeSlotMode::ModelAndFinish
        ? ResolveKnifeRuntimeModelDefinition(profile)
        : 0;
    request.paint_kit = profile.paint_kit;
    // A new model must not inherit the recorded source knife's paint. Keep
    // saved dash intent untouched; only a valid model request derives vanilla.
    request.explicit_vanilla_finish = profile.explicit_vanilla_finish ||
        (request.mode == KnifeSlotMode::ModelAndFinish && profile.paint_kit == 0);
    request.quality = profile.quality;
    request.seed = profile.seed;
    request.stat_trak = profile.stat_trak;
    request.wear = profile.wear;
    request.custom_name = profile.custom_name;
    return request;
}

KnifeAppearanceRequest BuildKnifeFinishRequest(const nskinzcs2::model::KnifeProfile& profile) noexcept
{
    auto request = BuildKnifeAppearanceRequest(profile);
    request.model_definition = 0;
    if (request.mode != KnifeSlotMode::None)
    {
        request.mode = KnifeSlotMode::FinishOnly;
    }
    return request;
}

KnifeApplyGateResult EvaluateKnifeApplyGate(const KnifeApplyGateInputs& inputs) noexcept
{
    KnifeApplyGateResult result;
    result.knife_sync_ready = inputs.context.knife_sync_ready;
    result.allow_finish_writes = false;
    result.resolved_mode = KnifeSlotMode::None;
    result.resolved_model_definition = 0;
    const bool authoritative_watched_knife =
        KnifeRuntimeContextHasAuthoritativeWatchedKnife(inputs.context);
    const bool has_world_binding_evidence =
        authoritative_watched_knife &&
        inputs.context.world_carrier_valid &&
        inputs.context.world_item_view_valid;
    const bool has_diagnostic_first_person_binding_evidence =
        inputs.context.first_person_source_valid &&
        (inputs.context.providee_carrier_valid ||
         inputs.context.viewmodel_carrier_valid);
    const bool has_direct_render_diagnostic_evidence =
        inputs.context.hud_proxy_fallback_allowed &&
        inputs.context.first_person_source_valid &&
        inputs.context.direct_first_person_render_valid;
    const bool has_real_viewmodel_binding_evidence =
        inputs.context.first_person_source_valid &&
        inputs.context.viewmodel_carrier_valid &&
        inputs.context.first_person_owner_kind == KnifeFirstPersonOwnerKind::ViewModel &&
        (!inputs.context.observer_chain_present ||
         (inputs.context.observer_in_eye_valid &&
          inputs.context.real_viewmodel_handle_valid &&
          inputs.context.real_viewmodel_weapon_matches_active_weapon &&
          inputs.context.real_viewmodel_active_weapon_verified_knife));
    const bool has_resource_query_model_swap_evidence =
        inputs.context.resource_query_bridge_ready &&
        inputs.context.resource_model_swap_ready &&
        inputs.context.resource_query_redirect_pairs > 0;
    const bool has_first_person_binding_evidence =
        has_diagnostic_first_person_binding_evidence ||
        has_direct_render_diagnostic_evidence;
    const bool has_model_write_first_person_binding_evidence =
        has_real_viewmodel_binding_evidence ||
        has_resource_query_model_swap_evidence;
    if (inputs.request.mode == KnifeSlotMode::None || inputs.request.xuid == 0)
    {
        result.reason = KnifeApplyGateReason::MissingWatchedKnife;
        return result;
    }

    if (inputs.context.watched_xuid != 0 &&
        inputs.request.xuid != inputs.context.watched_xuid &&
        !inputs.context.watched_target &&
        !has_first_person_binding_evidence &&
        !has_world_binding_evidence)
    {
        result.reason = KnifeApplyGateReason::ProfileXuidMismatch;
        return result;
    }

    if (inputs.context.bridged_pawn_entity_index >= 0 &&
        !inputs.context.bridge_active_weapon_is_knife)
    {
        result.reason = KnifeApplyGateReason::MissingWatchedKnife;
        return result;
    }

    // Finish-only writes target the authoritative world-carried CEconItemView
    // and do not touch the model, viewmodel ownership, or animation state.  A
    // demo observer can expose that carrier one render pass before its
    // first-person providee/viewmodel chain is discoverable.  Requiring the
    // latter blocked safe paint-only updates exactly while the watched player
    // was deploying the knife.  Keep first-person evidence mandatory below for
    // model/animation writes, but let the authoritative carrier accept finishes
    // immediately.
    const bool ready_for_finish_only = has_world_binding_evidence;

    if (!ready_for_finish_only)
    {
        result.reason =
            !has_world_binding_evidence
            ? KnifeApplyGateReason::MissingWorldBinding
            : KnifeApplyGateReason::MissingFirstPersonBinding;
        return result;
    }

    result.allow_finish_writes = true;
    result.resolved_mode = KnifeSlotMode::FinishOnly;
    result.reason = KnifeApplyGateReason::ReadyForFinishOnly;

    if (inputs.request.mode != KnifeSlotMode::ModelAndFinish)
    {
        return result;
    }

    result.degraded = true;
    if (inputs.request.model_definition == 0)
    {
        result.reason = KnifeApplyGateReason::MissingModelDefinition;
        return result;
    }

    if (!IsSupportedKnifeDefinitionImpl(inputs.request.model_definition) ||
        IsDefaultKnifeDefinition(inputs.request.model_definition))
    {
        result.reason = KnifeApplyGateReason::UnsupportedModelDefinition;
        return result;
    }

    if (!inputs.context.watched_target &&
        !has_resource_query_model_swap_evidence)
    {
        result.reason = KnifeApplyGateReason::MissingWatchedKnife;
        return result;
    }

    if (!has_world_binding_evidence)
    {
        result.reason = KnifeApplyGateReason::MissingWorldBinding;
        return result;
    }

    if (inputs.context.observer_chain_present && !inputs.context.observer_in_eye_valid)
    {
        result.reason = KnifeApplyGateReason::MissingWatchedKnife;
        return result;
    }

    if (inputs.context.observer_chain_present &&
        !inputs.context.observer_active_weapon_is_knife)
    {
        result.reason = KnifeApplyGateReason::MissingFirstPersonBinding;
        return result;
    }

    // Keep the resolved target definition once the watched knife and the
    // stable finish-only bindings are verified. The strict viewmodel gate
    // below still controls direct model writes and animation remap.
    result.resolved_model_definition = inputs.request.model_definition;

    if (!has_model_write_first_person_binding_evidence)
    {
        result.reason = KnifeApplyGateReason::MissingFirstPersonBinding;
        return result;
    }

    result.allow_model_writes = true;
    result.resolved_mode = KnifeSlotMode::ModelAndFinish;
    if (!has_real_viewmodel_binding_evidence)
    {
        result.degraded = true;
        result.reason = KnifeApplyGateReason::ReadyForModelAndFinish;
        return result;
    }

    if (!inputs.context.animation_hook_ready)
    {
        result.degraded = true;
        result.reason = KnifeApplyGateReason::ReadyForModelAndFinish;
        return result;
    }

    result.degraded = false;
    result.animation_binding_ready = true;
    result.animation_requires_strict_verification = true;
    result.allow_animation_remap = true;
    result.reason = KnifeApplyGateReason::ReadyForModelAndFinish;
    return result;
}

KnifeAppearancePipelineRoute ResolveKnifeAppearancePipelineRoute(
    const KnifeApplyGateResult& gate) noexcept
{
    KnifeAppearancePipelineRoute route;
    route.allow_model_rebind =
        gate.allow_finish_writes &&
        gate.allow_model_writes &&
        gate.resolved_mode == KnifeSlotMode::ModelAndFinish;
    route.use_exact_appearance_pipeline =
        gate.allow_finish_writes &&
        (gate.resolved_mode == KnifeSlotMode::FinishOnly || route.allow_model_rebind);
    return route;
}

KnifeRenderVerificationResult EvaluateKnifeRenderVerification(const KnifeRenderVerificationInputs& inputs) noexcept
{
    if (!inputs.world_render_state_resolved)
    {
        return {false, KnifeRenderVerificationReason::MissingWorldRenderState};
    }

    if (!inputs.world_render_expected_model_resolved)
    {
        return {false, KnifeRenderVerificationReason::MissingWorldRenderModel};
    }

    if (!inputs.world_render_model_matches_target)
    {
        return {false, KnifeRenderVerificationReason::WorldRenderModelMismatch};
    }

    if (!inputs.world_render_mesh_matches_target)
    {
        return {false, KnifeRenderVerificationReason::WorldRenderMeshMismatch};
    }

    if (!inputs.first_person_render_state_resolved)
    {
        return {false, KnifeRenderVerificationReason::MissingFirstPersonRenderState};
    }

    if (!inputs.first_person_render_expected_model_resolved)
    {
        return {false, KnifeRenderVerificationReason::MissingFirstPersonRenderModel};
    }

    if (inputs.first_person_render_untrusted)
    {
        return {false, KnifeRenderVerificationReason::UntrustedFirstPersonRender};
    }

    if (!inputs.first_person_render_model_matches_target)
    {
        return {false, KnifeRenderVerificationReason::FirstPersonRenderModelMismatch};
    }

    if (!inputs.first_person_render_mesh_matches_target)
    {
        return {false, KnifeRenderVerificationReason::FirstPersonRenderMeshMismatch};
    }

    return {true, KnifeRenderVerificationReason::Verified};
}

bool IsDefaultKnifeDefinition(const int definition_index) noexcept
{
    return definition_index == kKnifeDefaultCtDefinition || definition_index == kKnifeDefaultTDefinition;
}

bool KnifeRuntimeContextHasAuthoritativeWatchedKnife(const KnifeRuntimeContext& context) noexcept
{
    if (context.observer_chain_present)
    {
        return context.observer_in_eye_valid && context.observer_active_weapon_is_knife;
    }

    if (context.bridged_pawn_entity_index >= 0)
    {
        return context.bridge_active_weapon_is_knife;
    }

    return context.active_weapon_is_knife;
}

bool ShouldApplyKnifeModelDefinitionOverride(
    const KnifeAppearanceRequest& request,
    const int live_definition_index) noexcept
{
    return request.mode == KnifeSlotMode::ModelAndFinish &&
        request.model_definition != 0 &&
        live_definition_index != request.model_definition;
}

int ResolveKnifeRuntimeOverrideDefinition(
    const KnifeAppearanceRequest& request,
    const int live_definition_index) noexcept
{
    return ShouldApplyKnifeModelDefinitionOverride(request, live_definition_index)
        ? request.model_definition
        : 0;
}

std::uint64_t BuildKnifeProfileApplySignature(
    const KnifeAppearanceRequest& request,
    const std::uint64_t xuid,
    const int live_definition_index,
    const std::uint64_t target_tag) noexcept
{
    std::uint64_t signature = 0x03C6E1F7654B91D3ull;
    HashCombine(signature, target_tag);
    HashCombine(signature, xuid);
    HashCombine(signature, static_cast<std::uint64_t>(live_definition_index));
    HashCombine(signature, static_cast<std::uint64_t>(request.mode == KnifeSlotMode::ModelAndFinish ? 2 : request.mode == KnifeSlotMode::FinishOnly ? 1 : 0));
    HashCombine(signature, static_cast<std::uint64_t>(
        request.mode == KnifeSlotMode::ModelAndFinish ? request.model_definition : 0));
    HashCombine(signature, static_cast<std::uint64_t>(request.paint_kit));
    HashCombine(signature, static_cast<std::uint64_t>(request.explicit_vanilla_finish ? 1 : 0));
    HashCombine(signature, static_cast<std::uint64_t>(request.quality));
    HashCombine(signature, static_cast<std::uint64_t>(request.seed));
    HashCombine(signature, static_cast<std::uint64_t>(request.stat_trak));
    HashCombine(signature, static_cast<std::uint64_t>(FloatBits(request.wear)));
    HashCombine(signature, static_cast<std::uint64_t>(std::hash<std::string> {}(request.custom_name)));

    return signature;
}

const wchar_t* KnifeApplyGateReasonText(const KnifeApplyGateReason reason) noexcept
{
    switch (reason)
    {
    case KnifeApplyGateReason::ReadyForFinishOnly:
        return L"Knife finish-only slot.";
    case KnifeApplyGateReason::ReadyForModelAndFinish:
        return L"Knife swap is ready for strict world + first-person verification.";
    case KnifeApplyGateReason::MissingModelDefinition:
        return L"Knife swap fell back to finish-only: missing target knife definition.";
    case KnifeApplyGateReason::UnsupportedModelDefinition:
        return L"Knife swap fell back to finish-only: unsupported knife model definition.";
    case KnifeApplyGateReason::MissingWorldBinding:
        return L"Knife swap fell back to finish-only: missing writable world binding.";
    case KnifeApplyGateReason::MissingFirstPersonBinding:
        return L"Knife swap fell back to finish-only: missing a safe first-person owner.";
    case KnifeApplyGateReason::MissingAnimationFields:
        return L"Knife swap is waiting for a verified first-person animation owner; deploy/inspect remap remains disabled.";
    case KnifeApplyGateReason::AttackClassifierUncalibrated:
        return L"Knife deploy/inspect remap can stay active, but attack remap remains disabled until the classifier is calibrated.";
    case KnifeApplyGateReason::UnknownAttackState:
        return L"Knife attack state became unreadable; restored finish-only for safety.";
    case KnifeApplyGateReason::RestoreUnverified:
        return L"Knife swap fell back to finish-only: strict restore verification failed.";
    case KnifeApplyGateReason::ProfileXuidMismatch:
        return L"Knife finish-only slot waits for the watched player to match the configured XUID.";
    case KnifeApplyGateReason::MissingWatchedKnife:
    default:
        return L"Waiting for the watched player's active live knife.";
    }
}

const wchar_t* KnifeRenderVerificationReasonText(const KnifeRenderVerificationReason reason) noexcept
{
    switch (reason)
    {
    case KnifeRenderVerificationReason::Verified:
        return L"verified";
    case KnifeRenderVerificationReason::MissingWorldRenderState:
        return L"missing world render state";
    case KnifeRenderVerificationReason::MissingWorldRenderModel:
        return L"missing world target model";
    case KnifeRenderVerificationReason::WorldRenderModelMismatch:
        return L"world render model did not match the requested knife model";
    case KnifeRenderVerificationReason::WorldRenderMeshMismatch:
        return L"world render mesh mask did not match the requested knife state";
    case KnifeRenderVerificationReason::MissingFirstPersonRenderState:
        return L"missing first-person render state";
    case KnifeRenderVerificationReason::MissingFirstPersonRenderModel:
        return L"missing first-person target model";
    case KnifeRenderVerificationReason::UntrustedFirstPersonRender:
        return L"first-person render target remained an untrusted synthetic proxy";
    case KnifeRenderVerificationReason::FirstPersonRenderModelMismatch:
        return L"first-person render model did not match the requested knife model";
    case KnifeRenderVerificationReason::FirstPersonRenderMeshMismatch:
        return L"first-person render mesh mask did not match the requested knife state";
    default:
        return L"unknown";
    }
}

const wchar_t* KnifeFirstPersonOwnerKindText(const KnifeFirstPersonOwnerKind kind) noexcept
{
    switch (kind)
    {
    case KnifeFirstPersonOwnerKind::Providee:
        return L"providee";
    case KnifeFirstPersonOwnerKind::ViewModel:
        return L"viewmodel";
    case KnifeFirstPersonOwnerKind::WorldDiagnosticOnly:
        return L"world";
    case KnifeFirstPersonOwnerKind::None:
    default:
        return L"none";
    }
}
}
