#pragma once

#include <algorithm>
#include <cmath>

namespace nskinzcs2::ui::sticker_preview
{
// CS2's authored sticker scale describes a unit-wide decal plane.  The
// preview used to treat that unit as a radius ([-1, +1]), which doubled the
// linear size and made neighbouring slots look as if they overlapped.  A
// unit-wide plane has a half extent of 0.5.
inline constexpr float kCanonicalStickerHalfExtent = 0.5f;

// Position profiles store a delta from the material-authored slot.  The rail
// intentionally covers the useful fine-adjustment neighbourhood; the numeric
// input remains unrestricted for larger moves.
inline constexpr float kFineOffsetRailLimit = 0.1f;

[[nodiscard]] inline float ComputeStickerPreviewHalfExtent(
    const float profile_scale,
    const float authored_scale) noexcept
{
    const auto safe_profile_scale = std::isfinite(profile_scale)
        ? std::clamp(profile_scale, 0.1f, 5.0f)
        : 1.0f;
    const auto safe_authored_scale = std::isfinite(authored_scale)
        ? std::max(1.0f, authored_scale)
        : 1.0f;
    return kCanonicalStickerHalfExtent * safe_profile_scale / safe_authored_scale;
}
}
