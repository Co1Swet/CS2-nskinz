// SPDX-License-Identifier: AGPL-3.0-only
// nSkinzCS2 Workshop discovery/integration for the MulNX-derived map-remap path;
// see NOTICE.md.

#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nskinzcs2::runtime
{
struct WorkshopMapEntry
{
    std::string workshop_id;
    std::string title;
    std::string addon_name;
    std::string map_name;
    std::filesystem::path item_directory;
    // Resource prefix (without .vpk/.vmap) of the map''s separately compiled
    // 3D skybox, when the Workshop package exposes exactly one unambiguous
    // skybox package. Used to avoid keeping the demo map''s old skybox alive.
    std::string skybox_resource_prefix;
};

struct WorkshopMapScanResult
{
    std::vector<WorkshopMapEntry> entries;
    std::vector<std::wstring> diagnostics;
};

struct WorkshopRadarTexture
{
    std::string virtual_path;
    std::vector<std::uint8_t> bytes;
};

// Workshop packages can contain auxiliary map packages (for example a 3D
// skybox world) next to the playable world. These must never be offered as a
// demo replacement target.
bool IsLikelyAuxiliaryWorkshopMap(std::string_view map_name);

// A Workshop addon can be distributed either as a split VPK whose index is
// named *_dir.vpk, or as one self-contained <workshop-id>.vpk. Numbered
// *_000.vpk files are split data archives and must not be parsed as indexes.
bool IsWorkshopVpkIndexFile(std::wstring_view filename);

// Reads Steam's local appworkshop_730.acf plus each installed item's VPK tree.
// No Steam Web API or workshop file mutation is involved.
WorkshopMapScanResult DiscoverSubscribedWorkshopMaps(DWORD cs2_process_id = 0);

// Reads the selected map's compiled radar texture directly from its installed
// Workshop VPK. The Workshop item and game installation remain untouched.
std::optional<WorkshopRadarTexture> ReadWorkshopRadarTexture(
    std::string_view workshop_id,
    std::string_view target_map_name,
    DWORD cs2_process_id = 0,
    std::wstring* diagnostic = nullptr);

}
