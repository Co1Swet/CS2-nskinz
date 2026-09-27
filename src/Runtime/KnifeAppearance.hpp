#pragma once

#include "Core/AppState.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace nskinzcs2::runtime
{
enum class KnifeSlotMode
{
    None,
    FinishOnly,
    ModelAndFinish,
};

enum class KnifeRemapActionKind
{
    Deploy,
    Inspect,
    LightAttack,
    HeavyAttack,
};

enum class KnifeFirstPersonOwnerKind
{
    None,
    Providee,
    ViewModel,
    WorldDiagnosticOnly,
};

enum class KnifeApplyGateReason
{
    ReadyForFinishOnly,
    ReadyForModelAndFinish,
    MissingModelDefinition,
    UnsupportedModelDefinition,
    MissingWorldBinding,
    MissingFirstPersonBinding,
    MissingAnimationFields,
    AttackClassifierUncalibrated,
    UnknownAttackState,
    RestoreUnverified,
    ProfileXuidMismatch,
    MissingWatchedKnife,
};

enum class KnifeRenderVerificationReason
{
    Verified,
    MissingWorldRenderState,
    MissingWorldRenderModel,
    WorldRenderModelMismatch,
    WorldRenderMeshMismatch,
    MissingFirstPersonRenderState,
    MissingFirstPersonRenderModel,
    UntrustedFirstPersonRender,
    FirstPersonRenderModelMismatch,
    FirstPersonRenderMeshMismatch,
};

struct KnifeAppearanceRequest
{
    std::uint64_t xuid = 0;
    KnifeSlotMode mode = KnifeSlotMode::None;
    int model_definition = 0;
    int paint_kit = 0;
    bool explicit_vanilla_finish = false;
    int quality = 0;
    int seed = 0;
    int stat_trak = 0;
    float wear = 0.0000001f;
    std::string custom_name;
};

struct KnifeRuntimeContext
{
    std::uint64_t watched_xuid = 0;
    std::uint64_t observer_target_xuid = 0;
    const wchar_t* bridge_source = L"none";
    int live_definition_index = 0;
    int world_entity_index = -1;
    int providee_entity_index = -1;
    int viewmodel_entity_index = -1;
    int viewmodel_shell_entity_index = -1;
    int viewmodel_carrier_entity_index = -1;
    int viewmodel_render_entity_index = -1;
    int bridged_controller_entity_index = -1;
    int bridged_pawn_entity_index = -1;
    int observer_target_pawn_entity_index = -1;
    int bridge_active_weapon_entity_index = -1;
    int observer_active_weapon_entity_index = -1;
    int real_viewmodel_entity_index = -1;
    int real_viewmodel_weapon_entity_index = -1;
    int direct_first_person_render_entity_index = -1;
    int direct_first_person_animation_owner_entity_index = -1;
    int bridge_pawn_health = 0;
    int world_definition_index = 0;
    int providee_definition_index = 0;
    int viewmodel_definition_index = 0;
    int direct_first_person_render_definition_index = 0;
    int direct_first_person_animation_owner_definition_index = 0;
    int first_person_source_definition_index = 0;
    bool demo_playing = false;
    bool schema_ready = false;
    bool entity_system_ready = false;
    bool watched_target = false;
    bool observer_chain_present = false;
    bool observer_in_eye_valid = false;
    bool bridge_cache_hit = false;
    bool bridge_pawn_alive = false;
    bool bridge_active_weapon_is_knife = false;
    bool observer_active_weapon_is_knife = false;
    bool active_weapon_is_knife = false;
    bool world_carrier_valid = false;
    bool world_item_view_valid = false;
    bool providee_carrier_valid = false;
    bool viewmodel_carrier_valid = false;
    bool real_viewmodel_services_present = false;
    bool real_viewmodel_handle_valid = false;
    bool real_viewmodel_handle_invalid = false;
    bool real_viewmodel_weapon_matches_active_weapon = false;
    bool real_viewmodel_active_weapon_verified_knife = false;
    bool hud_proxy_fallback_allowed = false;
    bool direct_first_person_render_valid = false;
    bool direct_first_person_render_only = false;
    void* direct_first_person_render_entity = nullptr;
    bool direct_first_person_animation_owner_valid = false;
    bool first_person_source_valid = false;
    bool knife_sync_ready = false;
    bool resource_query_bridge_ready = false;
    bool resource_query_target_handle_ready = false;
    bool resource_model_swap_ready = false;
    bool resource_model_path_readback_changed = false;
    bool resource_world_model_matches_target = false;
    bool resource_first_person_model_matches_target = false;
    bool animation_hook_ready = false;
    bool attack_classifier_ready = false;
    int resource_query_redirect_pairs = 0;
    int resource_query_redirect_hits = 0;
    const wchar_t* viewmodel_resolution_source = L"none";
    std::uint32_t viewmodel_absence_audit_result = 0;
    KnifeFirstPersonOwnerKind first_person_owner_kind = KnifeFirstPersonOwnerKind::None;
};

struct KnifeApplyGateInputs
{
    KnifeAppearanceRequest request {};
    KnifeRuntimeContext context {};
};

struct KnifeApplyGateResult
{
    bool allow_finish_writes = false;
    bool knife_sync_ready = false;
    bool degraded = false;
    bool allow_model_writes = false;
    bool allow_animation_remap = false;
    bool animation_binding_ready = false;
    bool animation_requires_strict_verification = false;
    KnifeSlotMode resolved_mode = KnifeSlotMode::None;
    int resolved_model_definition = 0;
    KnifeApplyGateReason reason = KnifeApplyGateReason::MissingWatchedKnife;
};

struct KnifeAppearancePipelineRoute
{
    bool use_exact_appearance_pipeline = false;
    bool allow_model_rebind = false;
};

struct KnifeRenderVerificationInputs
{
    bool world_render_state_resolved = false;
    bool world_render_expected_model_resolved = false;
    bool world_render_model_matches_target = false;
    bool world_render_mesh_matches_target = false;
    bool first_person_render_state_resolved = false;
    bool first_person_render_expected_model_resolved = false;
    bool first_person_render_model_matches_target = false;
    bool first_person_render_mesh_matches_target = false;
    bool first_person_render_untrusted = false;
};

struct KnifeRenderVerificationResult
{
    bool verified = false;
    KnifeRenderVerificationReason reason = KnifeRenderVerificationReason::MissingWorldRenderState;
};

struct KnifeAnimationRemapKey
{
    int source_definition = 0;
    int target_definition = 0;
    KnifeRemapActionKind action_kind = KnifeRemapActionKind::Deploy;

    bool operator==(const KnifeAnimationRemapKey& other) const noexcept
    {
        return source_definition == other.source_definition &&
            target_definition == other.target_definition &&
            action_kind == other.action_kind;
    }
};

KnifeSlotMode ResolveKnifeSlotMode(const nskinzcs2::model::KnifeProfile& profile) noexcept;
bool KnifeProfileRequiresModelDefinition(KnifeSlotMode mode) noexcept;
KnifeAppearanceRequest BuildKnifeAppearanceRequest(const nskinzcs2::model::KnifeProfile& profile) noexcept;
// Production cosmetic request: a model selection must not alter material work.
KnifeAppearanceRequest BuildKnifeFinishRequest(const nskinzcs2::model::KnifeProfile& profile) noexcept;
KnifeApplyGateResult EvaluateKnifeApplyGate(const KnifeApplyGateInputs& inputs) noexcept;
KnifeAppearancePipelineRoute ResolveKnifeAppearancePipelineRoute(
    const KnifeApplyGateResult& gate) noexcept;
KnifeRenderVerificationResult EvaluateKnifeRenderVerification(const KnifeRenderVerificationInputs& inputs) noexcept;
bool IsDefaultKnifeDefinition(int definition_index) noexcept;
bool ShouldApplyKnifeModelDefinitionOverride(
    const KnifeAppearanceRequest& request,
    int live_definition_index) noexcept;
bool KnifeRuntimeContextHasAuthoritativeWatchedKnife(const KnifeRuntimeContext& context) noexcept;
int ResolveKnifeRuntimeOverrideDefinition(
    const KnifeAppearanceRequest& request,
    int live_definition_index) noexcept;
// Empty/default demo knife views use an invalid econ identity.  A model swap
// needs a stable target identity before Source 2 builds the target subclass.
// Existing real inventory identities are preserved.
std::uint32_t BuildSyntheticKnifeEconItemHigh(int target_definition_index) noexcept;
std::uint32_t ResolveKnifeEconItemHigh(
    std::uint32_t source_item_id_high,
    int target_definition_index) noexcept;
bool IsKnifeEconItemHighCompatible(
    std::uint32_t item_id_high,
    int target_definition_index) noexcept;
std::uint64_t BuildKnifeProfileApplySignature(
    const KnifeAppearanceRequest& request,
    std::uint64_t xuid,
    int live_definition_index,
    std::uint64_t target_tag) noexcept;
const wchar_t* KnifeApplyGateReasonText(KnifeApplyGateReason reason) noexcept;
const wchar_t* KnifeRenderVerificationReasonText(KnifeRenderVerificationReason reason) noexcept;
const wchar_t* KnifeFirstPersonOwnerKindText(KnifeFirstPersonOwnerKind kind) noexcept;
}
