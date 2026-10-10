#!/usr/bin/env python3
"""Generate compact ImGui weapon-preview geometry from decompiled CS2 SVGs.

The input directory must contain the official Panorama equipment icons exported
from the locally installed CS2 VPK, for example with ValveResourceFormat:

  Source2Viewer-CLI.exe -i pak01_dir.vpk -o <dir> -d \
    -e vsvg_c -f panorama/images/icons/equipment/

Only the five path commands currently used by the equipment icons are needed,
but the parser handles their absolute/relative variants for reproducibility.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import re
import xml.etree.ElementTree as ET


WEAPONS = (
    (1, "deagle"),
    (2, "elite"),
    (3, "fiveseven"),
    (4, "glock"),
    (7, "ak47"),
    (8, "aug"),
    (9, "awp"),
    (10, "famas"),
    (11, "g3sg1"),
    (13, "galilar"),
    (14, "m249"),
    (16, "m4a1"),
    (17, "mac10"),
    (19, "p90"),
    (23, "mp5sd"),
    (24, "ump45"),
    (25, "xm1014"),
    (26, "bizon"),
    (27, "mag7"),
    (28, "negev"),
    (29, "sawedoff"),
    (30, "tec9"),
    (32, "hkp2000"),
    (33, "mp7"),
    (34, "mp9"),
    (35, "nova"),
    (36, "p250"),
    (38, "scar20"),
    (39, "sg556"),
    (40, "ssg08"),
    (60, "m4a1_silencer"),
    (61, "usp_silencer"),
    (63, "cz75a"),
    (64, "revolver"),
)

TOKEN = re.compile(r"[A-Za-z]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


def cubic_point(p0, p1, p2, p3, t: float):
    u = 1.0 - t
    return (
        u * u * u * p0[0]
        + 3.0 * u * u * t * p1[0]
        + 3.0 * u * t * t * p2[0]
        + t * t * t * p3[0],
        u * u * u * p0[1]
        + 3.0 * u * u * t * p1[1]
        + 3.0 * u * t * t * p2[1]
        + t * t * t * p3[1],
    )


def flatten_path(data: str):
    tokens = TOKEN.findall(data)
    index = 0
    command = ""
    current = (0.0, 0.0)
    start = current
    last_control = None
    paths = []
    path = []

    def is_command(value: str) -> bool:
        return len(value) == 1 and value.isalpha()

    def number() -> float:
        nonlocal index
        value = float(tokens[index])
        index += 1
        return value

    def finish():
        nonlocal path
        if len(path) > 1 and path[-1] == path[0]:
            path.pop()
        if len(path) >= 3:
            paths.append(path)
        path = []

    while index < len(tokens):
        if is_command(tokens[index]):
            command = tokens[index]
            index += 1
            if command in "Zz":
                if path and path[-1] != start:
                    path.append(start)
                current = start
                last_control = None
                finish()
                continue
        if not command:
            raise ValueError("SVG path data began without a command")

        relative = command.islower()
        op = command.upper()
        if op == "M":
            x, y = number(), number()
            if relative:
                x += current[0]
                y += current[1]
            if path:
                finish()
            current = (x, y)
            start = current
            path.append(current)
            last_control = None
            command = "l" if relative else "L"
        elif op == "L":
            x, y = number(), number()
            if relative:
                x += current[0]
                y += current[1]
            current = (x, y)
            path.append(current)
            last_control = None
        elif op == "H":
            x = number() + (current[0] if relative else 0.0)
            current = (x, current[1])
            path.append(current)
            last_control = None
        elif op == "V":
            y = number() + (current[1] if relative else 0.0)
            current = (current[0], y)
            path.append(current)
            last_control = None
        elif op in ("C", "S"):
            if op == "C":
                x1, y1, x2, y2, x3, y3 = (number() for _ in range(6))
                if relative:
                    x1 += current[0]
                    y1 += current[1]
                    x2 += current[0]
                    y2 += current[1]
                    x3 += current[0]
                    y3 += current[1]
                control1 = (x1, y1)
            else:
                x2, y2, x3, y3 = (number() for _ in range(4))
                if relative:
                    x2 += current[0]
                    y2 += current[1]
                    x3 += current[0]
                    y3 += current[1]
                control1 = (
                    2.0 * current[0] - last_control[0],
                    2.0 * current[1] - last_control[1],
                ) if last_control is not None else current
            control2 = (x2, y2)
            destination = (x3, y3)
            control_length = (
                math.dist(current, control1)
                + math.dist(control1, control2)
                + math.dist(control2, destination)
            )
            segments = max(2, min(6, math.ceil(control_length / 1.75)))
            for step in range(1, segments + 1):
                path.append(cubic_point(current, control1, control2, destination, step / segments))
            current = destination
            last_control = control2
        else:
            raise ValueError(f"unsupported SVG path command {command!r}")

        if index < len(tokens) and is_command(tokens[index]):
            continue

    if path:
        finish()
    return paths


def simplify_points(points, tolerance: float):
    """Ramer-Douglas-Peucker reduction at sub-pixel source precision."""
    if len(points) <= 3:
        return points

    def distance_to_segment(point, start, end):
        dx = end[0] - start[0]
        dy = end[1] - start[1]
        length_squared = dx * dx + dy * dy
        if length_squared == 0.0:
            return math.dist(point, start)
        factor = max(0.0, min(1.0, ((point[0] - start[0]) * dx + (point[1] - start[1]) * dy) / length_squared))
        projection = (start[0] + factor * dx, start[1] + factor * dy)
        return math.dist(point, projection)

    def reduce(first: int, last: int, keep):
        furthest = 0.0
        furthest_index = -1
        for candidate in range(first + 1, last):
            distance = distance_to_segment(points[candidate], points[first], points[last])
            if distance > furthest:
                furthest = distance
                furthest_index = candidate
        if furthest_index >= 0 and furthest > tolerance:
            keep.add(furthest_index)
            reduce(first, furthest_index, keep)
            reduce(furthest_index, last, keep)

    keep = {0, len(points) - 1}
    reduce(0, len(points) - 1, keep)
    return [points[index] for index in sorted(keep)]


def read_geometry(path: pathlib.Path):
    root = ET.parse(path).getroot()
    view_box = root.attrib.get("viewBox")
    if view_box:
        x0, y0, width, height = map(float, view_box.replace(",", " ").split())
    else:
        width = float(root.attrib["width"].removesuffix("px"))
        height = float(root.attrib["height"].removesuffix("px"))
        x0 = y0 = 0.0
    paths = []
    for element in root.iter():
        if element.tag.rsplit("}", 1)[-1] == "path" and element.attrib.get("d"):
            paths.extend(flatten_path(element.attrib["d"]))
    normalized = []
    for points in paths:
        points = simplify_points(points, max(width, height) * 0.00075)
        normalized.append([
            (
                max(0, min(65535, round((x - x0) / width * 65535))),
                max(0, min(65535, round((y - y0) / height * 65535))),
            )
            for x, y in points
        ])
    return width / height, normalized


def write_header(source_dir: pathlib.Path, output: pathlib.Path):
    points = []
    paths = []
    models = []
    for definition, name in WEAPONS:
        svg = source_dir / f"{name}.svg"
        if not svg.is_file():
            raise FileNotFoundError(svg)
        aspect, model_paths = read_geometry(svg)
        first_path = len(paths)
        for model_path in model_paths:
            first_point = len(points)
            points.extend(model_path)
            paths.append((first_point, len(model_path)))
        models.append((definition, name, aspect, first_path, len(model_paths)))

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/generate_weapon_preview_geometry.py from the",
        "// locally installed CS2 Panorama equipment icons. Do not edit by hand.",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace nskinzcs2::ui::weapon_preview",
        "{",
        "struct Point { std::uint16_t x; std::uint16_t y; };",
        "struct Path { std::uint32_t first_point; std::uint16_t point_count; };",
        "struct Model { int definition; std::string_view name; float aspect; std::uint32_t first_path; std::uint16_t path_count; };",
        "",
        f"inline constexpr std::array<Point, {len(points)}> kPoints {{",
    ]
    for offset in range(0, len(points), 8):
        chunk = points[offset:offset + 8]
        lines.append("    " + ", ".join(f"Point{{{x}, {y}}}" for x, y in chunk) + ",")
    lines.extend(("};", "", f"inline constexpr std::array<Path, {len(paths)}> kPaths {{"))
    for first, count in paths:
        lines.append(f"    Path{{{first}, {count}}},")
    lines.extend(("};", "", f"inline constexpr std::array<Model, {len(models)}> kModels {{"))
    for definition, name, aspect, first, count in models:
        lines.append(f'    Model{{{definition}, "{name}", {aspect:.8f}f, {first}, {count}}},')
    lines.extend(("};", "", "} // namespace nskinzcs2::ui::weapon_preview", ""))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8")
    print(f"generated {output}: {len(models)} models, {len(paths)} paths, {len(points)} points")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    write_header(args.source_dir, args.output)


if __name__ == "__main__":
    main()
