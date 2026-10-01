#include "Runtime/WeaponAppearance.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cwchar>

namespace
{
constexpr std::uint16_t kAttributePaintKit = 6;
constexpr std::uint16_t kAttributeSeed = 7;
constexpr std::uint16_t kAttributeWear = 8;
constexpr std::uint16_t kAttributeStatTrak = 80;
constexpr std::uint16_t kAttributeStickerKitBase = 113;
constexpr std::uint16_t kAttributeStickerStride = 4;
constexpr std::uint16_t kAttributeKeychainId = 299;
constexpr std::uint16_t kAttributeKeychainOffsetX = 300;
constexpr std::uint16_t kAttributeKeychainOffsetY = 301;
constexpr std::uint16_t kAttributeKeychainOffsetZ = 302;
constexpr std::uint16_t kAttributeKeychainSeed = 306;
constexpr std::uint16_t kAttributeKeychainHighlight = 314;
constexpr std::uint16_t kAttributeKeychainSticker = 321;
constexpr float kDefaultWear = 0.0000001f;

bool StickerReplacesExistingSlot(const nskinzcs2::model::StickerProfile& sticker) noexcept
{
    return sticker.kit_id > 0;
}

float EncodeStickerKitAttributeValue(const int kit_id) noexcept
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(std::max(0, kit_id)));
}

void AppendKeychainSlotZeroAttributes(
    std::vector<std::pair<std::uint16_t, float>>& attributes,
    const nskinzcs2::model::KeychainProfile& keychain_profile)
{
    const auto keychain_id = static_cast<float>(std::max(0, keychain_profile.keychain_kit));
    attributes.emplace_back(kAttributeKeychainId, keychain_id);
    attributes.emplace_back(kAttributeKeychainOffsetX, 0.0f);
    attributes.emplace_back(kAttributeKeychainOffsetY, 0.0f);
    attributes.emplace_back(kAttributeKeychainOffsetZ, 0.0f);
    attributes.emplace_back(kAttributeKeychainSeed, 0.0f);
    attributes.emplace_back(kAttributeKeychainHighlight, 0.0f);
    attributes.emplace_back(kAttributeKeychainSticker, 0.0f);
}

bool IsKeychainAttributeDefinitionIndex(const std::uint16_t definition_index) noexcept
{
    switch (definition_index)
    {
    case kAttributeKeychainId:
    case kAttributeKeychainOffsetX:
    case kAttributeKeychainOffsetY:
    case kAttributeKeychainOffsetZ:
    case kAttributeKeychainSeed:
    case kAttributeKeychainHighlight:
    case kAttributeKeychainSticker:
        return true;
    default:
        return false;
    }
}
}

nskinzcs2::runtime::ResolvedWeaponAppearanceBinding nskinzcs2::runtime::ResolveWeaponAppearanceBinding(
    const WeaponAppearanceBindingInputs& inputs) noexcept
{
    ResolvedWeaponAppearanceBinding binding {};
    binding.owner_xuid = inputs.owner_xuid;
    binding.owned_weapon_handle = inputs.owned_weapon_handle;
    binding.owned_weapon_entity_index = inputs.owned_weapon_entity_index;
    binding.mesh_group_mask = inputs.mesh_group_mask == 0 ? 1ull : inputs.mesh_group_mask;
    binding.hud_entity_index = inputs.hud_entity_index;
    binding.hud_render_only = inputs.hud_render_only_verified;

    if (inputs.owned_weapon_entity_index < 0 || !inputs.owned_weapon_has_item_view)
    {
        return binding;
    }

    binding.carrier_entity_index = inputs.owned_weapon_entity_index;
    binding.render_entity_index = inputs.owned_weapon_entity_index;

    const auto has_unique_explicit_world_child =
        inputs.explicit_world_child_count == 1 &&
        inputs.explicit_world_child_entity_index >= 0;
    if (has_unique_explicit_world_child)
    {
        binding.render_entity_index = inputs.explicit_world_child_entity_index;
        binding.render_entity_is_owned = false;
        binding.render_entity_is_explicit_child = true;

        if (inputs.explicit_world_child_has_item_view &&
            !inputs.explicit_world_child_item_view_shared_with_owned)
        {
            binding.carrier_entity_index = inputs.explicit_world_child_entity_index;
            binding.carrier_entity_is_owned = false;
            binding.carrier_entity_is_explicit_child = true;
        }
    }

    binding.valid = true;
    return binding;
}

bool nskinzcs2::runtime::IsStandardWeaponHudRenderCandidate(
    const std::wstring_view class_name, const std::string_view model_name,
    const std::uint32_t candidate_owner, const std::uint32_t weapon_handle,
    const bool below_verified_arms, const bool aliases_world_or_arms) noexcept
{
    return below_verified_arms && !aliases_world_or_arms &&
        weapon_handle != 0xFFFFFFFFu && weapon_handle != 0xFFFFFFFEu &&
        candidate_owner == weapon_handle && class_name == L"C_CS2HudModelWeapon" &&
        model_name.starts_with("weapons/models/") &&
        model_name.find("/knife/") == std::string_view::npos &&
        model_name.find("knife_") == std::string_view::npos &&
        model_name.find("/arms/") == std::string_view::npos &&
        model_name.find("glove") == std::string_view::npos;
}

std::uint64_t nskinzcs2::runtime::ResolvePaintMeshGroupMask(
    const bool standard_weapon, const int paint_kit, const model::Catalog* catalog) noexcept
{
    if (!standard_weapon || paint_kit <= 0) { return 1; }
    if (catalog != nullptr)
    {
        const auto found = catalog->weapon_paint_uses_legacy_model.find(paint_kit);
        if (found != catalog->weapon_paint_uses_legacy_model.end()) { return found->second ? 2 : 1; }
    }
    // Preserve the old behavior only when metadata is genuinely unavailable.
    // Parsed modern kits WITHOUT use_legacy_model are explicit false entries.
    return 2;
}

nskinzcs2::runtime::WeaponAppearancePayload nskinzcs2::runtime::BuildWeaponAppearancePayload(
    const nskinzcs2::model::CosmeticProfile& profile,
    const std::uint32_t account_id,
    const nskinzcs2::model::KeychainProfile* keychain_profile)
{
    WeaponAppearancePayload payload {};
    const auto effective_wear =
        profile.base_item == 42 || profile.base_item == 5028
            ? profile.wear
            : nskinzcs2::model::NormalizeCosmeticWear(profile.wear);
    payload.fallback_paint_kit = profile.paint_kit;
    payload.fallback_seed = profile.seed;
    payload.fallback_wear = effective_wear;
    payload.fallback_stat_trak = profile.stat_trak;
    payload.quality = std::clamp(profile.quality, 0, 255);
    payload.account_id = account_id;
    payload.writes_custom_name = true;
    payload.custom_name = nskinzcs2::model::ResolveCosmeticCustomName(profile.custom_name);
    payload.temporary_attributes.reserve(32);
    const auto writes_finish =
        profile.paint_kit != 0 || profile.explicit_vanilla_finish;

    // paint_kit=0 is ambiguous in the game data, so only append attribute 6
    // when the persisted UI intent explicitly requests the vanilla finish.
    // The ordinary zero selection means "do not modify" and stays off this
    // path, including profiles synthesized solely for keychain work.
    const auto explicit_vanilla_knife_finish =
        profile.base_item == 42 &&
        profile.override_item == 0 &&
        profile.paint_kit == 0 &&
        profile.explicit_vanilla_finish;
    const auto explicit_vanilla_standard_weapon_finish =
        profile.base_item != 0 &&
        profile.base_item != 42 &&
        profile.base_item != 5028 &&
        profile.override_item == 0 &&
        profile.paint_kit == 0 &&
        profile.explicit_vanilla_finish;
    if (writes_finish && (profile.paint_kit != 0 ||
        explicit_vanilla_knife_finish ||
        explicit_vanilla_standard_weapon_finish))
    {
        payload.temporary_attributes.emplace_back(kAttributePaintKit, static_cast<float>(profile.paint_kit));
    }

    if (writes_finish && (profile.paint_kit != 0 || profile.seed != 0))
    {
        payload.temporary_attributes.emplace_back(kAttributeSeed, static_cast<float>(profile.seed));
    }

    if (writes_finish &&
        (profile.paint_kit != 0 || std::fabs(effective_wear - kDefaultWear) > 0.0000001f))
    {
        payload.temporary_attributes.emplace_back(kAttributeWear, effective_wear);
    }

    if (profile.stat_trak != 0)
    {
        payload.temporary_attributes.emplace_back(kAttributeStatTrak, static_cast<float>(profile.stat_trak));
    }

    for (std::size_t slot = 0; slot < profile.stickers.size(); ++slot)
    {
        const auto& sticker = profile.stickers[slot];
        if (!StickerReplacesExistingSlot(sticker))
        {
            continue;
        }

        // This path intentionally replaces only an already-recorded sticker
        // kit. Omitting the neighboring wear/scale/rotation attributes keeps
        // the demo's original transform, including CS2 free-placement data.
        // The runtime attribute collector rejects this write when the kit
        // attribute for the requested logical slot does not already exist.
        const auto base = static_cast<std::uint16_t>(kAttributeStickerKitBase + slot * kAttributeStickerStride);
        // "sticker slot N id" is an integer Econ attribute. The networked
        // attribute node exposes its 32-bit union through a float field, so
        // preserve the integer bit pattern instead of numerically converting
        // (76.0f would be read by the native integer getter as 0x42980000).
        payload.temporary_attributes.emplace_back(base, EncodeStickerKitAttributeValue(sticker.kit_id));
    }

    if (keychain_profile != nullptr &&
        (keychain_profile->keychain_kit != 0 || keychain_profile->explicit_clear))
    {
        AppendKeychainSlotZeroAttributes(payload.temporary_attributes, *keychain_profile);
    }

    payload.needs_refresh =
        std::any_of(
            payload.temporary_attributes.begin(),
            payload.temporary_attributes.end(),
            [](const auto& attribute)
            {
                return !IsKeychainAttributeDefinitionIndex(attribute.first);
            });
    return payload;
}

bool nskinzcs2::runtime::WeaponAppearanceNeedsIndependentHudApply(
    const bool hud_entity_present,
    const int hud_entity_index,
    const bool hud_item_view_present,
    const bool hud_item_view_shared_with_carrier) noexcept
{
    return hud_entity_present &&
        hud_entity_index >= 0 &&
        hud_item_view_present &&
        !hud_item_view_shared_with_carrier;
}

std::wstring nskinzcs2::runtime::DescribeWeaponAppearanceBinding(const ResolvedWeaponAppearanceBinding& binding)
{
    wchar_t buffer[512] {};
    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"owner_xuid=%llu, owned_weapon_entity=%d, carrier_entity=%d, render_entity=%d, hud_entity=%d, mesh_mask=%llu, carrier_is_owned=%d, carrier_is_explicit_child=%d, render_is_owned=%d, render_is_explicit_child=%d, valid=%d",
        static_cast<unsigned long long>(binding.owner_xuid),
        binding.owned_weapon_entity_index,
        binding.carrier_entity_index,
        binding.render_entity_index,
        binding.hud_entity_index,
        static_cast<unsigned long long>(binding.mesh_group_mask),
        binding.carrier_entity_is_owned ? 1 : 0,
        binding.carrier_entity_is_explicit_child ? 1 : 0,
        binding.render_entity_is_owned ? 1 : 0,
        binding.render_entity_is_explicit_child ? 1 : 0,
        binding.valid ? 1 : 0);
    return buffer;
}

std::wstring nskinzcs2::runtime::DescribeWeaponAppearancePayload(const WeaponAppearancePayload& payload)
{
    std::size_t sticker_attribute_count = 0;
    std::size_t keychain_attribute_count = 0;
    for (const auto& [definition_index, value] : payload.temporary_attributes)
    {
        static_cast<void>(value);
        if (IsKeychainAttributeDefinitionIndex(definition_index))
        {
            ++keychain_attribute_count;
        }
        else if (definition_index >= kAttributeStickerKitBase)
        {
            ++sticker_attribute_count;
        }
    }

    wchar_t buffer[512] {};
    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"paint=%d, seed=%d, wear=%g, stattrak=%d, quality=%d, account_id=%u, temporary_attribute_count=%zu, sticker_attribute_count=%zu, keychain_attribute_count=%zu, custom_name=%d, needs_refresh=%d",
        payload.fallback_paint_kit,
        payload.fallback_seed,
        payload.fallback_wear,
        payload.fallback_stat_trak,
        payload.quality,
        payload.account_id,
        payload.temporary_attributes.size(),
        sticker_attribute_count,
        keychain_attribute_count,
        payload.writes_custom_name ? 1 : 0,
        payload.needs_refresh ? 1 : 0);
    return buffer;
}

std::array<nskinzcs2::runtime::WeaponAppearanceRollbackStep, 3> nskinzcs2::runtime::WeaponAppearanceRollbackOrder() noexcept
{
    return {
        WeaponAppearanceRollbackStep::RestoreAttributeVector,
        WeaponAppearanceRollbackStep::RestorePrecacheFlag,
        WeaponAppearanceRollbackStep::RestoreMeshState,
    };
}

const wchar_t* nskinzcs2::runtime::WeaponAppearanceStageText(const WeaponAppearanceStage stage) noexcept
{
    switch (stage)
    {
    case WeaponAppearanceStage::None:
        return L"None";
    case WeaponAppearanceStage::Load:
        return L"Load";
    case WeaponAppearanceStage::Validate:
        return L"Validate";
    case WeaponAppearanceStage::Precache:
        return L"Precache";
    case WeaponAppearanceStage::Bind:
        return L"Bind";
    case WeaponAppearanceStage::Apply:
        return L"Apply";
    case WeaponAppearanceStage::Verify:
        return L"Verify";
    case WeaponAppearanceStage::Rollback:
        return L"Rollback";
    }

    return L"Unknown";
}
