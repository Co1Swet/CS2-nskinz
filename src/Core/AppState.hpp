#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nskinzcs2::model
{
inline constexpr float kMinimumCosmeticWear = 0.0000001f;
inline constexpr float kMaximumCosmeticWear = 1.0f;

inline float NormalizeCosmeticWear(const float wear) noexcept
{
    if (!std::isfinite(wear))
    {
        return kMinimumCosmeticWear;
    }

    return std::clamp(wear, kMinimumCosmeticWear, kMaximumCosmeticWear);
}

inline bool CosmeticWearMatchesExactly(const float actual, const float expected) noexcept
{
    return std::bit_cast<std::uint32_t>(actual) == std::bit_cast<std::uint32_t>(expected);
}

struct StickerProfile
{
    int kit_id = 0;
    float wear = 0.0000001f;
    float scale = 1.0f;
    float rotation = 0.0f;
};

struct CosmeticProfile
{
    std::string name = "Default";
    bool enabled = false;
    std::uint64_t xuid = 0;
    int base_item = 7;
    int override_item = 0;
    int paint_kit = 0;
    // Persisted intent bit: paint_kit=0 by itself means "do not modify the
    // finish"; paint_kit=0 plus this bit means "remove the finish".
    bool explicit_vanilla_finish = false;
    int quality = 0;
    int seed = 0;
    int stat_trak = 0;
    float wear = kMinimumCosmeticWear;
    std::string custom_name;
    std::array<StickerProfile, 5> stickers {};
};

enum class KnifeProfileMode
{
    FinishOnly,
    ModelAndFinish,
};

struct KnifeProfile
{
    std::string name = "Default Knife";
    bool enabled = false;
    std::uint64_t xuid = 0;
    KnifeProfileMode mode = KnifeProfileMode::FinishOnly;
    int model_definition = 0;
    int paint_kit = 0;
    bool explicit_vanilla_finish = false;
    int quality = 0;
    int seed = 0;
    int stat_trak = 0;
    float wear = 0.0000001f;
    std::string custom_name;
};

enum class GloveProfileMode
{
    FinishOnly,
    ModelAndFinish,
};

struct GloveProfile
{
    std::string name = "Default Gloves";
    bool enabled = false;
    std::uint64_t xuid = 0;
    GloveProfileMode mode = GloveProfileMode::FinishOnly;
    int model_definition = 0;
    int paint_kit = 0;
    bool explicit_vanilla_finish = false;
    int quality = 0;
    int seed = 0;
    float wear = kMinimumCosmeticWear;
};

struct KeychainProfile
{
    std::string name = "Default Keychain";
    bool enabled = false;
    std::uint64_t xuid = 0;
    int base_item = 7;
    int keychain_kit = 0;
    bool explicit_clear = false;
};

struct DemoMirvPlayerEntry
{
    std::uint64_t xuid = 0;
    std::string label;
};

struct MapRemapProfile
{
    bool enabled = false;
    // Keep the demo's original GameSession map name for Panorama/POV radar
    // while the world-loading messages are redirected to the workshop map.
    bool use_source_radar = true;
    // Radar textures shipped by Workshop authors do not use one universal
    // black level. Keep these user-adjustable so dark Nuke floors can be
    // retained without forcing the opaque editor grid back into the HUD.
    int radar_black_tolerance = 12;
    int radar_dark_detail_radius = 12;
    int radar_background_alpha_percent = 0;
    // Blank means: capture the currently loaded demo map when Update is pressed.
    std::string source_map_name;
    std::string workshop_id;
    std::string workshop_title;
    std::string addon_name;
    std::string target_map_name;
    // Filled by Workshop discovery when the selected map ships a separate
    // compiled 3D skybox package (for example de_nuke_skybox02_d).
    std::string target_skybox_resource_prefix;
    // Generated only by the isolated rundll32 control-panel process.  The
    // injected CS2 runtime never decodes or compiles radar textures; it only
    // validates and mounts this already compiled native Source 2 resource.
    // The request path is the Workshop resource Panorama asks for.  The
    // overlay path is a content-addressed alias compiled by nSkinz; keeping
    // both prevents Source 2 from reusing a stale resource after a slider
    // change.
    std::string radar_overlay_request_virtual_path;
    std::string radar_overlay_virtual_path;
    std::string radar_overlay_root_path;
    std::string radar_overlay_file_path;
};

struct ConfigDocument
{
    int version = 7;
    std::vector<CosmeticProfile> profiles;
    std::vector<KnifeProfile> knife_profiles;
    std::vector<GloveProfile> glove_profiles;
    std::vector<KeychainProfile> keychain_profiles;
    MapRemapProfile map_remap;
    bool experimental_allow_hud_alias_synthetic_proxy_model_write = false;
};

struct NamedId
{
    int id = 0;
    std::string name;
};

struct VisualInfo
{
    std::string world_model;
    std::string view_model;
    std::string resolved_world_model_resource_path;
    std::string resolved_view_model_resource_path;
    std::string knife_family;
    std::string killfeed_icon;
    bool knife_identity_resolved = false;
    bool is_knife = false;
    bool is_glove = false;
};

struct KeychainVisualInfo
{
    std::string model_path;
    std::string resolved_model_resource_path;
};

struct Catalog
{
    std::vector<NamedId> base_items;
    std::vector<NamedId> knife_items;
    std::vector<NamedId> glove_items;
    std::vector<NamedId> weapon_paint_kits;
    // Parsed from each paint kit, not inferred from its numeric ID or age.
    // Missing entry means metadata is unavailable (e.g. an old IPC snapshot).
    std::unordered_map<int, bool> weapon_paint_uses_legacy_model;
    // Exact [paint internal name]weapon internal name pairs from local game
    // collections/loot lists. Definition -> sorted unique paint IDs.
    // Missing definition means unknown, never "all paints are compatible".
    std::unordered_map<int, std::vector<int>> weapon_paint_compatibility;
    std::vector<NamedId> glove_paint_kits;
    std::unordered_map<int, int> glove_paint_compatibility;
    std::vector<NamedId> sticker_kits;
    std::vector<NamedId> keychain_kits;
    std::vector<NamedId> qualities;
    std::unordered_map<int, VisualInfo> visuals;
    std::unordered_map<int, KeychainVisualInfo> keychain_visuals;
    bool runtime_populated = false;
    std::vector<std::string> warnings;
};
}

namespace nskinzcs2::runtime
{
enum class Availability
{
    Unknown,
    MissingProcess,
    MissingHlae,
    MissingInterfaces,
    MissingHook,
    HookedPendingDemoGuard,
    Ready
};

enum class WeaponSkinRefreshState
{
    Disabled,
    Ready,
    Faulted,
    NeedsManualRearm,
};

enum class WeaponSkinRefreshTarget
{
    CanonicalItemView,
    PrimaryItemView,
    ExactHudItemView,
    SecondaryItemView,
    ViewModelItemView,
    HeuristicViewModelItemView,
    ProvideeItemView,
    HeuristicProvideeItemView,
    PreviewItemView,
};

enum class DemoKeychainState
{
    Idle,
    WaitingCooldown,
    WaitingStableFrames,
    WaitingFactory,
    Applied,
    ClearPending,
    Failed,
};

struct Status
{
    Availability availability = Availability::Unknown;
    WeaponSkinRefreshState weapon_skin_refresh_state = WeaponSkinRefreshState::Disabled;
    DemoKeychainState demo_keychain_state = DemoKeychainState::Idle;
    bool process_detected = false;
    bool hlae_detected = false;
    bool interfaces_resolved = false;
    bool interface_registry_enumerated = false;
    bool demo_guard_passed = false;
    bool demo_playing = false;
    bool demo_paused = false;
    bool demo_skin_rewrite_active = false;
    bool runtime_catalog_populated = false;
    bool hook_installed = false;
    bool frame_callbacks_active = false;
    bool demo_console_capture_ready = false;
    bool demo_console_query_inflight = false;
    bool schema_bound = false;
    bool entity_system_bound = false;
    bool client_entity_factory_requested = false;
    bool client_entity_factory_resolved = false;
    bool viewmodel_factory_discovery_ran = false;
    bool viewmodel_factory_observed = false;
    bool knife_animation_hook_resolved = false;
    bool knife_model_override_ready = false;
    bool stable_wearable_bound = false;
    bool runtime_created_wearable_bound = false;
    bool glove_pawn_fallback_bound = false;
    bool knife_viewmodel_sync_ready = false;
    bool map_remap_enabled = false;
    bool map_remap_hooks_installed = false;
    bool map_remap_source_captured = false;
    bool map_remap_use_source_radar = true;
    bool runtime_rebuilding = false;
    bool demo_keychain_create_disabled = false;
    std::uint64_t frame_callback_count = 0;
    std::uint64_t watched_xuid = 0;
    std::uint64_t probed_xuid = 0;
    std::uint64_t demo_keychain_owner_xuid = 0;
    std::uint64_t demo_keychain_authoritative_xuid = 0;
    int last_frame_stage = -1;
    int demo_tick = -1;
    int demo_keychain_weapon_entity_index = -1;
    int demo_keychain_base_item = 0;
    int demo_keychain_module_entity_index = -1;
    int demo_keychain_live_def_id = 0;
    int demo_keychain_stable_frame_count = 0;
    int demo_console_player_count = 0;
    int highest_entity_index = -1;
    int player_controller_count = 0;
    int player_pawn_count = 0;
    int preview_character_count = 0;
    int active_weapon_count = 0;
    int owned_weapon_count = 0;
    int enabled_profile_count = 0;
    int matched_profile_count = 0;
    int applied_weapon_count = 0;
    int applied_glove_count = 0;
    int skipped_knife_override_count = 0;
    std::uint64_t map_remap_replacement_count = 0;
    std::uint64_t map_remap_suppressed_source_geometry_count = 0;
    std::wstring knife_runtime_path;
    std::wstring knife_runtime_reason;
    std::wstring map_remap_source;
    std::wstring map_remap_target;
    std::wstring map_remap_issue;
    std::wstring process_path;
    std::wstring level_name;
    std::wstring demo_file_path;
    std::wstring last_error;
    std::wstring detail;
    std::wstring rebuild_reason;
    std::wstring demo_keychain_profile_name;
    std::wstring demo_keychain_issue;
    std::wstring demo_console_detail;
    std::wstring demo_console_issue;
    std::vector<std::wstring> diagnostics;
};
}

namespace nskinzcs2
{
class AppState
{
public:
    AppState() = default;

    model::ConfigDocument SnapshotConfig() const
    {
        std::scoped_lock lock(mutex_);
        return config_;
    }

    model::Catalog SnapshotCatalog() const
    {
        return *SnapshotCatalogShared();
    }

    std::shared_ptr<const model::Catalog> SnapshotCatalogShared() const
    {
        std::scoped_lock lock(catalog_mutex_);
        return catalog_;
    }

    std::vector<model::DemoMirvPlayerEntry> SnapshotDemoMirvPlayers() const
    {
        std::scoped_lock lock(mutex_);
        return demo_mirv_players_;
    }

    runtime::Status SnapshotStatus() const
    {
        std::scoped_lock lock(mutex_);
        return status_;
    }

    void ReplaceConfig(model::ConfigDocument document)
    {
        std::scoped_lock lock(mutex_);
        config_ = std::move(document);
        ++config_revision_;
    }

    void ReplaceCatalog(model::Catalog catalog)
    {
        // Allocate/copy outside the short publication lock. Existing readers
        // retain an immutable snapshot until their current apply pass ends.
        auto replacement = std::make_shared<const model::Catalog>(std::move(catalog));
        {
            std::scoped_lock lock(catalog_mutex_);
            catalog_.swap(replacement);
        }
    }

    void UpdateStatus(runtime::Status status)
    {
        std::scoped_lock lock(mutex_);
        status_ = std::move(status);
    }

    void ReplaceDemoMirvPlayers(std::vector<model::DemoMirvPlayerEntry> entries)
    {
        std::scoped_lock lock(mutex_);
        demo_mirv_players_ = std::move(entries);
    }

    void RequestDemoConsolePlayerRefresh() noexcept
    {
        demo_console_player_refresh_requested_.store(true);
    }

    bool ConsumeDemoConsolePlayerRefreshRequest() noexcept
    {
        return demo_console_player_refresh_requested_.exchange(false);
    }

    void RequestDemoKeychainRefresh() noexcept
    {
        demo_keychain_refresh_requested_.store(true);
    }

    bool ConsumeDemoKeychainRefreshRequest() noexcept
    {
        return demo_keychain_refresh_requested_.exchange(false);
    }

    std::uint64_t ConfigRevision() const noexcept
    {
        return config_revision_.load();
    }

    void RequestManualRefresh() noexcept
    {
        manual_refresh_requested_.store(true);
    }

    bool ConsumeManualRefreshRequest() noexcept
    {
        return manual_refresh_requested_.exchange(false);
    }

    void SetWindowVisible(bool value) noexcept
    {
        window_visible_.store(value);
    }

    bool WindowVisible() const noexcept
    {
        return window_visible_.load();
    }

    void RequestShutdown() noexcept
    {
        shutdown_requested_.store(true);
    }

    bool ShutdownRequested() const noexcept
    {
        return shutdown_requested_.load();
    }

private:
    mutable std::mutex mutex_;
    mutable std::mutex catalog_mutex_;
    model::ConfigDocument config_;
    std::shared_ptr<const model::Catalog> catalog_ = std::make_shared<const model::Catalog>();
    runtime::Status status_;
    std::vector<model::DemoMirvPlayerEntry> demo_mirv_players_;
    std::atomic<std::uint64_t> config_revision_ {0};
    std::atomic_bool demo_console_player_refresh_requested_ {false};
    std::atomic_bool demo_keychain_refresh_requested_ {false};
    std::atomic_bool manual_refresh_requested_ {false};
    std::atomic_bool window_visible_ {false};
    std::atomic_bool shutdown_requested_ {false};
};
}
