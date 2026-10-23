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
    bool explicit_vanilla_finish = false;
    int quality = 0;
    int seed = 0;
    float wear = model::kMinimumCosmeticWear;
    std::string custom_name;
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
    int native_visual_output_entity_index = -1;
    std::uint32_t native_visual_output_handle = 0xFFFFFFFFu;
    std::uint32_t native_visual_identity_handle = 0xFFFFFFFFu;
    bool has_world_render = false;
    bool has_first_person_render = false;
    bool native_visual_evidence_verified = false;
    bool has_native_visual_output = false;
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
    NativeVisualRefreshPending,
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

struct SyntheticEconItemIdentity
{
    std::uint32_t high = 0;
    std::uint32_t low = 0;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return high != 0 && high != 0xFFFFFFFFu &&
            low != 0 && low != 0xFFFFFFFFu;
    }
};

// Demo stock slots can contain an empty C_EconItemView. Source 2 will not
// materialise a non-stock glove from the invalid FFFFFFFF:FFFFFFFF sentinel.
// Give the runtime-owned view a stable, process-independent identity instead.
// The high-word namespace mirrors the observable demo state produced by the
// reference application; the low word is an independent nSkinz FNV-1a hash.
SyntheticEconItemIdentity BuildSyntheticGloveEconItemIdentity(
    std::uint64_t xuid,
    int definition_index) noexcept;

GloveSlotMode ResolveGloveSlotMode(const nskinzcs2::model::GloveProfile& profile) noexcept;
bool GloveProfileRequiresModelDefinition(GloveSlotMode mode) noexcept;
GloveCarrierKind ResolveCanonicalGloveCarrierKind(
    GloveSlotMode mode,
    bool stable_wearable_available,
    bool pawn_glove_available) noexcept;
// Finish-only can use Pawn m_EconGloves. Model-and-finish may also use it when
// the demo/network stream has already materialized a known glove definition;
// an empty definition-zero stock slot remains ineligible.
bool CanUsePawnEconGlovesCarrier(
    GloveSlotMode mode,
    bool definition_readable,
    int current_definition_index,
    bool current_definition_is_glove,
    bool requested_definition_is_glove) noexcept;
// A readable definition-zero Pawn slot is owned by the demo/network stream.
// Writing runtime-owned glove attributes or creating/mirroring a Wearable over
// it leaves invalid client-only state behind when CS2 destroys the old Pawn on
// demo reload. Such a slot must be materialized by the temporary-demo rewrite.
bool ShouldAttemptRuntimeGloveWearableCreation(
    GloveSlotMode mode,
    bool pawn_definition_readable,
    int pawn_definition_index) noexcept;
// Identifies one safe CEconWearable creation attempt. A Source2Client provider
// may outlive several demo pawn instances, so provider generation alone must
// not suppress recreation after the pawn's full handle/pointer changes.
std::uint64_t BuildGloveWearableCreationEpoch(
    std::uint64_t provider_generation,
    std::uint32_t pawn_handle,
    std::uintptr_t pawn_identity) noexcept;
std::uint64_t GloveWearableRetryDelayMillis(std::uint32_t failure_count) noexcept;
GloveAppearanceRequest BuildGloveAppearanceRequest(const nskinzcs2::model::GloveProfile& profile) noexcept;
// Special painted gloves are native unusual-quality items. The editor's
// historical zero default must not serialize a normal-quality glove because
// CS2 will not materialize that state like a real equipped glove.
int ResolveCanonicalGloveQuality(int requested_quality, int paint_kit) noexcept;
// CS2 team numbers: 2 = Terrorists, 3 = Counter-Terrorists. A glove paint kit
// of zero means restore the stock team glove, not keep a special glove model
// with an empty paint kit.
int ResolveDefaultGloveDefinitionForTeam(int team_number) noexcept;
// Stock T/CT gloves are rendered by the Pawn's native glove component rather
// than a separately equipped CEconWearable. Keep this decision explicit so a
// vanilla model-and-finish request is not routed through the special-glove
// wearable factory.
bool IsStockTeamGloveRestoreRequest(
    const GloveAppearanceRequest& request,
    int team_number) noexcept;
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
