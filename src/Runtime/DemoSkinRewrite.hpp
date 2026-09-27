#pragma once

#include "Core/AppState.hpp"

#include <Windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nskinzcs2::runtime
{
inline constexpr wchar_t kCurrentDemoPathSidecarName[] = L"nSkinzCS2.current-demo.txt";
inline constexpr wchar_t kGeneratedDemoDirectoryName[] = L"nskinz_temp";
inline constexpr wchar_t kGeneratedDemoFilePrefix[] = L"nskinz_skin_";

struct DemoSkinRewritePass
{
    std::uint64_t xuid = 0;
    std::vector<std::string> item_json;
};

struct DemoSkinRewritePlan
{
    std::vector<DemoSkinRewritePass> passes;
    int knife_item_count = 0;
    int glove_item_count = 0;
};

struct DemoSkinRewriteResult
{
    bool success = false;
    std::filesystem::path source_demo;
    std::filesystem::path output_demo;
    std::string playdemo_command;
    std::wstring detail;
};

// The injected runtime writes this small sidecar only when the current demo
// path changes. The isolated UI process reads it; the original demo is never
// modified or copied over.
bool PublishCurrentDemoPathSidecar(
    const std::filesystem::path& dll_directory,
    const std::filesystem::path& demo_path,
    std::wstring& issue);

std::optional<std::filesystem::path> ReadCurrentDemoPathSidecar(
    const std::filesystem::path& dll_directory);

bool IsGeneratedDemoSkinPath(const std::filesystem::path& path) noexcept;

DemoSkinRewritePlan BuildDefaultDemoSkinRewritePlan(
    const model::ConfigDocument& config);

DemoSkinRewriteResult GenerateTemporarySkinnedDemo(
    const model::ConfigDocument& config,
    const std::filesystem::path& captured_source_demo,
    DWORD cs2_process_id);
}
