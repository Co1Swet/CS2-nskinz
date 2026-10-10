#!/usr/bin/env python3
"""Generate the model-accurate sticker surface preview table.

Valve assets remain in ``build/temp``. The checked-in table is generated from
each compiled VMDL's StickerMarkup and its matching legacy/HD TEXCOORD_1 mesh.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import pathlib
import re
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw


ROOT = pathlib.Path("build/temp")
MODEL_ROOT = ROOT / "sticker-models"
OUTPUT = pathlib.Path("src/UI/StickerPreviewPlacement.generated.hpp")
OUTPUT_CPP = pathlib.Path("src/UI/StickerPreviewPlacement.generated.cpp")
MASK_WIDTH = 256
MASK_HEIGHT = 112

MODELS = {
    1: "deagle/weapon_pist_deagle", 2: "elite/weapon_pist_elite",
    3: "fiveseven/weapon_pist_fiveseven", 4: "glock18/weapon_pist_glock18",
    7: "ak47/weapon_rif_ak47", 8: "aug/weapon_rif_aug", 9: "awp/weapon_snip_awp",
    10: "famas/weapon_rif_famas", 11: "g3sg1/weapon_snip_g3sg1",
    13: "galilar/weapon_rif_galilar", 14: "m249/weapon_mach_m249",
    16: "m4a4/weapon_rif_m4a4", 17: "mac10/weapon_smg_mac10",
    19: "p90/weapon_smg_p90", 23: "mp5sd/weapon_smg_mp5sd",
    24: "ump45/weapon_smg_ump45", 25: "xm1014/weapon_shot_xm1014",
    26: "bizon/weapon_smg_bizon", 27: "mag7/weapon_shot_mag7",
    28: "negev/weapon_mach_negev", 29: "sawedoff/weapon_shot_sawedoff",
    30: "tec9/weapon_pist_tec9", 32: "hkp2000/weapon_pist_hkp2000",
    33: "mp7/weapon_smg_mp7", 34: "mp9/weapon_smg_mp9",
    35: "nova/weapon_shot_nova", 36: "p250/weapon_pist_p250",
    38: "scar20/weapon_snip_scar20", 39: "sg556/weapon_rif_sg556",
    40: "ssg08/weapon_snip_ssg08", 60: "m4a1_silencer/weapon_rif_m4a1_silencer",
    61: "usp_silencer/weapon_pist_usp_silencer", 63: "cz75a/weapon_pist_cz75a",
    64: "revolver/weapon_pist_revolver",
}

COMPONENT_DTYPE = {5123: np.uint16, 5125: np.uint32, 5126: np.float32}
COMPONENT_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def load_accessor(document, blob, index):
    source = document["accessors"][index]
    view = document["bufferViews"][source["bufferView"]]
    dtype = np.dtype(COMPONENT_DTYPE[source["componentType"]]).newbyteorder("<")
    width = COMPONENT_COUNT[source["type"]]
    offset = view.get("byteOffset", 0) + source.get("byteOffset", 0)
    stride = view.get("byteStride", dtype.itemsize * width)
    result = np.empty((source["count"], width), dtype=dtype)
    for row in range(source["count"]):
        result[row] = np.frombuffer(blob, dtype=dtype, count=width, offset=offset + row * stride)
    return result


def barycentric(point, triangle):
    a, b, c = triangle
    basis = np.column_stack([b - a, c - a])
    determinant = np.linalg.det(basis)
    if abs(determinant) < 1e-12:
        return None
    tail = np.linalg.solve(basis, point - a)
    return np.array([1.0 - tail[0] - tail[1], tail[0], tail[1]])


def parse_markup(text):
    marker = text.find("\n\tStickerMarkup =")
    if marker < 0:
        raise RuntimeError("compiled model DATA has no StickerMarkup")
    tail = text[marker:]
    end = tail.find("\n\tweapon_metadata =")
    if end >= 0:
        tail = tail[:end]
    pattern = re.compile(
        r"\{\s*Index = (?P<index>\d+)\s*Mesh = \"(?P<mesh>[^\"]+)\"(?P<meta>.*?)"
        r"Polygons =\s*\[\s*\{\s*Vertices =\s*\[(?P<vertices>.*?)\]\s*,?\s*\}\s*,?\s*\]",
        re.DOTALL,
    )
    variants = {False: {}, True: {}}
    for match in pattern.finditer(tail):
        meta = match.group("meta")
        offset = re.search(r"Offset = \[\s*([-+0-9.eE]+),\s*([-+0-9.eE]+)\s*\]", meta)
        scale = re.search(r"\n\s*Scale = ([-+0-9.eE]+)", meta)
        if not offset or not scale:
            continue
        numbers = [float(value) for value in re.findall(
            r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?", match.group("vertices"))]
        if len(numbers) % 6 != 0:
            raise RuntimeError(f"unexpected StickerMarkup payload for {match.group('mesh')}")
        legacy = "LegacyModel = true" in meta or "legacy" in match.group("mesh").casefold()
        variants[legacy][int(match.group("index"))] = {
            "offset": [float(offset.group(1)), float(offset.group(2))],
            "scale": float(scale.group(1)),
            "triangles": np.asarray(numbers).reshape((-1, 3, 2)),
        }
    for legacy, slots in variants.items():
        if len(slots) < 4:
            raise RuntimeError(f"StickerMarkup {'legacy' if legacy else 'hd'} has only {len(slots)} slots")
    return variants


def load_markup(vpk, vrf_cli):
    spec = importlib.util.spec_from_file_location("inspect_vpk", "tools/inspect_vpk_entries.py")
    inspect_vpk = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = inspect_vpk
    spec.loader.exec_module(inspect_vpk)
    header_size, tree_size, entries = inspect_vpk.parse_directory(vpk)
    entries_by_path = {entry.path.casefold(): entry for entry in entries}
    resource_root = ROOT / "sticker-markup-resources"
    resource_root.mkdir(parents=True, exist_ok=True)
    result = {}
    for definition, model in MODELS.items():
        resource = f"weapons/models/{model}.vmdl_c"
        entry = entries_by_path.get(resource.casefold())
        if entry is None:
            raise FileNotFoundError(resource)
        path = resource_root / f"{definition}.vmdl_c"
        path.write_bytes(inspect_vpk.extract_entry(vpk, header_size, tree_size, entry))
        completed = subprocess.run(
            [str(vrf_cli), "-i", str(path), "-b", "DATA"], check=True,
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        result[definition] = parse_markup(completed.stdout)
        print(f"markup {definition}: hd={len(result[definition][False])} legacy={len(result[definition][True])}")
    return result


def collect_mesh(document, blob, legacy):
    wanted = "body_legacy" if legacy else "body_hd"
    mesh = next((item for item in document["meshes"] if wanted in item.get("name", "").casefold()), None)
    if mesh is None:
        raise RuntimeError(f"glTF has no {wanted} mesh")
    uv_triangles, position_triangles, all_positions = [], [], []
    for primitive in mesh["primitives"]:
        attributes = primitive.get("attributes", {})
        if "POSITION" not in attributes or "TEXCOORD_1" not in attributes:
            continue
        positions = load_accessor(document, blob, attributes["POSITION"])
        uv = load_accessor(document, blob, attributes["TEXCOORD_1"])
        indices = load_accessor(document, blob, primitive["indices"]).reshape(-1).astype(np.int64).reshape((-1, 3))
        all_positions.append(positions)
        uv_triangles.append(uv[indices])
        position_triangles.append(positions[indices])
    if not uv_triangles:
        raise RuntimeError(f"{wanted} has no POSITION/TEXCOORD_1 triangles")
    return (np.concatenate(uv_triangles), np.concatenate(position_triangles)), np.concatenate(all_positions)


def mapping_at(offset, triangles):
    target = np.asarray(offset, dtype=np.float64) + 0.5
    uv, positions = triangles
    a = uv[:, 0]
    ab = uv[:, 1] - a
    ac = uv[:, 2] - a
    relative = target - a
    determinant = ab[:, 0] * ac[:, 1] - ab[:, 1] * ac[:, 0]
    usable = np.abs(determinant) > 1e-12
    w1 = np.zeros_like(determinant)
    w2 = np.zeros_like(determinant)
    w1[usable] = (relative[usable, 0] * ac[usable, 1] - relative[usable, 1] * ac[usable, 0]) / determinant[usable]
    w2[usable] = (ab[usable, 0] * relative[usable, 1] - ab[usable, 1] * relative[usable, 0]) / determinant[usable]
    w0 = 1.0 - w1 - w2
    usable &= (w0 >= -1e-5) & (w1 >= -1e-5) & (w2 >= -1e-5)
    indices = np.flatnonzero(usable)
    if len(indices) == 0:
        # Authored region edges can extend a few texels beyond the exported
        # triangle after float quantisation. Continue through the nearest UV
        # triangle instead of introducing a discontinuity at that edge.
        valid = np.flatnonzero(np.abs(determinant) > 1e-12)
        penalty = (np.maximum(-w0[valid], 0.0) + np.maximum(-w1[valid], 0.0) +
                   np.maximum(-w2[valid], 0.0) + np.maximum(w0[valid] - 1.0, 0.0) +
                   np.maximum(w1[valid] - 1.0, 0.0) + np.maximum(w2[valid] - 1.0, 0.0))
        best = penalty.min()
        indices = valid[penalty <= best + 1e-5]
    points = (w0[indices, None] * positions[indices, 0] +
              w1[indices, None] * positions[indices, 1] +
              w2[indices, None] * positions[indices, 2])
    return points[np.argmax(points[:, 0])]


def make_projector(positions):
    minimum, maximum = positions.min(axis=0), positions.max(axis=0)
    length = max(1e-6, maximum[2] - minimum[2])
    height = max(1e-6, maximum[1] - minimum[1])
    eye = np.array([maximum[0] + 0.285 * length, maximum[1] + 0.32 * height, minimum[2] - 0.19 * length])
    target = np.array([(minimum[0] + maximum[0]) * 0.5, minimum[1] + 0.58 * height, minimum[2] + 0.49 * length])
    forward = target - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, np.array([0.0, 1.0, 0.0]))
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    focal = 1.0 / math.tan(math.radians(52.0) * 0.5)

    def raw(points):
        relative = points - eye
        camera = np.column_stack([relative @ right, relative @ up, relative @ forward])
        return np.column_stack([0.5 + 0.5 * focal * camera[:, 0] / camera[:, 2],
                                0.5 - 0.5 * focal * camera[:, 1] / camera[:, 2]])

    raw_model = raw(positions)
    minimum2, maximum2 = raw_model.min(axis=0), raw_model.max(axis=0)
    span = maximum2 - minimum2
    scale = min(0.94 / max(span[0], 1e-6), 0.82 / max(span[1], 1e-6))
    center = (minimum2 + maximum2) * 0.5
    return lambda points: (raw(points) - center) * scale + np.array([0.5, 0.53])


def simplify_polyline(points, epsilon=1.5):
    if len(points) <= 2:
        return points
    start, end = np.asarray(points[0], dtype=float), np.asarray(points[-1], dtype=float)
    segment = end - start
    length = np.linalg.norm(segment)
    if length < 1e-6:
        distances = [np.linalg.norm(np.asarray(point) - start) for point in points]
    else:
        distances = []
        for point in points:
            delta = np.asarray(point) - start
            cross = segment[0] * delta[1] - segment[1] * delta[0]
            distances.append(abs(cross) / length)
    index = int(np.argmax(distances))
    if distances[index] <= epsilon:
        return [points[0], points[-1]]
    return (simplify_polyline(points[:index + 1], epsilon)[:-1] +
            simplify_polyline(points[index:], epsilon))


def raster_outline(mesh_triangles, project):
    mask = Image.new("1", (MASK_WIDTH, MASK_HEIGHT), 0)
    draw = ImageDraw.Draw(mask)
    for positions3 in mesh_triangles[1]:
        preview = project(positions3)
        draw.polygon([(round(p[0] * (MASK_WIDTH - 1)), round(p[1] * (MASK_HEIGHT - 1))) for p in preview], fill=1)
    pixels, left, right = mask.load(), [], []
    for y in range(MASK_HEIGHT):
        occupied = [x for x in range(MASK_WIDTH) if pixels[x, y]]
        if occupied:
            left.append((min(occupied), y))
            right.append((max(occupied), y))
    outline = simplify_polyline(left) + simplify_polyline(list(reversed(right)))
    return outline, mask


def cpp_float(value):
    return f"{0.0 if abs(value) < 5e-9 else value:.8f}f"


def generate(markup):
    layouts, surface_triangles, silhouette_points = [], [], []
    montage = Image.new("RGB", (720, 300 * len(MODELS)), (22, 25, 31))
    montage_draw = ImageDraw.Draw(montage)
    for row, definition in enumerate(MODELS):
        gltf = MODEL_ROOT / f"{definition}.gltf"
        document = json.loads(gltf.read_text(encoding="utf-8"))
        blob = gltf.with_suffix(".bin").read_bytes()
        for column, legacy in enumerate((True, False)):
            mesh_triangles, positions = collect_mesh(document, blob, legacy)
            project = make_projector(positions)
            slots = markup[definition][legacy]
            slot_surfaces = []
            for slot in range(5):
                source = slots.get(slot)
                if source is None:
                    slot_surfaces.append((0, 0))
                    continue
                first_triangle = len(surface_triangles)
                for offset3 in source["triangles"]:
                    preview3 = np.asarray([
                        project(mapping_at(offset, mesh_triangles)[None, :])[0]
                        for offset in offset3
                    ])
                    surface_triangles.append((offset3, preview3))
                slot_surfaces.append((first_triangle, len(surface_triangles) - first_triangle))
            outline, mask = raster_outline(mesh_triangles, project)
            first_point = len(silhouette_points)
            silhouette_points.extend(outline)
            native = []
            for slot in range(5):
                source = slots.get(slot)
                native.append((source["offset"][0], source["offset"][1], source["scale"]) if source else (0.0, 0.0, 0.0))
            layouts.append((definition, legacy, native, slot_surfaces, first_point, len(outline)))

            x0, y0 = column * 360, row * 300
            scaled = mask.resize((340, 149), Image.Resampling.NEAREST)
            montage.paste(Image.new("RGB", scaled.size, (67, 75, 86)), (x0 + 10, y0 + 32), scaled)
            montage_draw.text((x0 + 10, y0 + 8), f"{definition} {'legacy' if legacy else 'hd'}", fill=(235, 238, 243))
            for slot, source in sorted(slots.items()):
                point = project(mapping_at(source["offset"], mesh_triangles)[None, :])[0]
                x, y = x0 + 10 + point[0] * 340, y0 + 32 + point[1] * 149
                montage_draw.ellipse((x - 7, y - 7, x + 7, y + 7), fill=(255, 210, 54), outline="white")
                montage_draw.text((x - 3, y - 6), str(slot + 1), fill=(20, 23, 28))

    lines = [
        "#pragma once", "", '#include "Core/StickerPlacement.hpp"', "",
        "#include <array>", "#include <cstddef>", "#include <cstdint>", "#include <limits>", "",
        "namespace nskinzcs2::ui::sticker_preview", "{",
        "// Generated from CS2 VMDL StickerMarkup and the matching legacy/HD mesh.",
        "// The complete authored surface is piecewise projected; no point-fit matrix is used.",
        "struct NativeSlot { float offset_x; float offset_y; float authored_scale; };",
        "struct PreviewPoint { float x; float y; };",
        "struct SurfaceTriangle { PreviewPoint offset[3]; PreviewPoint preview[3]; };",
        "struct SurfaceRange { std::uint32_t first_triangle; std::uint16_t triangle_count; };",
        "struct SilhouettePoint { std::uint8_t x; std::uint8_t y; };",
        "struct WeaponLayout { int definition; bool legacy_model; std::array<NativeSlot, 5> slots; std::array<SurfaceRange, 5> surfaces; std::uint32_t first_point; std::uint16_t point_count; };",
        f"inline constexpr std::uint16_t kSilhouetteWidth = {MASK_WIDTH};",
        f"inline constexpr std::uint16_t kSilhouetteHeight = {MASK_HEIGHT};", "",
        f"inline constexpr std::array<SurfaceTriangle, {len(surface_triangles)}> kSurfaceTriangles {{{{",
    ]
    for offset3, preview3 in surface_triangles:
        offsets = ", ".join(f"{{{cpp_float(p[0])}, {cpp_float(p[1])}}}" for p in offset3)
        previews = ", ".join(f"{{{cpp_float(p[0])}, {cpp_float(p[1])}}}" for p in preview3)
        lines.append(f"    {{{{{offsets}}}, {{{previews}}}}},")
    lines.extend(["}};", "", f"inline constexpr std::array<SilhouettePoint, {len(silhouette_points)}> kSilhouettePoints {{{{"])
    lines.extend(f"    {{{x}, {y}}}," for x, y in silhouette_points)
    lines.extend(["}};", "", f"inline constexpr std::array<WeaponLayout, {len(layouts)}> kWeaponLayouts {{{{"])
    for definition, legacy, native, slot_surfaces, first_point, point_count in layouts:
        slots = ", ".join(f"{{{cpp_float(x)}, {cpp_float(y)}, {cpp_float(scale)}}}" for x, y, scale in native)
        surfaces = ", ".join(f"{{{first}, {count}}}" for first, count in slot_surfaces)
        lines.append(f"    {{{definition}, {'true' if legacy else 'false'}, {{{{{slots}}}}}, {{{{{surfaces}}}}}, {first_point}, {point_count}}},")
    lines.extend([
        "}};", "inline constexpr const auto& kPreviewLayouts = kWeaponLayouts;", "",
        "inline const WeaponLayout* FindWeaponLayout(const int definition, const bool legacy_model = true) noexcept",
        "{ for (const auto& layout : kWeaponLayouts) if (layout.definition == definition && layout.legacy_model == legacy_model) return &layout; return nullptr; }",
        "inline const WeaponLayout* FindPreviewLayout(const int definition, const bool legacy_model = true) noexcept { return FindWeaponLayout(definition, legacy_model); }", "",
        "inline NativeSlot NativePreset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept",
        "{ const auto* layout = FindWeaponLayout(definition, legacy_model); return layout != nullptr && slot < layout->slots.size() ? layout->slots[slot] : NativeSlot {}; }",
        "inline bool HasNativePreset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept { return NativePreset(definition, slot, legacy_model).authored_scale > 0.0f; }", "",
        "inline NativeSlot EffectivePreset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept",
        "{ const auto native = NativePreset(definition, slot, legacy_model); if (native.authored_scale > 0.0f) return native; const auto placement = nskinzcs2::model::ResolveStickerAnchor(definition, slot, legacy_model); if (!placement.BorrowsAnotherSlot()) return {}; const auto anchor = NativePreset(definition, placement.schema, legacy_model); return {anchor.offset_x + placement.offset_x, anchor.offset_y + placement.offset_y, anchor.authored_scale}; }",
        "inline NativeSlot DefaultPreset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept { return EffectivePreset(definition, slot, legacy_model); }",
        "inline PreviewPoint MaterialBaseOffset(const int definition, const std::size_t slot, const bool legacy_model = true) noexcept { const auto preset = EffectivePreset(definition, slot, legacy_model); return {preset.offset_x, preset.offset_y}; }",
        "inline PreviewPoint StoredToMaterialOffset(const int definition, const std::size_t slot, const float x, const float y, const bool legacy_model = true) noexcept { const auto base = MaterialBaseOffset(definition, slot, legacy_model); return {base.x + x, base.y + y}; }",
        "inline PreviewPoint MaterialToStoredOffset(const int definition, const std::size_t slot, const float x, const float y, const bool legacy_model = true) noexcept { const auto base = MaterialBaseOffset(definition, slot, legacy_model); return {x - base.x, y - base.y}; }",
        "inline PreviewPoint DefaultStoredOffset(const int, const std::size_t, const bool = true) noexcept { return {}; }", "",
        "inline bool Weights(const PreviewPoint point, const PreviewPoint (&triangle)[3], std::array<float, 3>& weights) noexcept",
        "{ const auto ax = triangle[1].x - triangle[0].x; const auto ay = triangle[1].y - triangle[0].y; const auto bx = triangle[2].x - triangle[0].x; const auto by = triangle[2].y - triangle[0].y; const auto determinant = ax * by - ay * bx; if (determinant > -0.0000001f && determinant < 0.0000001f) return false; const auto px = point.x - triangle[0].x; const auto py = point.y - triangle[0].y; weights[1] = (px * by - py * bx) / determinant; weights[2] = (ax * py - ay * px) / determinant; weights[0] = 1.0f - weights[1] - weights[2]; return true; }", "",
        "inline bool ClosestPointWeights(const PreviewPoint point, const PreviewPoint (&triangle)[3], std::array<float, 3>& weights, float& distance) noexcept",
        "{ distance = std::numeric_limits<float>::max(); bool found = false; for (std::size_t edge = 0; edge < 3; ++edge) { const auto next = (edge + 1) % 3; const auto dx = triangle[next].x - triangle[edge].x; const auto dy = triangle[next].y - triangle[edge].y; const auto length = dx * dx + dy * dy; if (length <= 0.0000000001f) continue; auto t = ((point.x - triangle[edge].x) * dx + (point.y - triangle[edge].y) * dy) / length; t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); const auto x = triangle[edge].x + t * dx; const auto y = triangle[edge].y + t * dy; const auto candidate = (point.x - x) * (point.x - x) + (point.y - y) * (point.y - y); if (candidate < distance) { distance = candidate; weights = {}; weights[edge] = 1.0f - t; weights[next] = t; found = true; } } return found; }", "",
        "inline SurfaceRange SurfaceForSlot(const WeaponLayout& layout, const std::size_t slot) noexcept",
        "{ const auto projection_slot = nskinzcs2::model::ResolveStickerProjectionSlot(layout.definition, slot, layout.legacy_model); return projection_slot < layout.surfaces.size() ? layout.surfaces[projection_slot] : SurfaceRange {}; }", "",
        "inline PreviewPoint MapSurface(const SurfaceRange surface, const PreviewPoint point, const bool inverse, const PreviewPoint* hint = nullptr) noexcept",
        "{ const SurfaceTriangle* fallback = nullptr; std::array<float, 3> fallback_weights {}; auto fallback_distance = std::numeric_limits<float>::max(); PreviewPoint hinted_result {}; auto hinted_distance = std::numeric_limits<float>::max(); bool has_hinted_result = false; for (std::uint32_t index = surface.first_triangle; index < surface.first_triangle + surface.triangle_count; ++index) { const auto& triangle = kSurfaceTriangles[index]; const auto& source = inverse ? triangle.preview : triangle.offset; std::array<float, 3> weights {}; if (!Weights(point, source, weights)) continue; const auto& target = inverse ? triangle.offset : triangle.preview; const PreviewPoint mapped {weights[0] * target[0].x + weights[1] * target[1].x + weights[2] * target[2].x, weights[0] * target[0].y + weights[1] * target[1].y + weights[2] * target[2].y}; const bool inside = weights[0] >= -0.0001f && weights[1] >= -0.0001f && weights[2] >= -0.0001f; if (inside) { if (!inverse || hint == nullptr) return mapped; const auto distance = (mapped.x - hint->x) * (mapped.x - hint->x) + (mapped.y - hint->y) * (mapped.y - hint->y); if (distance < hinted_distance) { hinted_distance = distance; hinted_result = mapped; has_hinted_result = true; } continue; } if (inverse) { std::array<float, 3> closest {}; float distance = 0.0f; if (ClosestPointWeights(point, source, closest, distance) && distance < fallback_distance) { fallback_distance = distance; fallback = &triangle; fallback_weights = closest; } continue; } const auto cx = (source[0].x + source[1].x + source[2].x) / 3.0f; const auto cy = (source[0].y + source[1].y + source[2].y) / 3.0f; const auto distance = (point.x - cx) * (point.x - cx) + (point.y - cy) * (point.y - cy); if (distance < fallback_distance) { fallback_distance = distance; fallback = &triangle; fallback_weights = weights; } } if (has_hinted_result) return hinted_result; if (fallback == nullptr) return inverse ? (hint != nullptr ? *hint : PreviewPoint {}) : PreviewPoint {0.5f, 0.5f}; const auto& target = inverse ? fallback->offset : fallback->preview; return {fallback_weights[0] * target[0].x + fallback_weights[1] * target[1].x + fallback_weights[2] * target[2].x, fallback_weights[0] * target[0].y + fallback_weights[1] * target[1].y + fallback_weights[2] * target[2].y}; }", "",
        "inline PreviewPoint OffsetToPreview(const int definition, const std::size_t slot, const float x, const float y, const bool legacy_model = true) noexcept { const auto* layout = FindWeaponLayout(definition, legacy_model); return layout ? MapSurface(SurfaceForSlot(*layout, slot), {x, y}, false) : PreviewPoint {0.5f, 0.5f}; }",
        "inline PreviewPoint PreviewToOffset(const int definition, const std::size_t slot, const float x, const float y, const bool legacy_model = true, const PreviewPoint* hint = nullptr) noexcept { const auto* layout = FindWeaponLayout(definition, legacy_model); return layout ? MapSurface(SurfaceForSlot(*layout, slot), {x, y}, true, hint) : PreviewPoint {}; }",
        "inline PreviewPoint EffectiveOffsetToPreview(const int definition, const std::size_t slot, const bool legacy_model, const float x, const float y) noexcept { return OffsetToPreview(definition, slot, x, y, legacy_model); }",
        "inline PreviewPoint PreviewToEffectiveOffset(const int definition, const std::size_t slot, const bool legacy_model, const float x, const float y, const PreviewPoint* hint = nullptr) noexcept { return PreviewToOffset(definition, slot, x, y, legacy_model, hint); }",
        "}", "",
    ])
    content = "\n".join(lines)
    arrays_start = content.index("inline constexpr std::array<SurfaceTriangle")
    helpers_start = content.index("inline const WeaponLayout* FindWeaponLayout")
    prefix = content[:arrays_start]
    helpers = content[helpers_start:]
    declarations = (
        f"extern const std::array<SurfaceTriangle, {len(surface_triangles)}> kSurfaceTriangles;\n"
        f"extern const std::array<SilhouettePoint, {len(silhouette_points)}> kSilhouettePoints;\n"
        f"extern const std::array<WeaponLayout, {len(layouts)}> kWeaponLayouts;\n"
        "inline const auto& kPreviewLayouts = kWeaponLayouts;\n\n"
    )
    OUTPUT.write_text(prefix + declarations + helpers, encoding="utf-8")

    arrays = content[arrays_start:helpers_start]
    arrays = arrays.replace("inline constexpr std::array", "const std::array")
    arrays = arrays.replace("inline constexpr const auto& kPreviewLayouts = kWeaponLayouts;\n", "")
    OUTPUT_CPP.write_text(
        '#include "UI/StickerPreviewPlacement.generated.hpp"\n\n'
        "namespace nskinzcs2::ui::sticker_preview\n{\n" + arrays + "}\n",
        encoding="utf-8")
    montage_path = ROOT / "sticker-surface-preview-montage.png"
    montage.save(montage_path)
    print(OUTPUT.resolve())
    print(OUTPUT_CPP.resolve())
    print(montage_path.resolve())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vpk", type=pathlib.Path, required=True)
    parser.add_argument("--vrf-cli", type=pathlib.Path, required=True)
    args = parser.parse_args()
    generate(load_markup(args.vpk, args.vrf_cli))


if __name__ == "__main__":
    main()
