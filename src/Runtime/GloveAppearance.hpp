#pragma once

#include "Core/AppState.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace nskinzcs2::runtime
{
enum class GloveSlotMode
{
    None,
    FinishOnly,
    ModelAndFinish,
};

enum class GloveCarrierKind
{
    None,
    PawnEconGloves,
    PawnEconGlovesRenderFallback,
    Wearable,
};

struct GloveAppearanceRequest
{
    std::uint64_t xuid = 0;
    GloveSlotMode mode = GloveSlotMode::None;
    int model_definition = 0;
    int paint_kit = 0;
    int quality = 0;
    int seed = 0;
    float wear = 0.0000001f;
};

struct ResolvedGloveCarrier
{
    std::uint64_t xuid = 0;
    int pawn_entity_index = -1;
    int glove_entity_index = -1;
    std::uint32_t glove_handle = 0xFFFFFFFFu;
    int current_definition_index = 0;
    bool runtime_created = false;
    bool valid = false;
};

struct ResolvedGloveRenderBinding
{
    int carrier_entity_index = -1;
    int world_render_entity_index = -1;
    int first_person_render_entity_index = -1;
    bool has_world_render = false;
    bool has_first_person_render = false;
    bool valid = false;
};

struct ResolvedModelIdentity
{
    std::uintptr_t live_glove_model_state = 0;
    std::uintptr_t first_person_arms_model_state = 0;
    std::uint64_t live_glove_model_handle = 0;
    std::uint64_t first_person_arms_model_handle = 0;
    std::uint64_t live_glove_model_name_token = 0;
    std::uint64_t first_person_arms_model_name_token = 0;
    std::uint64_t live_glove_skeleton_identity_hash = 0;
    std::uint64_t first_person_arms_skeleton_identity_hash = 0;
    std::uint64_t target_world_skeleton_identity_hash = 0;
    std::uint64_t target_view_skeleton_identity_hash = 0;
    std::uint64_t live_glove_material_resource_identity_hash = 0;
    std::uint64_t first_person_arms_material_resource_identity_hash = 0;
    std::uint64_t target_world_material_resource_identity_hash = 0;
    std::uint64_t target_view_material_resource_identity_hash = 0;
    std::uint32_t live_glove_material_group = 0;
    std::uint32_t first_person_arms_material_group = 0;
    std::string live_glove_model_name;
    std::string first_person_arms_model_name;
    std::string live_glove_model_resource_path;
    std::string first_person_arms_model_resource_path;
    std::string target_world_model_resource_path;
    std::string target_view_model_resource_path;
    std::string target_view_model_path;
    std::string target_world_model_path;
    bool live_glove_model_state_resolved = false;
    bool first_person_arms_model_state_resolved = false;
    bool live_glove_model_handle_resolved = false;
    bool first_person_arms_model_handle_resolved = false;
    bool live_glove_model_name_token_resolved = false;
    bool first_person_arms_model_name_token_resolved = false;
    bool live_glove_model_name_resolved = false;
    bool first_person_arms_model_name_resolved = false;
    bool live_glove_material_group_resolved = false;
    bool first_person_arms_material_group_resolved = false;
    bool live_glove_skeleton_identity_resolved = false;
    bool first_person_arms_skeleton_identity_resolved = false;
    bool target_world_skeleton_identity_resolved = false;
    bool target_view_skeleton_identity_resolved = false;
    bool live_glove_material_resource_identity_resolved = false;
    bool first_person_arms_material_resource_identity_resolved = false;
    bool target_world_material_resource_identity_resolved = false;
    bool target_view_material_resource_identity_resolved = false;
    bool target_view_matches_first_person_skeleton = false;
    bool target_model_known = false;
};

enum class GloveCompatibilityReason
{
    Compatible,
    MissingCarrier,
    MissingModelDefinition,
    PaintIncompatible,
    MissingRenderBinding,
    MissingModelIdentity,
    SkeletonMismatch,
};

struct GloveCompatibilityInputs
{
    GloveSlotMode mode = GloveSlotMode::None;
    GloveCarrierKind carrier_kind = GloveCarrierKind::None;
    int current_definition_index = 0;
    int requested_model_definition = 0;
    int compatible_paint_definition = 0;
    bool carrier_valid = false;
    bool render_binding_valid = false;
    bool allow_render_binding_after_apply = false;
    bool guarded_first_person_model_rebind_verified = false;
    bool live_glove_model_state_resolved = false;
    bool first_person_arms_model_state_resolved = false;
    bool live_glove_model_handle_resolved = false;
    bool first_person_arms_model_handle_resolved = false;
    bool live_glove_material_group_resolved = false;
    bool first_person_arms_material_group_resolved = false;
    bool live_glove_skeleton_identity_resolved = false;
    bool first_person_arms_skeleton_identity_resolved = false;
    bool target_view_skeleton_identity_resolved = false;
    bool target_view_matches_first_person_skeleton = false;
    bool target_model_known = false;
};

struct GloveCompatibilityResult
{
    bool compatible = false;
    GloveCompatibilityReason reason = GloveCompatibilityReason::MissingCarrier;
};

enum class GloveRenderVerificationReason
{
    Verified,
    CarrierFieldMismatch,
    ReapplyNotTriggered,
    MissingLiveRenderState,
    MissingHudRenderState,
    ModelBindingUnchanged,
    MaterialBindingUnchanged,
    MaterialBindingMismatch,
};

struct GloveRenderVerificationInputs
{
    GloveSlotMode mode = GloveSlotMode::None;
    bool pawn_econ_gloves_reapply_path = false;
    bool carrier_field_readback_matched = false;
    bool reapply_triggered = false;
    bool has_first_person_render = false;
    bool requested_model_changed = false;
    bool requested_finish_changed = false;
    bool live_render_state_resolved = false;
    bool first_person_render_state_resolved = false;
    bool live_model_binding_changed = false;
    bool first_person_model_binding_changed = false;
    bool live_material_binding_changed = false;
    bool first_person_material_binding_changed = false;
    bool first_person_binding_changed = false;
    bool finish_binding_confirmed = false;
    bool live_material_resource_matches_target = false;
    bool first_person_material_resource_matches_target = false;
};

struct GloveRenderVerificationResult
{
    bool verified = false;
    GloveRenderVerificationReason reason = GloveRenderVerificationReason::MissingLiveRenderState;
};

struct GloveApplyResult
{
    bool applied = false;
    bool verified = false;
    GloveSlotMode mode = GloveSlotMode::None;
    bool runtime_created = false;
    std::wstring issue;
};

GloveSlotMode ResolveGloveSlotMode(const nskinzcs2::model::GloveProfile& profile) noexcept;
bool GloveProfileRequiresModelDefinition(GloveSlotMode mode) noexcept;
GloveCarrierKind ResolveCanonicalGloveCarrierKind(
    GloveSlotMode mode,
    bool stable_wearable_available,
    bool pawn_glove_available) noexcept;
GloveAppearanceRequest BuildGloveAppearanceRequest(const nskinzcs2::model::GloveProfile& profile) noexcept;
// Returns an already matching slot or the first existing invalid slot. It
// never selects an append position because CS2 owns capacity/allocator state.
int SelectReusableWearableHandleSlot(
    const std::uint32_t* handles,
    std::size_t handle_count,
    std::uint32_t requested_handle) noexcept;
// The formal CS2 model binder is only valid for a real Wearable with a verified
// scene-node/model-state context and no live model handle. It is never a Pawn
// or synthetic-render fallback. The opaque CSkeletonInstance +0xA0 state is
// not an "empty render instance" flag; CS2's formal entry supports both of its
// branches.
bool ShouldInitializeWearableModel(
    GloveSlotMode mode,
    bool real_wearable,
    int wearable_entity_index,
    bool target_model_known,
    std::uint64_t live_model_handle,
    bool formal_bind_context_verified) noexcept;
GloveCompatibilityResult EvaluateGloveCompatibility(const GloveCompatibilityInputs& inputs) noexcept;
const wchar_t* GloveCompatibilityReasonText(GloveCompatibilityReason reason) noexcept;
GloveRenderVerificationResult EvaluateGloveRenderVerification(const GloveRenderVerificationInputs& inputs) noexcept;
const wchar_t* GloveRenderVerificationReasonText(GloveRenderVerificationReason reason) noexcept;
}
