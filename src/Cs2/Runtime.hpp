#pragma once

#include "Core\AppState.hpp"
#include "Runtime\KnifeAppearance.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nskinzcs2
{
namespace runtime
{
enum class RuntimeRebuildReason
{
    None,
    ManualRefresh,
    ConfigChanged,
    DemoSeek,
    FullUpdate,
    PauseResume,
    WatchedPlayerSwitch,
    EntityListChanged,
};

enum class KnifeRefreshHelperPolicy
{
    Disabled,
    ManualOnly,
    ManualOrSeekOnly,
};

struct RuntimeRebuildInputs
{
    bool manual_refresh = false;
    bool config_changed = false;
    bool demo_seeked = false;
    bool full_update = false;
    bool pause_resumed = false;
    bool watched_player_switched = false;
    bool entity_list_changed = false;
};

struct RuntimeRebuildDecision
{
    bool rebuild = false;
    RuntimeRebuildReason reason = RuntimeRebuildReason::None;
};

enum class RuntimeGloveBindingChoice
{
    Unavailable,
    ReuseStable,
    ReuseRuntimeCreated,
    RecreateRuntimeCreated,
};

enum class GloveApplyMode
{
    None,
    TextureOnly,
    ModelAndTexture,
};

enum class CanonicalWeaponCarrierKind
{
    None,
    World,
    Hud,
    ViewModel,
    Providee,
};

enum class KnifeCarrierDiscoveryResult
{
    Unresolved,
    RealCarrierFound,
    DiscoverableIncompleteCarrier,
    NoDiscoverableCompleteCarrier,
};

enum class KnifeCarrierFactoryFeasibility
{
    Unknown,
    ViewmodelFactoryPresentWithoutInstance,
    NoViewmodelFactoryEvidence,
};

enum class KnifeViewmodelAbsenceAuditResult
{
    Skipped,
    MissingHandle,
    StaleHandle,
    ShellWithoutWritableCarrier,
    NonQualifyingCandidate,
    SyntheticAliasOnly,
};

enum class KnifePhase2TargetMode
{
    None,
    FirstPersonRender,
    WorldOnlyConsumer,
};

struct RuntimeGloveBindingInputs
{
    bool stable_binding_valid = false;
    bool runtime_created_binding_valid = false;
    bool client_factory_available = false;
};

struct WearableFactoryConfirmationInputs
{
    bool candidate_resolved = false;
    bool candidate_matches_previous = false;
    std::uint32_t previous_candidate_confirmations = 0;
    std::uint32_t previous_failure_confirmations = 0;
};

struct WearableFactoryConfirmationResult
{
    std::uint32_t candidate_confirmations = 0;
    std::uint32_t failure_confirmations = 0;
    bool publish_available = false;
    bool publish_unavailable = false;
};

struct CanonicalWeaponCarrierDecisionInputs
{
    bool world_valid = false;
    bool hud_valid = false;
    bool viewmodel_valid = false;
    bool providee_valid = false;
    bool world_hud_shared = false;
    bool world_viewmodel_shared = false;
    bool world_providee_shared = false;
    bool hud_viewmodel_shared = false;
    bool hud_providee_shared = false;
    bool viewmodel_providee_shared = false;
};

RuntimeRebuildDecision EvaluateRuntimeRebuildDecision(const RuntimeRebuildInputs& inputs) noexcept;
RuntimeGloveBindingChoice EvaluateGloveBindingChoice(const RuntimeGloveBindingInputs& inputs) noexcept;
WearableFactoryConfirmationResult EvaluateWearableFactoryConfirmation(
    const WearableFactoryConfirmationInputs& inputs) noexcept;
bool IsWearableModelSourceTypeReady(std::uint8_t model_type) noexcept;
// Matches the current CEconWearable client-think override shape without
// accepting a generic executable pointer. Runtime additionally validates both
// relative targets and the exact reflected CEconWearable primary vtable.
bool MatchesWearableDefinitionRefreshThunk(
    const std::uint8_t* bytes,
    std::size_t size) noexcept;
GloveApplyMode ResolveGloveApplyMode(const nskinzcs2::model::CosmeticProfile& profile) noexcept;
CanonicalWeaponCarrierKind ResolveCanonicalWeaponCarrierKind(const CanonicalWeaponCarrierDecisionInputs& inputs) noexcept;
bool CanonicalWeaponCarrierDecisionIsShared(const CanonicalWeaponCarrierDecisionInputs& inputs) noexcept;
bool RequiresExactHudViewmodelPath(bool watched_weapon_xuid, bool active_weapon) noexcept;
bool ShouldQueuePrimaryWeaponMaterialRefresh(bool watched_weapon_xuid, bool active_weapon) noexcept;
bool KnifeRefreshHelperPolicyArmsOnRebuildReason(
    KnifeRefreshHelperPolicy policy,
    RuntimeRebuildReason reason) noexcept;
bool ShouldPreserveWeaponAppearanceSignaturesOnRebuildReason(
    RuntimeRebuildReason reason) noexcept;
bool ShouldAllowWeaponRefreshHelperDispatch(
    bool knife_request,
    KnifeRefreshHelperPolicy policy,
    bool knife_epoch_armed) noexcept;
bool ShouldIncrementCustomEconReloadEventForMaterialRefresh() noexcept;
bool ShouldConsumeKnifeRefreshHelperEpoch(
    bool dispatched_knife_request,
    bool helper_succeeded) noexcept;
bool ShouldDispatchKnifeRefreshWithInPlaceAttributes(
    bool knife_request,
    bool needs_skin_refresh,
    bool live_attribute_values_match) noexcept;
bool ShouldUseKnifeVanillaTemporaryRefreshOverlay(
    bool knife_request,
    bool explicit_vanilla_finish,
    bool paint_attribute_present) noexcept;
bool ShouldAllowVerifiedInPlaceKnifeInitializationRefresh(
    bool knife_request,
    bool needs_skin_refresh,
    bool live_attribute_values_match,
    bool refresh_signature_matches) noexcept;
bool ShouldQueueWeaponMaterialRefresh(
    bool needs_refresh,
    bool completed_signature_matches,
    bool force_dispatch = false) noexcept;

bool ShouldUseStableKnifeFinishFastPath(
    bool knife_request,
    bool exact_appearance_pipeline,
    bool allow_model_rebind,
    bool refresh_signature_matches,
    bool definition_state_matches,
    bool attribute_state_matches) noexcept;
bool ShouldUseStableStandardWeaponMaterialFastPath(
    bool standard_weapon_request,
    bool refresh_signature_matches,
    bool definition_state_matches,
    bool attribute_state_matches,
    bool render_mesh_state_matches,
    bool hud_mesh_state_matches) noexcept;
bool ShouldAllowAttributeListVectorMutationForProfile(
    const nskinzcs2::model::CosmeticProfile& profile) noexcept;
bool ShouldInstallRuntimeOwnedAttributeListForProfile(
    const nskinzcs2::model::CosmeticProfile& profile) noexcept;
std::uint64_t ResolveWeaponMeshGroupMask(int base_item_definition_index, int paint_kit) noexcept;
bool WeaponSkinRefreshTargetNeedsMaterialRefresh(WeaponSkinRefreshTarget target) noexcept;
WeaponSkinRefreshTarget ResolveLinkedWeaponRefreshTarget(
    const nskinzcs2::model::CosmeticProfile& profile,
    WeaponSkinRefreshTarget preferred_target) noexcept;
bool UsesTemporaryWorldWeaponRefreshModel(const nskinzcs2::model::CosmeticProfile& profile) noexcept;
bool StandardWeaponNeedsItemIdHighReset(std::uint32_t current_item_id_high, bool uses_temporary_world_refresh_model) noexcept;
std::uint32_t BuildStandardWeaponMaterialCacheKey(
    const nskinzcs2::model::CosmeticProfile& profile,
    std::uint64_t xuid,
    int current_definition_index) noexcept;
std::vector<std::pair<std::uint16_t, float>> BuildWeaponRefreshAttributes(const nskinzcs2::model::CosmeticProfile& profile);
std::vector<std::uint16_t> BuildMissingWeaponAttributeDefinitionIndices(
    const nskinzcs2::model::CosmeticProfile& profile,
    const std::vector<std::uint16_t>& existing_definition_indices,
    const nskinzcs2::model::KeychainProfile* keychain_profile = nullptr);
std::vector<std::pair<std::uint16_t, float>> BuildWeaponRefreshAttributes(
    const nskinzcs2::model::CosmeticProfile& profile,
    const nskinzcs2::model::KeychainProfile* keychain_profile);
bool KnifeAnimationRemapTableHasDuplicateKeys();
bool KnifeAnimationRemapTableSupportsDefinitions(const std::vector<int>& knife_definition_indices);
bool KnifeAnimationRemapTableSupportsTransition(
    int source_definition_index,
    int target_definition_index,
    KnifeRemapActionKind action_kind);

#ifdef NSKINZCS2_TESTS
struct WeaponMeshMaskProbeResult
{
    int successful_updates = 0;
    double elapsed_milliseconds = 0;
    bool matching_readonly_is_noop = false;
    bool missing_dirty_data_rejected = false;
    bool mismatching_readonly_rejected = false;
    bool both_masks_verified = false;
};
WeaponMeshMaskProbeResult ProbeWeaponMeshMaskForTests();
bool ProbeStandardWeaponRenderOnlyBindingForTests();
bool ProbeStandardWeaponHudDiscoveryForTests();
bool ProbeStandardWeaponHudPendingGateForTests();
bool ProbeStandardWeaponAppearanceEpochForTests();
bool ProbeWeaponPaintCompatibilityForTests();

struct KnifeRepeatedRepairProbeResult
{
    int repaired_frames = 0;
    int redundant_refresh_requests = 0;
    bool completed_request_reused = false;
    bool changed_finish_queued = false;
    bool new_entity_epoch_queued = false;
    bool model_and_finish_preserved = false;
};
KnifeRepeatedRepairProbeResult ProbeKnifeRepeatedFieldRepairForTests();

struct StandardWeaponAttributeProbeResult
{
    bool matched_before = false;
    bool synchronized = false;
    bool matched_after = false;
    bool repeated_update_is_noop = false;
    bool demo_rewrite_repaired = false;
    bool vectors_and_node_metadata_preserved = false;
    bool original_values_restored = false;
    std::size_t changed_slots = 0;
};
bool ProbeStandardWeaponStockFinishForTests();
bool ProbeStandardWeaponMaterialCallsForTests(const std::filesystem::path& client_path);
StandardWeaponAttributeProbeResult ProbeStandardWeaponAttributeSynchronizationForTests(
    const std::vector<std::pair<std::uint16_t, float>>& static_attributes,
    const std::vector<std::pair<std::uint16_t, float>>& dynamic_attributes,
    const std::vector<std::pair<std::uint16_t, float>>& desired_attributes,
    bool callback_available = true,
    int fail_callback_number = -1);

struct Source2ClientGetterProbeResult
{
    bool success = false;
    bool used_forwarder = false;
    bool used_seh_call = false;
    void* resolved_pointer = nullptr;
    void* raw_target = nullptr;
    void* thunk_body = nullptr;
    void* real_object = nullptr;
    std::size_t vtable_offset = static_cast<std::size_t>(-1);
    void* final_getter_target = nullptr;
    std::wstring pattern_name;
    std::wstring failure_reason;
};

Source2ClientGetterProbeResult ProbeSource2ClientPointerGetterForTests(
    HMODULE owning_module,
    void* candidate_instance,
    void* method) noexcept;

enum class GloveWearableCandidateProbeStage
{
    PawnWearableHandles,
    OwnedLiveEntities,
    LooseGloveRenderables,
};

struct GloveWearableCandidateProbeInput
{
    GloveWearableCandidateProbeStage stage = GloveWearableCandidateProbeStage::PawnWearableHandles;
    bool has_item_view = false;
    bool exact_glove_definition = false;
    bool has_non_glove_definition = false;
    bool owner_matches_pawn = false;
    bool owner_matches_xuid = false;
    bool account_matches_xuid = false;
    bool glove_renderable = false;
};

struct GloveWearableCandidateProbeResult
{
    bool accepted = false;
    int score = -1;
    std::wstring selection_reason;
    std::wstring rejection_reason;
};

GloveWearableCandidateProbeResult ProbeGloveWearableCandidateRankingForTests(
    const GloveWearableCandidateProbeInput& input) noexcept;

struct DataFirstClientClassNodeProbeResult
{
    bool success = false;
    void* client_class = nullptr;
    void* raw_create_target = nullptr;
    std::size_t create_fn_offset = 0;
    std::size_t name_offset = 0;
    std::size_t next_offset = 0;
    int chain_length = 0;
    std::wstring create_field_resolution;
    std::wstring resolution_method;
    std::wstring failure_reason;
};

DataFirstClientClassNodeProbeResult ProbeWearableClientClassNodeInferenceForTests(
    void* module_base,
    std::size_t module_size,
    const char* expected_name) noexcept;
struct ExactWorldWeaponBindingProbeInput
{
    int owned_weapon_entity_index = -1;
    bool owned_weapon_has_item_view = false;
    int explicit_world_child_count = 0;
    int explicit_world_child_entity_index = -1;
    bool explicit_world_child_has_item_view = false;
    bool explicit_world_child_item_view_shared_with_owned = false;
    int hud_entity_index = -1;
};

struct ExactWorldWeaponBindingProbeResult
{
    bool success = false;
    int carrier_entity_index = -1;
    int render_entity_index = -1;
    int hud_entity_index = -1;
    bool render_entity_is_explicit_owner_child = false;
    bool carrier_entity_is_explicit_owner_child = false;
};

ExactWorldWeaponBindingProbeResult ProbeExactWorldWeaponApplicationBindingForTests(
    const ExactWorldWeaponBindingProbeInput& input) noexcept;

struct KnifeModelPathBindingMatchProbeInput
{
    bool state_active = false;
    bool state_real_viewmodel_verified = false;
    bool state_strict_verified = false;
    bool state_animation_owner_verified = false;
    bool state_direct_render_rebind_attempted = false;
    bool state_direct_render_rebind_succeeded = false;
    std::uint64_t state_xuid = 0;
    int state_world_entity_index = -1;
    int state_world_render_entity_index = -1;
    int state_first_person_entity_index = -1;
    int state_first_person_render_entity_index = -1;
    std::uintptr_t state_first_person_render_entity_ptr = 0;
    std::uintptr_t state_first_person_render_item_view_ptr = 0;
    std::uintptr_t state_first_person_render_model_state = 0;
    bool state_first_person_render_only = false;
    bool state_first_person_direct_render_source = false;
    bool state_synthetic_world_visual_isolation_active = false;
    int state_target_definition_index = 0;
    int state_verified_target_definition_index = 0;
    int state_verified_viewmodel_carrier_entity_index = -1;
    int state_verified_viewmodel_render_entity_index = -1;
    KnifeFirstPersonOwnerKind state_owner_kind = KnifeFirstPersonOwnerKind::None;
    std::uint64_t binding_xuid = 0;
    int binding_world_entity_index = -1;
    int binding_world_render_entity_index = -1;
    int binding_first_person_entity_index = -1;
    int binding_first_person_render_entity_index = -1;
    std::uintptr_t binding_first_person_render_entity_ptr = 0;
    std::uintptr_t binding_first_person_render_item_view_ptr = 0;
    std::uintptr_t binding_first_person_render_model_state = 0;
    bool binding_first_person_render_only = false;
    bool binding_first_person_direct_render_source = false;
    bool binding_first_person_owner_weapon_like = false;
    int binding_target_definition_index = 0;
    KnifeFirstPersonOwnerKind binding_owner_kind = KnifeFirstPersonOwnerKind::None;
};

bool ProbeKnifeModelPathStateMatchesBindingForTests(
    const KnifeModelPathBindingMatchProbeInput& input) noexcept;
bool ProbeAllowKnifeModelOnlyAnimationHookForTests(
    const KnifeModelPathBindingMatchProbeInput& input,
    bool allow_hud_alias_experiment) noexcept;
bool ProbeAllowKnifeVisualOnlyFallbackForTests(
    bool safe_verified_animation_owner,
    bool first_person_render_target_available,
    bool separate_first_person_render_target,
    bool first_person_render_only,
    bool skip_first_person_owner_carrier_write,
    bool allow_guarded_real_owner_synthetic_render_proxy) noexcept;
bool ProbeAllowKnifeSyntheticWorldVisualIsolationForTests(
    const KnifeModelPathBindingMatchProbeInput& input,
    bool allow_guarded_real_owner_synthetic_render_proxy) noexcept;
bool ProbeAllowGuardedKnifeSyntheticWorldRenderRebindForTests(
    const KnifeModelPathBindingMatchProbeInput& input,
    bool allow_guarded_real_owner_synthetic_render_proxy) noexcept;
bool ProbeShouldRollbackGuardedKnifeSyntheticFirstPersonRebindForTests(
    bool world_direct_rebind,
    bool world_direct_rebind_skipped) noexcept;
struct KnifePersistentOwnerReacquireProbeInput
{
    std::uint64_t xuid = 0;
    int world_entity_index = -1;
    int candidate_entity_index = -1;
    int candidate_definition_index = 0;
    KnifeFirstPersonOwnerKind candidate_owner_kind = KnifeFirstPersonOwnerKind::None;
    std::uint64_t first_observation_frame = 1;
    std::uint64_t second_observation_frame = 2;
};

struct KnifePersistentOwnerReacquireProbeResult
{
    bool first_pending = false;
    bool first_promoted = false;
    int first_confirm_hits = 0;
    bool second_pending = false;
    bool second_promoted = false;
    int second_confirm_hits = 0;
    int first_person_entity_index = -1;
    int animation_entity_index = -1;
    bool direct_render_source = true;
    bool render_only = true;
    KnifeFirstPersonOwnerKind owner_kind = KnifeFirstPersonOwnerKind::None;
};

KnifePersistentOwnerReacquireProbeResult ProbeKnifePersistentOwnerReacquireForTests(
    const KnifePersistentOwnerReacquireProbeInput& input) noexcept;
struct KnifeCarrierDiscoveryDecisionProbeInput
{
    bool complete_candidate_found = false;
    bool related_incomplete_candidate_found = false;
    bool factory_observed = false;
};

struct KnifeCarrierDiscoveryDecisionProbeResult
{
    KnifeCarrierDiscoveryResult carrier_result = KnifeCarrierDiscoveryResult::Unresolved;
    KnifeCarrierFactoryFeasibility factory_feasibility = KnifeCarrierFactoryFeasibility::Unknown;
    bool no_exposed_non_synthetic_render_owner = false;
};

KnifeCarrierDiscoveryDecisionProbeResult ProbeKnifeCarrierDiscoveryDecisionForTests(
    const KnifeCarrierDiscoveryDecisionProbeInput& input) noexcept;
struct KnifeCarrierPromotionProbeInput
{
    bool safe_linked_item_view = false;
    bool world_owned_proxy = false;
    bool direct_viewmodel_attachment = false;
    bool reverse_viewmodel_attachment = false;
    bool owner_chain_matches_player = false;
    bool exact_render_object_entity = false;
    bool exact_render_object_alias = false;
    bool shell_direct_viewmodel_attachment = false;
    bool shell_reverse_viewmodel_attachment = false;
    bool shell_owner_chain_matches_player = false;
    bool shell_relation_to_render_object = false;
    bool world_neighborhood_match = false;
    bool shell_world_neighborhood_match = false;
};

struct KnifeCarrierPromotionProbeResult
{
    bool promotable_relation = false;
    bool discovery_relevant = false;
    bool accept_writable_carrier = false;
};

KnifeCarrierPromotionProbeResult ProbeResolveKnifeCarrierPromotionForTests(
    const KnifeCarrierPromotionProbeInput& input) noexcept;
struct ClientClassDescriptorProbeResult
{
    bool viewmodel_like = false;
    bool weapon_like = false;
    bool econ_like = false;
    bool factory_observed = false;
};

ClientClassDescriptorProbeResult ProbeClientClassDescriptorClassificationForTests(
    const char* class_name,
    bool trusted_create) noexcept;
KnifeViewmodelAbsenceAuditResult ProbeClassifyKnifeViewmodelAbsenceAuditForTests(
    bool field_present,
    std::uint32_t raw_handle,
    bool raw_entity_visible,
    bool handle_entity_synthetic,
    bool shared_with_render_object,
    bool handle_entity_weapon_like,
    bool handle_entity_item_view_safe,
    int reverse_viewmodel_ref_count) noexcept;
struct KnifePhase2TargetSelectionProbeInput
{
    bool has_real_first_person_render_target = false;
    bool has_real_first_person_owner = false;
    bool no_exposed_non_synthetic_render_owner = false;
    KnifeViewmodelAbsenceAuditResult viewmodel_absence_result =
        KnifeViewmodelAbsenceAuditResult::Skipped;
    int consumer_clusters = 0;
    int entity_bound_clusters = 0;
    int anonymous_clusters = 0;
    bool single_consumer_is_entity_bound_world = false;
    bool caller_matches_valid_hit = false;
};

KnifePhase2TargetMode ProbeResolveKnifePhase2TargetSelectionForTests(
    const KnifePhase2TargetSelectionProbeInput& input) noexcept;

struct KnifeWorldOnlyRenderVerificationProbeInput
{
    bool world_render_state_resolved = false;
    bool world_render_expected_model_resolved = false;
    bool world_render_model_matches_target = false;
    bool world_render_handle_matches_target_or_donor = false;
    bool world_render_mesh_matches_target = false;
    int resource_hits = 0;
    bool resource_readback = false;
};

bool ProbeEvaluateKnifeWorldOnlyRenderVerificationForTests(
    const KnifeWorldOnlyRenderVerificationProbeInput& input) noexcept;

struct KnifePhase2ArmProbeInput
{
    KnifePhase2TargetMode target_mode = KnifePhase2TargetMode::None;
    int world_render_entity_index = -1;
    int first_person_render_entity_index = -1;
    bool first_person_render_entity_synthetic = false;
};

struct KnifePhase2ArmProbeResult
{
    bool armed = false;
    KnifePhase2TargetMode target_mode = KnifePhase2TargetMode::None;
    int render_entity_index = -1;
    bool synthetic_render_proxy = false;
};

KnifePhase2ArmProbeResult ProbeKnifePhase2ArmGateForTests(
    const KnifePhase2ArmProbeInput& input) noexcept;

std::wstring ProbeBuildKnifeCarrierRuntimeReasonTextForTests(
    bool no_exposed_non_synthetic_render_owner,
    KnifeCarrierDiscoveryResult carrier_result,
    KnifeCarrierFactoryFeasibility factory_feasibility);
bool ProbeAllowGuardedKnifeSyntheticRenderProxyForTests(
    bool candidate,
    bool aliases_hud_proxy,
    bool restore_state_readable,
    bool allow_hud_alias_experiment) noexcept;
std::uint64_t ProbeResolveKnifeSyntheticProxyDesiredMeshGroupMaskForTests(
    bool original_first_person_mesh_group_mask_valid,
    std::uint64_t original_first_person_mesh_group_mask,
    bool live_first_person_mesh_group_mask_valid,
    std::uint64_t live_first_person_mesh_group_mask,
    bool world_render_mesh_group_mask_valid,
    std::uint64_t world_render_mesh_group_mask) noexcept;
bool ProbeShouldBlockUnsafeKnifeSyntheticRenderProxyCarrierWriteForTests(
    bool first_person_render_only,
    bool skip_first_person_owner_carrier_write,
    int first_person_render_entity_index,
    std::uintptr_t first_person_render_entity,
    std::uintptr_t first_person_render_item_view) noexcept;
bool ProbeKnifeRefreshGuardBindingRestoresOnAbandonForTests() noexcept;
bool ProbeKnifeRefreshAttachmentPromotionAllowedForTests(
    const nskinzcs2::model::CosmeticProfile& profile,
    const nskinzcs2::model::KeychainProfile* keychain_profile = nullptr) noexcept;
bool ProbePromotedWeaponRefreshAttachmentRestoresOnAbandonForTests(
    const nskinzcs2::model::CosmeticProfile& profile,
    const nskinzcs2::model::KeychainProfile* keychain_profile = nullptr) noexcept;
bool ProbePassiveWatchedKnifeTraceGateForTests(
    const nskinzcs2::runtime::KnifeRuntimeContext& context,
    std::uint64_t xuid) noexcept;
std::uint64_t ProbePreferredKnifeTargetXuidForTests(
    const nskinzcs2::model::ConfigDocument& config,
    const std::vector<std::uint64_t>& live_pawn_xuids) noexcept;
std::uint64_t ProbeResolveWatchedKnifeTargetXuidForTests(
    bool observer_resolved,
    bool observer_chain_present,
    std::uint64_t observer_target_xuid,
    bool bridge_resolved,
    std::uint64_t bridge_xuid,
    std::uint64_t heuristic_watched_xuid) noexcept;
#endif
}

class RuntimeController
{
public:
    RuntimeController(AppState& state, HMODULE module_instance);
    ~RuntimeController();

    void Start();
    void Stop();
    static std::vector<std::string> BuildVersionCandidates(const std::string& prefix, int min_value, int max_value, const std::vector<std::string>& exact = {});
    static std::vector<std::string> FilterVersionCandidates(const std::vector<std::string>& discovered_names, const std::string& prefix, int min_value, int max_value);

private:
    struct ResolvedInterface
    {
        std::wstring module_name;
        std::string version;
        void* address = nullptr;
    };

    struct ModuleInterfaces
    {
        std::wstring module_name;
        std::vector<std::string> names;
    };

    struct Bindings
    {
        ResolvedInterface source2_client;
        ResolvedInterface engine_to_client;
        ResolvedInterface engine_cvar;
        ResolvedInterface game_ui_service;
        ResolvedInterface scene_system;
        ResolvedInterface resource_system;
        ResolvedInterface resource_handle_utils;
        ResolvedInterface file_system;
        ResolvedInterface input_system;
        ResolvedInterface schema_system;
        ResolvedInterface localize;
        std::vector<ModuleInterfaces> interface_dump;

        bool AnyMissing() const noexcept
        {
            return source2_client.address == nullptr ||
                engine_to_client.address == nullptr ||
                input_system.address == nullptr ||
                schema_system.address == nullptr ||
                game_ui_service.address == nullptr;
        }
    };

    static std::wstring CurrentProcessPath();
    static bool IsCs2Process(const std::wstring& process_path);
    static bool IsHlaeLoaded();
    static std::vector<std::string> EnumerateInterfaceNames(const wchar_t* module_name);
    static void* ResolveCreateInterface(const wchar_t* module_name, const std::vector<std::string>& versions, std::string& out_version);
    static std::wstring ToWide(const std::string& value);
    static runtime::Status BuildStatusSkeleton();

    void WorkerLoop();
    void WorkerLoopImpl();
    void ReportWorkerThreadFailure(const wchar_t* message);
    void TryInstallGlobalResourceSwapperEarly();
    Bindings TryResolveBindings(runtime::Status& status);
    bool TryInstallRuntimeHook(runtime::Status& status);
    bool TryRunManualCarrierOnlyProbe(const Bindings& bindings, runtime::Status& status);
    void ConsumeDemoKeychainRefresh(runtime::Status& status);
    void ConsumeManualRefresh(runtime::Status& status);
    void AppendHookTelemetry(runtime::Status& status) const;
    void LogStatusIfChanged(const runtime::Status& status);

    AppState& state_;
    HMODULE module_instance_ = nullptr;
    std::thread worker_;
    std::atomic_bool stop_requested_ {false};
    std::atomic_bool global_swapper_early_attempted_ {false};
    std::atomic_bool hook_attempted_ {false};
    std::atomic_bool hook_installed_ {false};
    std::atomic_bool hook_activation_requested_ {false};
    std::atomic_bool carrier_probe_requested_ {false};
    std::atomic_bool logged_unavailable_refresh_ {false};
    HANDLE automation_refresh_event_ = nullptr;
    HANDLE automation_demo_command_event_ = nullptr;
    std::wstring global_swapper_early_reason_;
    std::wstring global_swapper_early_route_;
    std::wstring last_logged_status_;
};
}
