// SPDX-License-Identifier: AGPL-3.0-only
// Map-remap behavior adapted from Co1Swet/MulNX_CS2 commit
// 361008b208326d9e7916ff2cd58dd57a0492c400; see NOTICE.md.

#pragma once

#include "Core/AppState.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nskinzcs2::runtime
{
struct MapRemapStatus
{
    bool enabled = false;
    bool hooks_installed = false;
    bool source_captured = false;
    bool use_source_radar = true;
    std::string source_map_name;
    std::string workshop_id;
    std::string target_map_name;
    std::uint64_t replacement_count = 0;
    std::uint64_t suppressed_source_geometry_count = 0;
    std::wstring issue;
};

// Pure path classifier shared by the runtime hook and regression tests. It
// deliberately targets only map-generated models and a proven-different 3D
// skybox; ordinary player, weapon and shared prop models are never matched.
bool ShouldSuppressSourceMapModelPath(
    std::string_view resource_path,
    std::string_view source_map_name,
    std::string_view target_map_name,
    std::string_view target_skybox_resource_prefix) noexcept;

// Redirects only a source-map 3D-skybox entity reference (.vmap/.vmap_c) to
// the target map's known skybox resource. VPK archive requests are never
// redirected because Source 2 opens one package through several coordinated
// _dir/_flt/data stages that must retain the same archive identity.
bool BuildSourceSkyboxResourceRedirect(
    std::string_view resource_path,
    std::string_view source_map_name,
    std::string_view target_map_name,
    std::string_view target_skybox_resource_prefix,
    char* replacement,
    std::size_t replacement_size) noexcept;

// Redirects stale demo-map entity-lump and dynamic-lighting resource paths.
// A verified geometry-identical pair may deliberately keep the source probe
// atlas so dynamic demo models remain readable under the Workshop world.
// Numeric probe-octree handshakes are redirected only when a source/target
// entity-lump mapping has been verified; guessing an ID creates a nonexistent
// target path and black models.
bool BuildSourceMapRuntimeDataRedirect(
    std::string_view resource_path,
    std::string_view source_map_name,
    std::string_view target_map_name,
    char* replacement,
    std::size_t replacement_size) noexcept;

// Called on Update. Hooks are intentionally installed only after the user has
// staged a valid map replacement, before the second playdemo invocation.
void ConfigureMapRemap(
    const nskinzcs2::model::MapRemapProfile& profile,
    const std::wstring& current_level_name = {});
MapRemapStatus SnapshotMapRemapStatus();
void ShutdownMapRemap();
}
