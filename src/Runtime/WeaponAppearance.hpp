#pragma once

#include "Core\AppState.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nskinzcs2::runtime
{
enum class WeaponAppearanceStage
{
    None,
    Load,
    Validate,
    Precache,
    Bind,
    Apply,
    Verify,
    Rollback,
};

enum class WeaponAppearanceRollbackStep
{
    RestoreAttributeVector,
    RestorePrecacheFlag,
    RestoreMeshState,
};

struct WeaponAppearanceRequest
{
    std::uint64_t owner_xuid = 0;
    std::uint32_t owned_weapon_handle = 0xFFFFFFFFu;
    int owned_weapon_entity_index = -1;
    int current_definition_index = 0;
    std::uint64_t desired_signature = 0;
    bool watched_weapon = false;
    bool active_weapon = false;
    model::CosmeticProfile profile;
};

struct WeaponAppearanceBindingInputs
{
    std::uint64_t owner_xuid = 0;
    std::uint32_t owned_weapon_handle = 0xFFFFFFFFu;
    int owned_weapon_entity_index = -1;
    bool owned_weapon_has_item_view = false;
    int explicit_world_child_count = 0;
    int explicit_world_child_entity_index = -1;
    bool explicit_world_child_has_item_view = false;
    bool explicit_world_child_item_view_shared_with_owned = true;
    int hud_entity_index = -1;
    std::uint64_t mesh_group_mask = 1;
};

struct ResolvedWeaponAppearanceBinding
{
    std::uint64_t owner_xuid = 0;
    std::uint32_t owned_weapon_handle = 0xFFFFFFFFu;
    int owned_weapon_entity_index = -1;
    int carrier_entity_index = -1;
    int render_entity_index = -1;
    int hud_entity_index = -1;
    std::uint64_t mesh_group_mask = 1;
    bool render_entity_is_owned = true;
    bool render_entity_is_explicit_child = false;
    bool carrier_entity_is_owned = true;
    bool carrier_entity_is_explicit_child = false;
    bool valid = false;
};

struct WeaponAppearancePayload
{
    int fallback_paint_kit = 0;
    int fallback_seed = 0;
    float fallback_wear = model::kMinimumCosmeticWear;
    int fallback_stat_trak = 0;
    int quality = 0;
    std::uint32_t account_id = 0;
    bool writes_custom_name = false;
    std::string custom_name;
    std::vector<std::pair<std::uint16_t, float>> temporary_attributes;
    bool needs_refresh = false;
};

struct WeaponAppearanceSnapshot
{
    bool valid = false;
    int fallback_paint_kit = 0;
    int fallback_seed = 0;
    float fallback_wear = model::kMinimumCosmeticWear;
    int fallback_stat_trak = 0;
    std::uint32_t keychain_def_id = 0;
    std::uint32_t keychain_seed = 0;
    std::uint16_t definition_index = 0;
    std::uint32_t owner_xuid_low = 0;
    std::uint32_t owner_xuid_high = 0;
    std::uint32_t item_id_high = 0;
    std::uint32_t item_id_low = 0;
    std::uint32_t account_id = 0;
    std::uint32_t external_item_provider_registered_token = 0;
    std::uint64_t registered_as_item_id = 0;
    std::uint8_t quality = 0;
    bool econ_entity_initialized = false;
    bool item_view_initialized = false;
    bool item_view_disallow_soc = false;
    bool restore_custom_material_after_precache = false;
    bool has_owner_xuid_low = false;
    bool has_owner_xuid_high = false;
    bool has_definition_index = false;
    bool has_item_id_low = false;
    bool has_account_id = false;
    bool has_external_item_provider_registered_token = false;
    bool has_registered_as_item_id = false;
    bool has_quality = false;
    bool has_keychain_def_id = false;
    bool has_keychain_seed = false;
    bool has_econ_entity_initialized = false;
    bool has_item_view_initialized = false;
    bool has_item_view_disallow_soc = false;
    bool has_restore_custom_material_after_precache = false;
    bool has_custom_name = false;
    bool has_custom_name_override = false;
    std::string custom_name;
    std::string custom_name_override;
    std::uint64_t render_mesh_group_mask = 0;
    std::uint64_t hud_mesh_group_mask = 0;
    bool has_render_mesh_group_mask = false;
    bool has_hud_mesh_group_mask = false;
};

struct WeaponAppearanceApplyResult
{
    WeaponAppearanceStage stage = WeaponAppearanceStage::None;
    bool applied = false;
    bool verified = false;
    bool rolled_back = false;
    std::wstring issue;
};

ResolvedWeaponAppearanceBinding ResolveWeaponAppearanceBinding(const WeaponAppearanceBindingInputs& inputs) noexcept;
WeaponAppearancePayload BuildWeaponAppearancePayload(
    const nskinzcs2::model::CosmeticProfile& profile,
    std::uint32_t account_id,
    const nskinzcs2::model::KeychainProfile* keychain_profile = nullptr);
bool WeaponAppearanceNeedsIndependentHudApply(
    bool hud_entity_present,
    int hud_entity_index,
    bool hud_item_view_present,
    bool hud_item_view_shared_with_carrier) noexcept;
std::wstring DescribeWeaponAppearanceBinding(const ResolvedWeaponAppearanceBinding& binding);
std::wstring DescribeWeaponAppearancePayload(const WeaponAppearancePayload& payload);
std::array<WeaponAppearanceRollbackStep, 3> WeaponAppearanceRollbackOrder() noexcept;
const wchar_t* WeaponAppearanceStageText(WeaponAppearanceStage stage) noexcept;
}
