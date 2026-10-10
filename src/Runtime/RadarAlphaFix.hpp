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
    int black_tolerance = 12;
    // Restores a dark run only when retained map pixels bound it on opposite
    // sides. The generic control accepts up to 64; known wide-road maps may
    // calibrate that input up to a 96-pixel internal search radius.
    // unbounded exterior matte remains excluded.
    int dark_detail_radius = 12;
    // Alpha applied to pixels classified as the baked background. Zero gives
    // the same fully transparent result as Valve''s HUD background setting.
    int background_alpha_percent = 0;
};

struct RadarAlphaOverlay
{
    // Original Workshop resource requested by Panorama. virtual_path is a
    // content-addressed compile alias so regenerated masks never reuse an old
    // native resource name.
    std::string request_virtual_path;
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
    // Effective radius after map-specific calibration. This is exposed for
    // diagnostics so a changed slider value can never be silently ignored.
    int applied_dark_detail_radius = 0;
    // Effective cutoff after the source texture's opaque neutral matte has
    // been sampled and the user's value has been applied as a signed offset.
    int applied_black_tolerance = 0;
    int detected_neutral_mode = 0;
    std::size_t detected_neutral_pixels = 0;
    std::size_t detected_neutral_mode_pixels = 0;
    // True when an audited map-specific alpha reference replaced heuristic
    // matte detection. The reference is embedded in the binary, so the same
    // mask is used on every machine and does not depend on a local file.
    bool used_map_reference_mask = false;
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
