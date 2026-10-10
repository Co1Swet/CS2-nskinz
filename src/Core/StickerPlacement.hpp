#pragma once

#include <cstddef>
#include <cstdint>

namespace nskinzcs2::model
{
// CS2's "sticker slot N schema" attribute selects the material-authored
// sticker home used by a logical slot. Schema zero means "use this slot".
// Several weapon/model variants only author four homes, so logical slot five
// must borrow an existing home and add the measured offset below.
struct StickerAnchorPlacement
{
    std::uint32_t schema = 0;
    float offset_x = 0.0f;
    float offset_y = 0.0f;

    [[nodiscard]] constexpr bool BorrowsAnotherSlot() const noexcept
    {
        return schema != 0;
    }
};

[[nodiscard]] constexpr StickerAnchorPlacement ResolveFifthStickerAnchor(
    const int weapon_definition,
    const bool legacy_model) noexcept
{
    switch (weapon_definition)
    {
    case 1: // Desert Eagle
        return legacy_model ? StickerAnchorPlacement {1, 0.13099661f, 0.031996623f}
                            : StickerAnchorPlacement {};
    case 2: // Dual Berettas
        return legacy_model ? StickerAnchorPlacement {2, -0.034058955f, 0.06794105f}
                            : StickerAnchorPlacement {2, -0.017009478f, 0.09099052f};
    case 4: // Glock-18
        return legacy_model ? StickerAnchorPlacement {}
                            : StickerAnchorPlacement {1, 0.31375384f, -0.07624616f};
    case 7: // AK-47
        return legacy_model ? StickerAnchorPlacement {1, 0.12881461f, 0.03781461f}
                            : StickerAnchorPlacement {1, 0.14699425f, 0.028994253f};
    case 9: // AWP
        return legacy_model ? StickerAnchorPlacement {}
                            : StickerAnchorPlacement {1, 0.26138917f, 0.041389152f};
    case 13: // Galil AR
        return legacy_model ? StickerAnchorPlacement {3, 0.079801366f, 0.011801365f}
                            : StickerAnchorPlacement {1, 0.3629775f, 0.0299775f};
    case 14: // M249
        return legacy_model ? StickerAnchorPlacement {3, 0.0037358073f, 0.047735807f}
                            : StickerAnchorPlacement {2, -0.26837832f, 0.1806217f};
    case 16: // M4A4
        return legacy_model ? StickerAnchorPlacement {}
                            : StickerAnchorPlacement {1, 0.33399266f, 0.015992647f};
    case 17: // MAC-10
        return legacy_model ? StickerAnchorPlacement {3, 0.12097203f, 0.06197203f}
                            : StickerAnchorPlacement {};
    case 24: // UMP-45
        return legacy_model ? StickerAnchorPlacement {3, -0.15774709f, -0.008747093f}
                            : StickerAnchorPlacement {2, -0.060853533f, 0.06014647f};
    case 26: // PP-Bizon
        return legacy_model ? StickerAnchorPlacement {1, 0.11569809f, 0.054698095f}
                            : StickerAnchorPlacement {1, -0.16905054f, -0.00005054744f};
    case 27: // MAG-7
        return legacy_model ? StickerAnchorPlacement {2, -0.22446628f, 0.0075337263f}
                            : StickerAnchorPlacement {3, -0.11744954f, 0.0085504595f};
    case 28: // Negev
        return legacy_model ? StickerAnchorPlacement {3, -0.15602273f, -0.016022725f}
                            : StickerAnchorPlacement {1, 0.032989472f, 0.051989473f};
    case 30: // Tec-9
        return legacy_model ? StickerAnchorPlacement {}
                            : StickerAnchorPlacement {1, 0.16984202f, 0.13384202f};
    case 34: // MP9
        return legacy_model ? StickerAnchorPlacement {}
                            : StickerAnchorPlacement {2, -0.05625756f, 0.13074245f};
    case 38: // SCAR-20
        return legacy_model ? StickerAnchorPlacement {2, 0.28459403f, -0.011405965f}
                            : StickerAnchorPlacement {3, -0.0620967f, 0.013903301f};
    case 39: // SG 553
        return legacy_model ? StickerAnchorPlacement {2, -0.061953463f, -0.015953463f}
                            : StickerAnchorPlacement {2, -0.06719466f, -0.02519466f};
    case 40: // SSG 08
        return legacy_model ? StickerAnchorPlacement {3, -0.041737854f, -0.0067378553f}
                            : StickerAnchorPlacement {1, 0.0839881f, 0.024988096f};
    default:
        return {};
    }
}

[[nodiscard]] constexpr StickerAnchorPlacement ResolveStickerAnchor(
    const int weapon_definition,
    const std::size_t logical_slot,
    const bool legacy_model) noexcept
{
    return logical_slot == 4
        ? ResolveFifthStickerAnchor(weapon_definition, legacy_model)
        : StickerAnchorPlacement {};
}

[[nodiscard]] constexpr std::size_t ResolveStickerProjectionSlot(
    const int weapon_definition,
    const std::size_t logical_slot,
    const bool legacy_model) noexcept
{
    const auto placement = ResolveStickerAnchor(
        weapon_definition, logical_slot, legacy_model);
    return placement.BorrowsAnotherSlot()
        ? static_cast<std::size_t>(placement.schema)
        : logical_slot;
}

[[nodiscard]] constexpr float ResolveSerializedStickerOffsetX(
    const int weapon_definition,
    const std::size_t logical_slot,
    const bool legacy_model,
    const float stored_delta) noexcept
{
    return stored_delta +
        ResolveStickerAnchor(weapon_definition, logical_slot, legacy_model).offset_x;
}

[[nodiscard]] constexpr float ResolveSerializedStickerOffsetY(
    const int weapon_definition,
    const std::size_t logical_slot,
    const bool legacy_model,
    const float stored_delta) noexcept
{
    return stored_delta +
        ResolveStickerAnchor(weapon_definition, logical_slot, legacy_model).offset_y;
}
}
