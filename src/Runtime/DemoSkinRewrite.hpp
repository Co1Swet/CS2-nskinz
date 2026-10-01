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
inline constexpr wchar_t kGeneratedDemoSkinManifestSuffix[] = L".nskinz.json";

struct DemoSkinRewritePass
{
    std::uint64_t xuid = 0;
    std::vector<std::string> item_json;
};

struct DemoSkinRewritePlan
{
    std::vector<DemoSkinRewritePass> passes;
    int weapon_item_count = 0;
    int sticker_slot_request_count = 0;
    int knife_item_count = 0;
    int glove_item_count = 0;
    std::wstring validation_issue;
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

// Generated demos can contain complete default-carrier firearm/glove records.
// Reconstructed firearms remain live for one native material initialization
// per entity generation; reconstructed gloves stay entirely stream-owned.
bool IsGeneratedDemoRewrittenWeaponProfile(
    const std::filesystem::path& generated_demo,
    std::uint64_t xuid,
    int base_item) noexcept;

bool ShouldSuppressGeneratedDemoGloveProfile(
    const std::filesystem::path& generated_demo,
    std::uint64_t xuid) noexcept;

DemoSkinRewritePlan BuildDefaultDemoSkinRewritePlan(
    const model::ConfigDocument& config,
    const model::Catalog* catalog = nullptr);

DemoSkinRewriteResult GenerateTemporarySkinnedDemo(
    const model::ConfigDocument& config,
    const model::Catalog* catalog,
    const std::filesystem::path& captured_source_demo,
    DWORD cs2_process_id);
}
