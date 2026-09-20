#pragma once

#include "Core/AppState.hpp"

#include <Windows.h>

#include <filesystem>

namespace nskinzcs2::ui
{
// Launches the complete ImGui/D3D control panel in rundll32.exe.  No tool UI
// graphics or text-service objects are then created inside cs2.exe.
bool LaunchExternalUiHost(HMODULE module, DWORD runtime_process_id);

// Saves the complete editor document and asks the injected runtime to reload
// it before its next manual refresh.
bool PublishExternalConfigUpdate(
    DWORD runtime_process_id,
    const std::filesystem::path& config_path,
    model::ConfigDocument& document);
}

extern "C" __declspec(dllexport) void CALLBACK nSkinzUiHost(
    HWND rundll_window,
    HINSTANCE module,
    LPSTR command_line,
    int show_command);
