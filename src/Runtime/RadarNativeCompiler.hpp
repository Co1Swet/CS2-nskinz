#pragma once

#include "Runtime/RadarAlphaFix.hpp"

#include <filesystem>
#include <string>

namespace nskinzcs2::runtime
{
// Compiles the generated transparent PNG through CS2's own resourcecompiler.
// All input, output and dependency links live in an isolated cache beside the
// Steam library; no CS2 or Workshop file is changed.
bool CompileRadarOverlayWithCs2(
    const std::filesystem::path& cs2_install_root,
    RadarAlphaOverlay& overlay,
    std::wstring& diagnostic);

// Writes a successfully compiled resource to an overlay-only cache tree. The
// returned root is safe to add as a GAME search path and contains no copied
// gameinfo or shader packages from the compiler cache.
bool MaterializeCompiledRadarOverlay(
    const RadarAlphaOverlay& overlay,
    std::filesystem::path& overlay_root,
    std::filesystem::path& overlay_file,
    std::wstring& diagnostic);

// Lightweight fail-closed validation used by the injected runtime before it
// mounts a file prepared by the external control-panel process.
bool ValidateNativeRadarVtexFile(
    const std::filesystem::path& path,
    std::wstring& diagnostic);
}
