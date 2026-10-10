#pragma once

#include "Core/StickerPlacement.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nskinzcs2::ui::sticker_preview
{
// Generated from CS2 VMDL StickerMarkup and the matching legacy/HD mesh.
// The complete authored surface is piecewise projected; no point-fit matrix is used.
struct NativeSlot { float offset_x; float offset_y; float authored_scale; };
struct PreviewPoint { float x; float y; };
struct SurfaceTriangle { PreviewPoint offset[3]; PreviewPoint preview[3]; };
struct SurfaceRange { std::uint32_t first_triangle; std::uint16_t triangle_count; };
struct SilhouettePoint { std::uint8_t x; std::uint8_t y; };
struct WeaponLayout { int definition; bool legacy_model; std::array<NativeSlot, 5> slots; std::array<SurfaceRange, 5> surfaces; std::uint32_t first_point; std::uint16_t point_count; };
inline constexpr std::uint16_t kSilhouetteWidth = 256;
inline constexpr std::uint16_t kSilhouetteHeight = 112;

extern const std::array<SurfaceTriangle, 5751> kSurfaceTriangles;
extern const std::array<SilhouettePoint, 2007> kSilhouettePoints;
extern const std::array<WeaponLayout, 68> kWeaponLayouts;
inline const auto& kPreviewLayouts = kWeaponLayouts;

inline const WeaponLayout* FindWeaponLayout(const int definition, const bool legacy_model = true) noexcept
{ for (const auto& layout : kWeaponLayouts) if (layout.definition == definition && layout.legacy_model == legacy_model) return &layout; return nullptr; }
inline const WeaponLayout* FindPreviewLayout(const int definition, const bool legacy_model = true) noexcept { return FindWeaponLayout(definition, legacy_model); }

inline NativeSlot NativePreset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept
{ const auto* layout = FindWeaponLayout(definition, legacy_model); return layout != nullptr && slot < layout->slots.size() ? layout->slots[slot] : NativeSlot {}; }
inline bool HasNativePreset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept { return NativePreset(definition, slot, legacy_model).authored_scale > 0.0f; }

inline NativeSlot EffectivePreset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept
{ const auto native = NativePreset(definition, slot, legacy_model); if (native.authored_scale > 0.0f) return native; const auto placement = nskinzcs2::model::ResolveStickerAnchor(definition, slot, legacy_model); if (!placement.BorrowsAnotherSlot()) return {}; const auto anchor = NativePreset(definition, placement.schema, legacy_model); return {anchor.offset_x + placement.offset_x, anchor.offset_y + placement.offset_y, anchor.authored_scale}; }
inline NativeSlot DefaultPreset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept { return EffectivePreset(definition, slot, legacy_model); }
inline PreviewPoint MaterialBaseOffset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept { const auto preset = EffectivePreset(definition, slot, legacy_model); return {preset.offset_x, preset.offset_y}; }
inline PreviewPoint StoredToMaterialOffset(const int definition, const std::size_t slot, const float x, const float y, const bool legacy_model = true) noexcept { const auto base = MaterialBaseOffset(definition, slot, legacy_model); return {base.x + x, base.y + y}; }
inline PreviewPoint MaterialToStoredOffset(const int definition, const std::size_t slot, const float x, const float y, const bool legacy_model = true) noexcept { const auto base = MaterialBaseOffset(definition, slot, legacy_model); return {x - base.x, y - base.y}; }
inline PreviewPoint DefaultStoredOffset(const int, const std::size_t, const bool = true) noexcept { return {}; }

inline bool Weights(const PreviewPoint point, const PreviewPoint (&triangle)[3], std::array<float, 3>& weights) noexcept
{ const auto ax = triangle[1].x - triangle[0].x; const auto ay = triangle[1].y - triangle[0].y; const auto bx = triangle[2].x - triangle[0].x; const auto by = triangle[2].y - triangle[0].y; const auto determinant = ax * by - ay * bx; if (determinant > -0.0000001f && determinant < 0.0000001f) return false; const auto px = point.x - triangle[0].x; const auto py = point.y - triangle[0].y; weights[1] = (px * by - py * bx) / determinant; weights[2] = (ax * py - ay * px) / determinant; weights[0] = 1.0f - weights[1] - weights[2]; return true; }

inline bool ClosestPointWeights(const PreviewPoint point, const PreviewPoint (&triangle)[3], std::array<float, 3>& weights, float& distance) noexcept
{ distance = std::numeric_limits<float>::max(); bool found = false; for (std::size_t edge = 0; edge < 3; ++edge) { const auto next = (edge + 1) % 3; const auto dx = triangle[next].x - triangle[edge].x; const auto dy = triangle[next].y - triangle[edge].y; const auto length = dx * dx + dy * dy; if (length <= 0.0000000001f) continue; auto t = ((point.x - triangle[edge].x) * dx + (point.y - triangle[edge].y) * dy) / length; t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); const auto x = triangle[edge].x + t * dx; const auto y = triangle[edge].y + t * dy; const auto candidate = (point.x - x) * (point.x - x) + (point.y - y) * (point.y - y); if (candidate < distance) { distance = candidate; weights = {}; weights[edge] = 1.0f - t; weights[next] = t; found = true; } } return found; }

inline SurfaceRange SurfaceForSlot(const WeaponLayout& layout, const std::size_t slot) noexcept
{ const auto projection_slot = nskinzcs2::model::ResolveStickerProjectionSlot(layout.definition, slot, layout.legacy_model); return projection_slot < layout.surfaces.size() ? layout.surfaces[projection_slot] : SurfaceRange {}; }

inline PreviewPoint MapSurface(const SurfaceRange surface, const PreviewPoint point, const bool inverse, const PreviewPoint* hint = nullptr) noexcept
{ const SurfaceTriangle* fallback = nullptr; std::array<float, 3> fallback_weights {}; auto fallback_distance = std::numeric_limits<float>::max(); PreviewPoint hinted_result {}; auto hinted_distance = std::numeric_limits<float>::max(); bool has_hinted_result = false; for (std::uint32_t index = surface.first_triangle; index < surface.first_triangle + surface.triangle_count; ++index) { const auto& triangle = kSurfaceTriangles[index]; const auto& source = inverse ? triangle.preview : triangle.offset; std::array<float, 3> weights {}; if (!Weights(point, source, weights)) continue; const auto& target = inverse ? triangle.offset : triangle.preview; const PreviewPoint mapped {weights[0] * target[0].x + weights[1] * target[1].x + weights[2] * target[2].x, weights[0] * target[0].y + weights[1] * target[1].y + weights[2] * target[2].y}; const bool inside = weights[0] >= -0.0001f && weights[1] >= -0.0001f && weights[2] >= -0.0001f; if (inside) { if (!inverse || hint == nullptr) return mapped; const auto distance = (mapped.x - hint->x) * (mapped.x - hint->x) + (mapped.y - hint->y) * (mapped.y - hint->y); if (distance < hinted_distance) { hinted_distance = distance; hinted_result = mapped; has_hinted_result = true; } continue; } if (inverse) { std::array<float, 3> closest {}; float distance = 0.0f; if (ClosestPointWeights(point, source, closest, distance) && distance < fallback_distance) { fallback_distance = distance; fallback = &triangle; fallback_weights = closest; } continue; } const auto cx = (source[0].x + source[1].x + source[2].x) / 3.0f; const auto cy = (source[0].y + source[1].y + source[2].y) / 3.0f; const auto distance = (point.x - cx) * (point.x - cx) + (point.y - cy) * (point.y - cy); if (distance < fallback_distance) { fallback_distance = distance; fallback = &triangle; fallback_weights = weights; } } if (has_hinted_result) return hinted_result; if (fallback == nullptr) return inverse ? (hint != nullptr ? *hint : PreviewPoint {}) : PreviewPoint {0.5f, 0.5f}; const auto& target = inverse ? fallback->offset : fallback->preview; return {fallback_weights[0] * target[0].x + fallback_weights[1] * target[1].x + fallback_weights[2] * target[2].x, fallback_weights[0] * target[0].y + fallback_weights[1] * target[1].y + fallback_weights[2] * target[2].y}; }

inline PreviewPoint OffsetToPreview(const int definition, const std::size_t slot, const float x, const float y, const bool legacy_model = true) noexcept { const auto* layout = FindWeaponLayout(definition, legacy_model); return layout ? MapSurface(SurfaceForSlot(*layout, slot), {x, y}, false) : PreviewPoint {0.5f, 0.5f}; }
inline PreviewPoint PreviewToOffset(const int definition, const std::size_t slot, const float x, const float y, const bool legacy_model = true, const PreviewPoint* hint = nullptr) noexcept { const auto* layout = FindWeaponLayout(definition, legacy_model); return layout ? MapSurface(SurfaceForSlot(*layout, slot), {x, y}, true, hint) : PreviewPoint {}; }
inline PreviewPoint EffectiveOffsetToPreview(const int definition, const std::size_t slot, const bool legacy_model, const float x, const float y) noexcept { return OffsetToPreview(definition, slot, x, y, legacy_model); }
inline PreviewPoint PreviewToEffectiveOffset(const int definition, const std::size_t slot, const bool legacy_model, const float x, const float y, const PreviewPoint* hint = nullptr) noexcept { return PreviewToOffset(definition, slot, x, y, legacy_model, hint); }
}
