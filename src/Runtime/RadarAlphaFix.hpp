#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nskinzcs2::runtime
{
enum class RadarAlphaFixOutcome
{
    Generated,
    NotNeeded,
    Unsupported,
    Failed,
};

struct RadarAlphaMaskSettings
{
    // Pixels at or below this neutral brightness are eligible for removal.
    // Higher values remove more grid/matte but can hide very dark roads.
    int black_tolerance = 24;
    // Restores a dark run only when retained map pixels bound it on opposite
    // sides. Values above four use stricter multi-axis enclosure and the
    // effective value is capped at six to avoid restoring long grid lines.
    int dark_detail_radius = 4;
    // Alpha applied to pixels classified as the baked background. Zero gives
    // the same fully transparent result as Valve''s HUD background setting.
    int background_alpha_percent = 0;
};

struct RadarAlphaOverlay
{
    std::string virtual_path;
    std::vector<std::uint8_t> png_bytes;
    // Populated only after CompileRadarOverlayWithCs2 succeeds. The in-memory
    // PNG wrapper used during early development is never exposed to CS2.
    std::vector<std::uint8_t> vtex_bytes;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t visible_pixels = 0;
    std::size_t transparent_pixels = 0;
    std::size_t softened_edge_pixels = 0;
    std::size_t recovered_detail_pixels = 0;
};

RadarAlphaFixOutcome BuildWorkshopRadarAlphaOverlay(
    std::string_view workshop_id,
    std::string_view target_map_name,
    RadarAlphaOverlay& output,
    std::wstring& diagnostic,
    DWORD cs2_process_id = 0,
    const RadarAlphaMaskSettings& settings = {});

// Kept public for deterministic tests and format audits. This does not touch
// any file; it returns the masked pixels as an encoded PNG for native compile.
RadarAlphaFixOutcome BuildRadarAlphaOverlayFromVtex(
    std::string_view virtual_path,
    const std::vector<std::uint8_t>& source_vtex,
    RadarAlphaOverlay& output,
    std::wstring& diagnostic,
    const RadarAlphaMaskSettings& settings = {});
}
