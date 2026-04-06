#include "Runtime/WeaponAppearance.hpp"

#include <algorithm>
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
constexpr std::uint16_t kAttributeStickerWear = 1;
constexpr std::uint16_t kAttributeStickerScale = 2;
constexpr std::uint16_t kAttributeStickerRotation = 3;
constexpr float kDefaultWear = 0.0000001f;

bool StickerNeedsAttributes(const nskinzcs2::model::StickerProfile& sticker) noexcept
{
    return sticker.kit_id != 0 ||
        std::fabs(sticker.wear - kDefaultWear) > 0.0000001f ||
        std::fabs(sticker.scale - 1.0f) > 0.0001f ||
        std::fabs(sticker.rotation) > 0.0001f;
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

nskinzcs2::runtime::WeaponAppearancePayload nskinzcs2::runtime::BuildWeaponAppearancePayload(
    const nskinzcs2::model::CosmeticProfile& profile,
    const std::uint32_t account_id)
{
    WeaponAppearancePayload payload {};
    payload.fallback_paint_kit = profile.paint_kit;
    payload.fallback_seed = profile.seed;
    payload.fallback_wear = profile.wear;
    payload.fallback_stat_trak = profile.stat_trak;
    payload.quality = std::clamp(profile.quality, 0, 255);
    payload.account_id = account_id;
    payload.writes_custom_name = !profile.custom_name.empty();
    payload.custom_name = profile.custom_name;
    payload.temporary_attributes.reserve(32);

    if (profile.paint_kit != 0)
    {
        payload.temporary_attributes.emplace_back(kAttributePaintKit, static_cast<float>(profile.paint_kit));
    }

    if (profile.paint_kit != 0 || profile.seed != 0)
    {
        payload.temporary_attributes.emplace_back(kAttributeSeed, static_cast<float>(profile.seed));
    }

    if (profile.paint_kit != 0 || std::fabs(profile.wear - kDefaultWear) > 0.0000001f)
    {
        payload.temporary_attributes.emplace_back(kAttributeWear, profile.wear);
    }

    if (profile.stat_trak != 0)
    {
        payload.temporary_attributes.emplace_back(kAttributeStatTrak, static_cast<float>(profile.stat_trak));
    }

    for (std::size_t slot = 0; slot < profile.stickers.size(); ++slot)
    {
        const auto& sticker = profile.stickers[slot];
        if (!StickerNeedsAttributes(sticker))
        {
            continue;
        }

        const auto base = static_cast<std::uint16_t>(kAttributeStickerKitBase + slot * kAttributeStickerStride);
        payload.temporary_attributes.emplace_back(base, static_cast<float>(sticker.kit_id));
        payload.temporary_attributes.emplace_back(static_cast<std::uint16_t>(base + kAttributeStickerWear), sticker.wear);
        payload.temporary_attributes.emplace_back(static_cast<std::uint16_t>(base + kAttributeStickerScale), sticker.scale);
        payload.temporary_attributes.emplace_back(static_cast<std::uint16_t>(base + kAttributeStickerRotation), sticker.rotation);
    }

    payload.needs_refresh = !payload.temporary_attributes.empty();
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
    for (const auto& [definition_index, value] : payload.temporary_attributes)
    {
        static_cast<void>(value);
        if (definition_index >= kAttributeStickerKitBase)
        {
            ++sticker_attribute_count;
        }
    }

    wchar_t buffer[512] {};
    _snwprintf_s(
        buffer,
        _countof(buffer),
        _TRUNCATE,
        L"paint=%d, seed=%d, wear=%g, stattrak=%d, quality=%d, account_id=%u, temporary_attribute_count=%zu, sticker_attribute_count=%zu, custom_name=%d, needs_refresh=%d",
        payload.fallback_paint_kit,
        payload.fallback_seed,
        payload.fallback_wear,
        payload.fallback_stat_trak,
        payload.quality,
        payload.account_id,
        payload.temporary_attributes.size(),
        sticker_attribute_count,
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
