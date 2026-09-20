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
};

RadarAlphaFixOutcome BuildWorkshopRadarAlphaOverlay(
    std::string_view workshop_id,
    std::string_view target_map_name,
    RadarAlphaOverlay& output,
    std::wstring& diagnostic,
    DWORD cs2_process_id = 0);

// Kept public for deterministic tests and format audits. This does not touch
// any file; it returns the masked pixels as an encoded PNG for native compile.
RadarAlphaFixOutcome BuildRadarAlphaOverlayFromVtex(
    std::string_view virtual_path,
    const std::vector<std::uint8_t>& source_vtex,
    RadarAlphaOverlay& output,
    std::wstring& diagnostic);
}
