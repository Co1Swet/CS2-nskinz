#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nskinzcs2::model
{
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
    int quality = 0;
    int seed = 0;
    int stat_trak = 0;
    float wear = 0.0000001f;
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
    int quality = 0;
    int seed = 0;
    float wear = 0.0000001f;
};

struct ConfigDocument
{
    int version = 3;
    std::vector<CosmeticProfile> profiles;
    std::vector<KnifeProfile> knife_profiles;
    std::vector<GloveProfile> glove_profiles;
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

struct Catalog
{
    std::vector<NamedId> base_items;
    std::vector<NamedId> knife_items;
    std::vector<NamedId> glove_items;
    std::vector<NamedId> weapon_paint_kits;
    std::vector<NamedId> glove_paint_kits;
    std::unordered_map<int, int> glove_paint_compatibility;
    std::vector<NamedId> sticker_kits;
    std::vector<NamedId> qualities;
    std::unordered_map<int, VisualInfo> visuals;
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

struct Status
{
    Availability availability = Availability::Unknown;
    WeaponSkinRefreshState weapon_skin_refresh_state = WeaponSkinRefreshState::Disabled;
    bool process_detected = false;
    bool hlae_detected = false;
    bool interfaces_resolved = false;
    bool interface_registry_enumerated = false;
    bool demo_guard_passed = false;
    bool demo_playing = false;
    bool demo_paused = false;
    bool runtime_catalog_populated = false;
    bool hook_installed = false;
    bool frame_callbacks_active = false;
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
    bool runtime_rebuilding = false;
    std::uint64_t frame_callback_count = 0;
    std::uint64_t watched_xuid = 0;
    std::uint64_t probed_xuid = 0;
    int last_frame_stage = -1;
    int demo_tick = -1;
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
    std::wstring knife_runtime_path;
    std::wstring knife_runtime_reason;
    std::wstring process_path;
    std::wstring level_name;
    std::wstring last_error;
    std::wstring detail;
    std::wstring rebuild_reason;
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
        std::scoped_lock lock(mutex_);
        return catalog_;
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
        std::scoped_lock lock(mutex_);
        catalog_ = std::move(catalog);
    }

    void UpdateStatus(runtime::Status status)
    {
        std::scoped_lock lock(mutex_);
        status_ = std::move(status);
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
    model::ConfigDocument config_;
    model::Catalog catalog_;
    runtime::Status status_;
    std::atomic<std::uint64_t> config_revision_ {0};
    std::atomic_bool manual_refresh_requested_ {false};
    std::atomic_bool window_visible_ {false};
    std::atomic_bool shutdown_requested_ {false};
};
}
