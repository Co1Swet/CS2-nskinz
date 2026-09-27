// SPDX-License-Identifier: AGPL-3.0-only
// Map-remap behavior adapted from Co1Swet/MulNX_CS2 commit
// 361008b208326d9e7916ff2cd58dd57a0492c400, with the current GameSession
// parser signature/layout synchronized from f2466e18e26a8fc3daf4ac7945bc45220ba95993;
// see NOTICE.md.

#include "Runtime/MapRemap.hpp"

#include "Core/Log.hpp"
#include "Runtime/RadarNativeCompiler.hpp"

#include "MinHook.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
enum class MessageKind : std::uint32_t
{
    DemoHeader = 0,
    SpawnGroup = 1,
    ClearStringTables = 2,
    ServerInfo = 3,
    GameSession = 4,
    MapKeyValues = 5,
    FailedResourceLog = 6,
    ManifestPath = 7,
};

struct TextRegion
{
    std::uint8_t* begin = nullptr;
    std::size_t size = 0;
};

struct InlineHook
{
    void* target = nullptr;
    void* stub = nullptr;
    void* trampoline = nullptr;
    bool installed = false;
};

struct RuntimeConfig
{
    bool enabled = false;
    bool source_captured = false;
    bool current_header_matched = false;
    bool use_source_radar = true;
    char source_map[256] {};
    char workshop_id[64] {};
    char target_map[256] {};
    char target_skybox_resource_prefix[512] {};
    char source_overview[512] {};
    char target_overview[512] {};
};

using UtlStringAssignFn = void(__fastcall*)(void*, const char*, std::uint64_t);
using KeyValuesSetStringFn = void(__fastcall*)(void*, const char*, const char*);
using RequestResourceByHashFn = void*(__fastcall*)(void*, std::uint64_t, std::uint64_t);
using FindResourceByHashFn = void*(__fastcall*)(void*, std::uint64_t);
using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
using FileSystemOpenFn = void*(__fastcall*)(void*, const char*, const char*, const char*);
using FileSystemReadFileFn = bool(__fastcall*)(
    void*,
    const char*,
    const char*,
    void*,
    int,
    int,
    void*);
using FileSystemAddSearchPathFn = void(__fastcall*)(void*, const char*, const char*, int, int, int);
using FileSystemRemoveSearchPathFn = bool(__fastcall*)(void*, const char*, const char*);

SRWLOCK g_config_lock = SRWLOCK_INIT;
SRWLOCK g_failed_resource_lock = SRWLOCK_INIT;
RuntimeConfig g_config;
std::array<InlineHook, 8> g_hooks;
InlineHook g_resource_request_hook;
InlineHook g_file_system_open_hook;
InlineHook g_file_system_read_file_hook;
UtlStringAssignFn g_assign = nullptr;
KeyValuesSetStringFn g_set_keyvalues_string = nullptr;
RequestResourceByHashFn g_request_resource_original = nullptr;
FindResourceByHashFn g_find_resource_by_hash = nullptr;
FileSystemOpenFn g_file_system_open_original = nullptr;
FileSystemReadFileFn g_file_system_read_file_original = nullptr;
void* g_file_system_interface = nullptr;
FileSystemAddSearchPathFn g_file_system_add_search_path = nullptr;
FileSystemRemoveSearchPathFn g_file_system_remove_search_path = nullptr;
std::array<char, 1024> g_radar_overlay_search_path {};
bool g_radar_overlay_mounted = false;
std::unordered_set<std::string> g_failed_model_paths;
std::atomic_bool g_hooks_installed {false};
std::atomic_bool g_source_radar_route_logged {false};
std::atomic_bool g_radar_overview_route_logged {false};
std::atomic_bool g_source_skybox_route_logged {false};
std::atomic_uint64_t g_replacement_count {0};
std::atomic_uint64_t g_suppressed_source_geometry_count {0};
std::atomic_uint32_t g_source_geometry_log_count {0};
std::atomic_uint32_t g_source_runtime_data_log_count {0};
std::mutex g_install_mutex;
std::mutex g_issue_mutex;
std::wstring g_issue;

constexpr std::array<std::uint8_t, 17> kDemoHeaderSignature {
    0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x85, 0x1F, 0xFF, 0xFF, 0xFF, 0xE9, 0x4A, 0x03, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 17> kSpawnGroupSignature {
    0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x85, 0x1C, 0xFF, 0xFF, 0xFF, 0xE9, 0x58, 0x05, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 17> kServerInfoSignature {
    0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x85, 0x14, 0xFC, 0xFF, 0xFF, 0xE9, 0x16, 0x01, 0x00, 0x00,
};
constexpr std::array<std::uint8_t, 14> kClearTablesSignature {
    0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x85, 0xC0, 0xFE, 0xFF, 0xFF, 0xEB, 0x60,
};
constexpr std::array<std::uint8_t, 21> kUtlStringAssignSignature {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x18, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0xD9, 0x4C,
};
constexpr std::array<std::uint8_t, 21> kRequestResourceSignature {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF8, 0x48, 0x8B, 0xDA,
};
constexpr std::array<std::uint8_t, 18> kFindResourceSignature {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20,
};
constexpr char kSafeManifestModel[] = "models/tools/bullet_hit_marker.vmdl";
constexpr std::size_t kFileSystemOpenSlot = 13;
constexpr std::size_t kFileSystemReadFileSlot = 25;
constexpr std::size_t kFileSystemAddSearchPathSlot = 31;
constexpr std::size_t kFileSystemRemoveSearchPathSlot = 32;

void SetIssue(std::wstring issue)
{
    std::lock_guard<std::mutex> lock(g_issue_mutex);
    g_issue = std::move(issue);
}

std::wstring Utf8ToWide(const std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
    {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string WideToUtf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

bool IsSafeMapName(const std::string_view value, const bool allow_empty)
{
    if (value.empty())
    {
        return allow_empty;
    }
    if (value.size() >= 256)
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char character)
    {
        return std::isalnum(character) != 0 || character == '_' || character == '-' ||
            character == '/' || character == '\\' || character == '.';
    });
}

bool IsWorkshopId(const std::string_view value)
{
    return !value.empty() && value.size() < 64 &&
        std::all_of(value.begin(), value.end(), [](const unsigned char character)
        {
            return std::isdigit(character) != 0;
        });
}

std::optional<TextRegion> GetTextRegion(const HMODULE module)
{
    if (module == nullptr)
    {
        return std::nullopt;
    }
    __try
    {
        auto* base = reinterpret_cast<std::uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return std::nullopt;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return std::nullopt;
        }
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (std::uint16_t index = 0; index < nt->FileHeader.NumberOfSections; ++index)
        {
            if (std::memcmp(section[index].Name, ".text", 5) == 0)
            {
                return TextRegion {
                    base + section[index].VirtualAddress,
                    static_cast<std::size_t>(section[index].Misc.VirtualSize),
                };
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return std::nullopt;
    }
    return std::nullopt;
}

template <std::size_t Size>
std::vector<std::uint8_t*> FindMatches(const TextRegion& region, const std::array<std::uint8_t, Size>& pattern)
{
    std::vector<std::uint8_t*> matches;
    if (region.begin == nullptr || region.size < Size)
    {
        return matches;
    }
    for (std::size_t offset = 0; offset + Size <= region.size; ++offset)
    {
        if (std::memcmp(region.begin + offset, pattern.data(), Size) == 0)
        {
            matches.push_back(region.begin + offset);
        }
    }
    return matches;
}

std::vector<std::uint8_t*> FindMaskedMatches(
    const TextRegion& region,
    const std::initializer_list<int> pattern)
{
    std::vector<std::uint8_t*> matches;
    if (region.begin == nullptr || region.size < pattern.size())
    {
        return matches;
    }
    std::vector<int> compiled(pattern);
    for (std::size_t offset = 0; offset + compiled.size() <= region.size; ++offset)
    {
        bool matched = true;
        for (std::size_t index = 0; index < compiled.size(); ++index)
        {
            if (compiled[index] >= 0 && region.begin[offset + index] != compiled[index])
            {
                matched = false;
                break;
            }
        }
        if (matched)
        {
            matches.push_back(region.begin + offset);
        }
    }
    return matches;
}

template <typename T>
void AppendValue(std::vector<std::uint8_t>& bytes, const T value)
{
    const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(value));
}

void AppendBytes(std::vector<std::uint8_t>& bytes, const std::initializer_list<std::uint8_t> values)
{
    bytes.insert(bytes.end(), values.begin(), values.end());
}

bool TryReadValue(const void* address, void* output, const std::size_t size) noexcept
{
    if (address == nullptr || output == nullptr)
    {
        return false;
    }
    __try
    {
        std::memcpy(output, address, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void* ResolveStringObject(void* message, const std::size_t offset) noexcept
{
    std::uintptr_t tagged = 0;
    if (!TryReadValue(static_cast<std::uint8_t*>(message) + offset, &tagged, sizeof(tagged)))
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(tagged & ~static_cast<std::uintptr_t>(3));
}

bool CopyUtlString(void* object, char* output, const std::size_t output_size) noexcept
{
    if (object == nullptr || output == nullptr || output_size < 2)
    {
        return false;
    }
    __try
    {
        const auto* bytes = static_cast<const std::uint8_t*>(object);
        const auto size = *reinterpret_cast<const std::uint64_t*>(bytes + 0x10);
        const auto capacity = *reinterpret_cast<const std::uint64_t*>(bytes + 0x18);
        if (size >= output_size || size > capacity || capacity > (1ull << 30))
        {
            return false;
        }
        const char* source = capacity >= 16
            ? *reinterpret_cast<char* const*>(bytes)
            : reinterpret_cast<const char*>(bytes);
        if (source == nullptr)
        {
            return false;
        }
        std::memcpy(output, source, static_cast<std::size_t>(size));
        output[size] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CopyCString(const char* source, char* output, const std::size_t output_size) noexcept
{
    if (source == nullptr || output == nullptr || output_size < 2)
    {
        return false;
    }
    __try
    {
        for (std::size_t index = 0; index < output_size; ++index)
        {
            output[index] = source[index];
            if (output[index] == '\0')
            {
                return true;
            }
        }
        output[output_size - 1] = '\0';
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        output[0] = '\0';
        return false;
    }
}

void LogMapReplacement(const wchar_t* stage, const char* source, const char* target) noexcept;

char NormalizePathCharacter(const char value) noexcept
{
    if (value == '\\')
    {
        return '/';
    }
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

std::string NormalizeResourcePath(std::string_view value)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const char character)
    {
        return NormalizePathCharacter(character);
    });
    while (normalized.starts_with("./"))
    {
        normalized.erase(0, 2);
    }
    return normalized;
}

std::string NormalizeMapLeaf(std::string_view value)
{
    auto normalized = NormalizeResourcePath(value);
    while (!normalized.empty() && normalized.back() == '/')
    {
        normalized.pop_back();
    }
    if (const auto slash = normalized.find_last_of('/'); slash != std::string::npos)
    {
        normalized.erase(0, slash + 1);
    }
    for (const std::string_view suffix : {".vmap_c", ".vmap", ".vpk"})
    {
        if (normalized.ends_with(suffix))
        {
            normalized.resize(normalized.size() - suffix.size());
            break;
        }
    }
    return normalized;
}

bool TargetUsesDifferentKnownSkybox(
    const std::string_view source_map_name,
    const std::string_view target_skybox_resource_prefix)
{
    const auto source = NormalizeMapLeaf(source_map_name);
    const auto target_skybox = NormalizeResourcePath(target_skybox_resource_prefix);
    if (source.empty() || target_skybox.empty())
    {
        return false;
    }
    const auto source_prefab = "maps/prefabs/" + source + "/";
    return !target_skybox.starts_with(source_prefab);
}

void LogSourceGeometrySuppression(
    const wchar_t* stage,
    const char* source,
    const char* target) noexcept
{
    g_replacement_count.fetch_add(1, std::memory_order_relaxed);
    g_suppressed_source_geometry_count.fetch_add(1, std::memory_order_relaxed);
    const auto log_index = g_source_geometry_log_count.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 12)
    {
        LogMapReplacement(stage, source, target);
    }
    else if (log_index == 12)
    {
        try
        {
            nskinzcs2::Logger::Info(
                L"[MapRemap/SourceGeometry] further original-map geometry suppressions are counted but not individually logged.");
        }
        catch (...)
        {
        }
    }
}

void LogSourceSkyboxRedirect(
    const wchar_t* stage,
    const char* source,
    const char* target) noexcept
{
    g_replacement_count.fetch_add(1, std::memory_order_relaxed);
    g_suppressed_source_geometry_count.fetch_add(1, std::memory_order_relaxed);
    if (!g_source_skybox_route_logged.exchange(true, std::memory_order_relaxed))
    {
        LogMapReplacement(stage, source, target);
    }
}

void LogSourceRuntimeDataRedirect(
    const wchar_t* stage,
    const char* source,
    const char* target) noexcept
{
    const auto log_index = g_source_runtime_data_log_count.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 12)
    {
        LogMapReplacement(stage, source, target);
    }
    else
    {
        g_replacement_count.fetch_add(1, std::memory_order_relaxed);
        if (log_index == 12)
        {
            try
            {
                nskinzcs2::Logger::Info(
                    L"[MapRemap/RuntimeData] further target entity/probe/cubemap routes are counted but not individually logged.");
            }
            catch (...)
            {
            }
        }
    }
}

bool EndsWithPathInsensitive(const char* path, const char* suffix) noexcept
{
    if (path == nullptr || suffix == nullptr)
    {
        return false;
    }
    const auto path_length = std::strlen(path);
    const auto suffix_length = std::strlen(suffix);
    if (path_length < suffix_length)
    {
        return false;
    }
    const auto offset = path_length - suffix_length;
    if (offset != 0 && path[offset - 1] != '/' && path[offset - 1] != '\\')
    {
        return false;
    }
    for (std::size_t index = 0; index < suffix_length; ++index)
    {
        if (NormalizePathCharacter(path[offset + index]) != NormalizePathCharacter(suffix[index]))
        {
            return false;
        }
    }
    return true;
}

bool ResolveRadarOverviewPath(
    const char* requested_path,
    char* replacement,
    const std::size_t replacement_size) noexcept
{
    std::array<char, 1024> requested {};
    if (!CopyCString(requested_path, requested.data(), requested.size()))
    {
        return false;
    }

    std::array<char, 512> target {};
    std::array<char, 512> source {};
    AcquireSRWLockShared(&g_config_lock);
    const bool active = g_config.enabled && g_config.use_source_radar;
    strcpy_s(target.data(), target.size(), g_config.target_overview);
    strcpy_s(source.data(), source.size(), g_config.source_overview);
    ReleaseSRWLockShared(&g_config_lock);
    if (!active || target[0] == '\0' || source[0] == '\0' ||
        !EndsWithPathInsensitive(requested.data(), target.data()))
    {
        return false;
    }

    return strcpy_s(replacement, replacement_size, source.data()) == 0;
}

bool ResolveMapRuntimeDataPath(
    const char* requested_path,
    char* replacement,
    const std::size_t replacement_size) noexcept
{
    std::array<char, 1024> requested {};
    if (!CopyCString(requested_path, requested.data(), requested.size()))
    {
        return false;
    }

    std::array<char, 256> source {};
    std::array<char, 256> target {};
    AcquireSRWLockShared(&g_config_lock);
    const bool active = g_config.enabled;
    strcpy_s(source.data(), source.size(), g_config.source_map);
    strcpy_s(target.data(), target.size(), g_config.target_map);
    ReleaseSRWLockShared(&g_config_lock);
    return active && nskinzcs2::runtime::BuildSourceMapRuntimeDataRedirect(
        requested.data(),
        source.data(),
        target.data(),
        replacement,
        replacement_size);
}

void LogRadarOverviewReplacement(
    const wchar_t* stage,
    const char* requested,
    const char* replacement) noexcept
{
    g_replacement_count.fetch_add(1, std::memory_order_relaxed);
    if (!g_radar_overview_route_logged.exchange(true, std::memory_order_relaxed))
    {
        LogMapReplacement(stage, requested, replacement);
    }
}

void* __fastcall HookedFileSystemOpen(
    void* file_system,
    const char* file_name,
    const char* options,
    const char* path_id) noexcept
{
    nskinzcs2::Logger::DeferredScope deferred_log;
    std::array<char, 512> replacement {};
    if (ResolveMapRuntimeDataPath(file_name, replacement.data(), replacement.size()))
    {
        LogSourceRuntimeDataRedirect(L"RuntimeData/Open", file_name, replacement.data());
        file_name = replacement.data();
    }
    else if (ResolveRadarOverviewPath(file_name, replacement.data(), replacement.size()))
    {
        LogRadarOverviewReplacement(L"RadarTexture/Open", file_name, replacement.data());
        file_name = replacement.data();
    }
    return g_file_system_open_original != nullptr
        ? g_file_system_open_original(file_system, file_name, options, path_id)
        : nullptr;
}

bool __fastcall HookedFileSystemReadFile(
    void* file_system,
    const char* file_name,
    const char* path_id,
    void* buffer,
    const int max_bytes,
    const int starting_byte,
    void* allocator) noexcept
{
    nskinzcs2::Logger::DeferredScope deferred_log;
    std::array<char, 512> replacement {};
    if (ResolveMapRuntimeDataPath(file_name, replacement.data(), replacement.size()))
    {
        LogSourceRuntimeDataRedirect(L"RuntimeData/ReadFile", file_name, replacement.data());
        file_name = replacement.data();
    }
    else if (ResolveRadarOverviewPath(file_name, replacement.data(), replacement.size()))
    {
        LogRadarOverviewReplacement(L"RadarTexture/ReadFile", file_name, replacement.data());
        file_name = replacement.data();
    }
    return g_file_system_read_file_original != nullptr &&
        g_file_system_read_file_original(
            file_system,
            file_name,
            path_id,
            buffer,
            max_bytes,
            starting_byte,
            allocator);
}

bool TryAssignUtlString(void* object, const char* value) noexcept
{
    if (object == nullptr || value == nullptr || g_assign == nullptr)
    {
        return false;
    }
    __try
    {
        g_assign(object, value, static_cast<std::uint64_t>(std::strlen(value)));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool HasBit(void* message, const std::uint32_t bit) noexcept
{
    std::uint32_t bits = 0;
    return TryReadValue(static_cast<std::uint8_t*>(message) + 0x10, &bits, sizeof(bits)) &&
        (bits & bit) != 0;
}

void LogMapReplacement(
    const wchar_t* stage,
    const char* source,
    const char* target) noexcept
{
    try
    {
        nskinzcs2::Logger::Info(
            std::wstring(L"[MapRemap/") + stage + L"] " +
            Utf8ToWide(source != nullptr ? source : "") + L" -> " +
            Utf8ToWide(target != nullptr ? target : ""));
    }
    catch (...)
    {
    }
}

bool ReplaceMatchingMap(
    void* message,
    const std::size_t offset,
    const wchar_t* stage) noexcept
{
    auto* object = ResolveStringObject(message, offset);
    std::array<char, 256> current {};
    if (!CopyUtlString(object, current.data(), current.size()))
    {
        return false;
    }
    if (current[0] == '\0' || std::strcmp(current.data(), "<empty>") == 0)
    {
        return false;
    }

    std::array<char, 256> source {};
    std::array<char, 256> target {};
    AcquireSRWLockShared(&g_config_lock);
    const bool enabled = g_config.enabled;
    strcpy_s(source.data(), source.size(), g_config.source_map);
    strcpy_s(target.data(), target.size(), g_config.target_map);
    ReleaseSRWLockShared(&g_config_lock);

    const bool replaced = enabled && source[0] != '\0' && target[0] != '\0' &&
        std::strcmp(current.data(), source.data()) == 0 &&
        std::strcmp(current.data(), target.data()) != 0 &&
        TryAssignUtlString(object, target.data());
    if (replaced)
    {
        g_replacement_count.fetch_add(1, std::memory_order_relaxed);
        LogMapReplacement(stage, current.data(), target.data());
    }
    return replaced;
}

bool KeepSourceMapForPovRadar(
    void* message,
    const std::size_t offset) noexcept
{
    auto* object = ResolveStringObject(message, offset);
    std::array<char, 256> current {};
    if (!CopyUtlString(object, current.data(), current.size()))
    {
        return false;
    }

    std::array<char, 256> source {};
    std::array<char, 256> target {};
    AcquireSRWLockShared(&g_config_lock);
    const bool active = g_config.enabled && g_config.use_source_radar;
    strcpy_s(source.data(), source.size(), g_config.source_map);
    strcpy_s(target.data(), target.size(), g_config.target_map);
    ReleaseSRWLockShared(&g_config_lock);
    if (!active || source[0] == '\0')
    {
        return false;
    }

    // CMsgGameSessionConfiguration::s1_mapname is consumed by the UI-side
    // game-session state. Keep it on the demo map while DemoHeader,
    // ServerInfo, SpawnGroup and map KeyValues continue to load the workshop
    // world. This makes mirv_pov use the original overview coordinates and
    // radar texture without touching either HLAE or workshop files.
    if (target[0] != '\0' && std::strcmp(current.data(), target.data()) == 0 &&
        TryAssignUtlString(object, source.data()))
    {
        g_replacement_count.fetch_add(1, std::memory_order_relaxed);
        LogMapReplacement(L"GameSession/radar_source", current.data(), source.data());
    }
    else if (!g_source_radar_route_logged.exchange(true, std::memory_order_relaxed))
    {
        try
        {
            nskinzcs2::Logger::Info(
                L"[MapRemap/GameSession/radar_source] preserving original demo map " +
                Utf8ToWide(source.data()) + L" for POV radar.");
        }
        catch (...)
        {
        }
    }
    return true;
}

bool TrySetKeyValue(void* key_values, const char* key, const char* value) noexcept
{
    if (key_values == nullptr || key == nullptr || value == nullptr || g_set_keyvalues_string == nullptr)
    {
        return false;
    }
    __try
    {
        g_set_keyvalues_string(key_values, key, value);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool TryFindResource(void* resource_system, const std::uint64_t hash, bool& found) noexcept
{
    found = false;
    if (resource_system == nullptr || g_find_resource_by_hash == nullptr)
    {
        return false;
    }
    __try
    {
        found = g_find_resource_by_hash(
            static_cast<std::uint8_t*>(resource_system) + 0x2BE0,
            hash) != nullptr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void* __fastcall HookedRequestResource(
    void* resource_system,
    const std::uint64_t hash,
    const std::uint64_t type) noexcept
{
    nskinzcs2::Logger::DeferredScope deferred_log;

    // Resource type is the little-endian FourCC "vmdl".  Only suppress a
    // request when the resource-system lookup safely confirms it is absent.
    if (type == 0x6C646D76ull && hash != 0)
    {
        bool found = false;
        if (TryFindResource(resource_system, hash, found) && !found)
        {
            g_replacement_count.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }
    return g_request_resource_original != nullptr
        ? g_request_resource_original(resource_system, hash, type)
        : nullptr;
}

extern "C" void* __fastcall MapRemapHookCallback(
    const std::uint32_t raw_kind,
    void* message,
    void* parse_result,
    void* auxiliary) noexcept
{
    nskinzcs2::Logger::DeferredScope deferred_log;
    const auto kind = static_cast<MessageKind>(raw_kind);
    if (kind == MessageKind::SpawnGroup && parse_result == nullptr)
    {
        return nullptr;
    }

    switch (kind)
    {
    case MessageKind::DemoHeader:
    {
        AcquireSRWLockExclusive(&g_config_lock);
        g_config.current_header_matched = false;
        ReleaseSRWLockExclusive(&g_config_lock);
        if (message != nullptr && HasBit(message, 0x8))
        {
            auto* map_object = ResolveStringObject(message, 0x30);
            std::array<char, 256> current {};
            if (CopyUtlString(map_object, current.data(), current.size()))
            {
                AcquireSRWLockExclusive(&g_config_lock);
                if (g_config.enabled && g_config.source_map[0] == '\0' && current[0] != '\0' &&
                    std::strcmp(current.data(), "<empty>") != 0)
                {
                    strcpy_s(g_config.source_map, current.data());
                    g_config.source_captured = true;
                }
                const bool matches = g_config.enabled && g_config.source_map[0] != '\0' &&
                    std::strcmp(current.data(), "<empty>") != 0 &&
                    std::strcmp(current.data(), g_config.source_map) == 0;
                g_config.current_header_matched = matches;
                std::array<char, 256> target {};
                strcpy_s(target.data(), target.size(), g_config.target_map);
                ReleaseSRWLockExclusive(&g_config_lock);
                const bool changed = matches && target[0] != '\0' &&
                    std::strcmp(current.data(), target.data()) != 0 &&
                    TryAssignUtlString(map_object, target.data());
                if (changed)
                {
                    g_replacement_count.fetch_add(1, std::memory_order_relaxed);
                    LogMapReplacement(L"DemoHeader/map_name", current.data(), target.data());
                }
            }
        }
        if (message != nullptr && HasBit(message, 0x20))
        {
            auto* addon_object = ResolveStringObject(message, 0x40);
            std::array<char, 64> current_addon {};
            const bool current_addon_valid = CopyUtlString(
                addon_object,
                current_addon.data(),
                current_addon.size());
            AcquireSRWLockShared(&g_config_lock);
            const bool enabled = g_config.enabled;
            std::array<char, 64> workshop_id {};
            strcpy_s(workshop_id.data(), workshop_id.size(), g_config.workshop_id);
            ReleaseSRWLockShared(&g_config_lock);
            if (enabled && workshop_id[0] != '\0' &&
                (!current_addon_valid || std::strcmp(current_addon.data(), workshop_id.data()) != 0) &&
                TryAssignUtlString(addon_object, workshop_id.data()))
            {
                g_replacement_count.fetch_add(1, std::memory_order_relaxed);
                LogMapReplacement(
                    L"DemoHeader/addons",
                    current_addon_valid ? current_addon.data() : "<unreadable>",
                    workshop_id.data());
            }
        }
        break;
    }
    case MessageKind::SpawnGroup:
        if (message != nullptr && HasBit(message, 0x1))
        {
            ReplaceMatchingMap(message, 0x18, L"SpawnGroup/name");
        }
        break;
    case MessageKind::ClearStringTables:
        if (message != nullptr)
        {
            ReplaceMatchingMap(message, 0x18, L"ClearAllStringTables/mapname");
        }
        break;
    case MessageKind::ServerInfo:
        if (message != nullptr)
        {
            ReplaceMatchingMap(message, 0x20, L"ServerInfo/map_name");
        }
        break;
    case MessageKind::GameSession:
        if (message != nullptr && HasBit(message, 0x4))
        {
            // CSVCMsg_GameSessionConfiguration field 11 is s1_mapname.  In
            // build 2000918 the first four scalar fields moved the string
            // slots: bit 2 still identifies the field, but its CUtlString is
            // now at +0x40 instead of +0x28.
            ReplaceMatchingMap(message, 0x40, L"GameSession/s1_mapname");
        }
        break;
    case MessageKind::MapKeyValues:
    {
        std::array<char, 256> target {};
        AcquireSRWLockShared(&g_config_lock);
        const bool active = g_config.enabled;
        strcpy_s(target.data(), target.size(), g_config.target_map);
        ReleaseSRWLockShared(&g_config_lock);
        if (active && target[0] != '\0')
        {
            bool changed = false;
            if (TrySetKeyValue(auxiliary, "map", target.data()))
            {
                g_replacement_count.fetch_add(1, std::memory_order_relaxed);
                changed = true;
            }
            if (TrySetKeyValue(auxiliary, "levelname", target.data()))
            {
                g_replacement_count.fetch_add(1, std::memory_order_relaxed);
                changed = true;
            }
            if (changed)
            {
                LogMapReplacement(L"KeyValues/map+levelname", "<root>", target.data());
            }
        }
        break;
    }
    case MessageKind::FailedResourceLog:
    {
        std::array<char, 1024> path {};
        if (CopyCString(static_cast<const char*>(parse_result), path.data(), path.size()))
        {
            try
            {
                std::string normalized(path.data());
                constexpr std::string_view suffix = ".vmdl_c";
                if (normalized.size() > suffix.size() &&
                    normalized.compare(normalized.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    normalized.resize(normalized.size() - 2);
                    AcquireSRWLockExclusive(&g_failed_resource_lock);
                    try
                    {
                        g_failed_model_paths.insert(std::move(normalized));
                    }
                    catch (...)
                    {
                        ReleaseSRWLockExclusive(&g_failed_resource_lock);
                        throw;
                    }
                    ReleaseSRWLockExclusive(&g_failed_resource_lock);
                }
            }
            catch (...)
            {
            }
        }
        break;
    }
    case MessageKind::ManifestPath:
    {
        std::array<char, 1024> path {};
        if (CopyCString(static_cast<const char*>(auxiliary), path.data(), path.size()))
        {
            std::array<char, 256> source {};
            std::array<char, 256> target {};
            std::array<char, 512> target_skybox {};
            AcquireSRWLockShared(&g_config_lock);
            const bool active = g_config.enabled;
            strcpy_s(source.data(), source.size(), g_config.source_map);
            strcpy_s(target.data(), target.size(), g_config.target_map);
            strcpy_s(
                target_skybox.data(),
                target_skybox.size(),
                g_config.target_skybox_resource_prefix);
            ReleaseSRWLockShared(&g_config_lock);
            thread_local std::array<char, 1024> skybox_redirect {};
            skybox_redirect.fill('\0');
            if (active && nskinzcs2::runtime::BuildSourceMapRuntimeDataRedirect(
                    path.data(),
                    source.data(),
                    target.data(),
                    skybox_redirect.data(),
                    skybox_redirect.size()))
            {
                LogSourceRuntimeDataRedirect(
                    L"RuntimeData/Manifest",
                    path.data(),
                    skybox_redirect.data());
                return skybox_redirect.data();
            }
            if (active && nskinzcs2::runtime::BuildSourceSkyboxResourceRedirect(
                    path.data(),
                    source.data(),
                    target.data(),
                    target_skybox.data(),
                    skybox_redirect.data(),
                    skybox_redirect.size()))
            {
                LogSourceSkyboxRedirect(
                    L"SourceSkybox/Manifest",
                    path.data(),
                    skybox_redirect.data());
                return skybox_redirect.data();
            }
            if (active && nskinzcs2::runtime::ShouldSuppressSourceMapModelPath(
                    path.data(),
                    source.data(),
                    target.data(),
                    target_skybox.data()))
            {
                LogSourceGeometrySuppression(
                    L"SourceGeometry/Manifest",
                    path.data(),
                    kSafeManifestModel);
                return const_cast<char*>(kSafeManifestModel);
            }

            bool failed = false;
            AcquireSRWLockShared(&g_failed_resource_lock);
            try
            {
                failed = g_failed_model_paths.find(path.data()) != g_failed_model_paths.end();
            }
            catch (...)
            {
            }
            ReleaseSRWLockShared(&g_failed_resource_lock);
            if (failed)
            {
                g_replacement_count.fetch_add(1, std::memory_order_relaxed);
                LogMapReplacement(L"Manifest/path", path.data(), kSafeManifestModel);
                return const_cast<char*>(kSafeManifestModel);
            }
        }
        break;
    }
    }
    return nullptr;
}

std::vector<std::uint8_t> BuildStub(const MessageKind kind, const void* trampoline)
{
    std::vector<std::uint8_t> code;
    code.reserve(192);
    AppendBytes(code, {0x9C, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
    AppendBytes(code, {0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00});
    AppendBytes(code, {0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20});
    AppendBytes(code, {0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30});
    AppendBytes(code, {0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40});
    AppendBytes(code, {0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50});
    AppendBytes(code, {0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60});
    AppendBytes(code, {0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70});
    code.push_back(0xB9);
    AppendValue(code, static_cast<std::uint32_t>(kind));
    AppendBytes(code, {0x48, 0x8B, 0xD6});
    if (kind == MessageKind::MapKeyValues)
    {
        AppendBytes(code, {0x49, 0x89, 0xE9});
    }
    else if (kind == MessageKind::ManifestPath)
    {
        AppendBytes(code, {0x4D, 0x89, 0xC1});
    }
    else
    {
        AppendBytes(code, {0x45, 0x33, 0xC9});
    }
    AppendBytes(code, {0x49, 0x89, 0xC0});
    AppendBytes(code, {0x48, 0xB8});
    AppendValue(code, reinterpret_cast<std::uintptr_t>(&MapRemapHookCallback));
    AppendBytes(code, {0xFF, 0xD0});
    if (kind == MessageKind::ManifestPath)
    {
        AppendBytes(code, {0x48, 0x85, 0xC0, 0x74, 0x08});
        AppendBytes(code, {0x48, 0x89, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00});
    }
    AppendBytes(code, {0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20});
    AppendBytes(code, {0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30});
    AppendBytes(code, {0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40});
    AppendBytes(code, {0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50});
    AppendBytes(code, {0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60});
    AppendBytes(code, {0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70});
    AppendBytes(code, {0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00});
    AppendBytes(code, {0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58, 0x9D});
    // MinHook's trampoline executes the displaced MOV/TEST pair and returns
    // to the original conditional branch without changing any live register.
    AppendBytes(code, {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    AppendValue(code, reinterpret_cast<std::uintptr_t>(trampoline));
    return code;
}

bool InstallInlineHook(InlineHook& hook, void* target, const MessageKind kind)
{
    if (target == nullptr)
    {
        return false;
    }

    auto code = BuildStub(kind, nullptr);
    void* stub = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (stub == nullptr)
    {
        return false;
    }
    std::memcpy(stub, code.data(), code.size());

    // MinHook rejects a detour that is not executable.  Publish the bridge as
    // RX before MH_CreateHook, then briefly make it writable only to install
    // the trampoline address returned by MinHook.  The hook is still disabled
    // during this whole sequence.
    DWORD initial_protect = 0;
    if (!VirtualProtect(stub, code.size(), PAGE_EXECUTE_READ, &initial_protect))
    {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());

    void* trampoline = nullptr;
    const auto create_status = MH_CreateHook(target, stub, &trampoline);
    if (create_status != MH_OK || trampoline == nullptr)
    {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }

    const auto trampoline_offset = code.size() - sizeof(std::uintptr_t);
    DWORD writable_protect = 0;
    if (!VirtualProtect(stub, code.size(), PAGE_READWRITE, &writable_protect))
    {
        MH_RemoveHook(target);
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(
        static_cast<std::uint8_t*>(stub) + trampoline_offset,
        &trampoline,
        sizeof(trampoline));
    DWORD final_protect = 0;
    if (!VirtualProtect(stub, code.size(), PAGE_EXECUTE_READ, &final_protect))
    {
        MH_RemoveHook(target);
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, code.size());

    hook.target = target;
    hook.stub = stub;
    hook.trampoline = trampoline;
    hook.installed = true;
    return true;
}

void RemoveInlineHook(InlineHook& hook)
{
    if (hook.installed && hook.target != nullptr)
    {
        MH_DisableHook(hook.target);
        MH_RemoveHook(hook.target);
    }
    if (hook.stub != nullptr)
    {
        VirtualFree(hook.stub, 0, MEM_RELEASE);
    }
    hook = {};
}

bool IsExecutableAddress(const void* address) noexcept
{
    if (address == nullptr)
    {
        return false;
    }
    MEMORY_BASIC_INFORMATION information {};
    if (VirtualQuery(address, &information, sizeof(information)) != sizeof(information) ||
        information.State != MEM_COMMIT || (information.Protect & PAGE_GUARD) != 0)
    {
        return false;
    }
    const DWORD protection = information.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool ResolveFileSystemHookTargets(void*& open_target, void*& read_file_target) noexcept
{
    open_target = nullptr;
    read_file_target = nullptr;
    const auto module = GetModuleHandleW(L"filesystem_stdio.dll");
    if (module == nullptr)
    {
        return false;
    }
    const auto factory = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
    if (factory == nullptr)
    {
        return false;
    }

    void* file_system = nullptr;
    __try
    {
        file_system = factory("VFileSystem017", nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    void** vtable = nullptr;
    void* add_search_path_target = nullptr;
    void* remove_search_path_target = nullptr;
    if (file_system == nullptr || !TryReadValue(file_system, &vtable, sizeof(vtable)) || vtable == nullptr ||
        !TryReadValue(vtable + kFileSystemOpenSlot, &open_target, sizeof(open_target)) ||
        !TryReadValue(vtable + kFileSystemReadFileSlot, &read_file_target, sizeof(read_file_target)) ||
        !TryReadValue(
            vtable + kFileSystemAddSearchPathSlot,
            &add_search_path_target,
            sizeof(add_search_path_target)) ||
        !TryReadValue(
            vtable + kFileSystemRemoveSearchPathSlot,
            &remove_search_path_target,
            sizeof(remove_search_path_target)))
    {
        open_target = nullptr;
        read_file_target = nullptr;
        return false;
    }
    if (open_target == read_file_target || !IsExecutableAddress(open_target) ||
        !IsExecutableAddress(read_file_target) || !IsExecutableAddress(add_search_path_target) ||
        !IsExecutableAddress(remove_search_path_target))
    {
        open_target = nullptr;
        read_file_target = nullptr;
        return false;
    }
    g_file_system_interface = file_system;
    g_file_system_add_search_path = reinterpret_cast<FileSystemAddSearchPathFn>(add_search_path_target);
    g_file_system_remove_search_path = reinterpret_cast<FileSystemRemoveSearchPathFn>(remove_search_path_target);
    return true;
}

void UnmountRadarOverlay() noexcept
{
    if (g_radar_overlay_mounted && g_file_system_interface != nullptr &&
        g_file_system_remove_search_path != nullptr && g_radar_overlay_search_path[0] != '\0')
    {
        __try
        {
            g_file_system_remove_search_path(
                g_file_system_interface,
                g_radar_overlay_search_path.data(),
                "GAME");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    g_radar_overlay_mounted = false;
    g_radar_overlay_search_path.fill('\0');
}

bool TryAddRadarSearchPath(const char* path) noexcept
{
    if (path == nullptr || path[0] == '\0' || g_file_system_interface == nullptr ||
        g_file_system_add_search_path == nullptr)
    {
        return false;
    }
    __try
    {
        g_file_system_add_search_path(
            g_file_system_interface,
            path,
            "GAME",
            0,
            1,
            0);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsSafeRadarVirtualPath(const std::string_view path) noexcept
{
    constexpr std::string_view prefix = "panorama/images/overheadmaps/";
    constexpr std::string_view suffix = "_radar_tga.vtex_c";
    if (path.size() <= prefix.size() + suffix.size() || path.size() >= sizeof(g_config.target_overview) ||
        path.substr(0, prefix.size()) != prefix || path.substr(path.size() - suffix.size()) != suffix ||
        path.find("..") != std::string_view::npos || path.find(':') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos)
    {
        return false;
    }
    return std::all_of(path.begin(), path.end(), [](const unsigned char character)
    {
        return std::isalnum(character) != 0 || character == '/' || character == '_' ||
            character == '-' || character == '.';
    });
}

bool MountPreparedRadarOverlay(
    const nskinzcs2::model::MapRemapProfile& profile,
    std::wstring& diagnostic)
{
    if (!profile.enabled || !profile.use_source_radar)
    {
        return true;
    }
    if (profile.radar_overlay_virtual_path.empty() &&
        profile.radar_overlay_root_path.empty() &&
        profile.radar_overlay_file_path.empty())
    {
        nskinzcs2::Logger::Info(
            L"[MapRemap/RadarTexture] no external alpha overlay was published; the target radar resource will remain unchanged.");
        return true;
    }
    if (profile.radar_overlay_virtual_path.empty() ||
        profile.radar_overlay_root_path.empty() ||
        profile.radar_overlay_file_path.empty())
    {
        diagnostic = L"The externally prepared radar overlay paths are incomplete.";
        return false;
    }
    if (g_file_system_interface == nullptr || g_file_system_add_search_path == nullptr)
    {
        diagnostic = L"VFileSystem017 is unavailable, so the externally prepared radar overlay cannot be mounted.";
        return false;
    }
    if (!IsSafeRadarVirtualPath(profile.radar_overlay_virtual_path))
    {
        diagnostic = L"The externally prepared radar virtual path was rejected.";
        return false;
    }

    const auto root_wide = Utf8ToWide(profile.radar_overlay_root_path);
    const auto file_wide = Utf8ToWide(profile.radar_overlay_file_path);
    if (root_wide.empty() || file_wide.empty())
    {
        diagnostic = L"The externally prepared radar paths are not valid UTF-8.";
        return false;
    }
    const auto root = std::filesystem::path(root_wide).lexically_normal();
    const auto file = std::filesystem::path(file_wide).lexically_normal();
    const auto expected =
        (root / std::filesystem::path(profile.radar_overlay_virtual_path)).lexically_normal();
    if (!root.is_absolute() || !file.is_absolute() ||
        _wcsicmp(expected.c_str(), file.c_str()) != 0)
    {
        diagnostic = L"The externally prepared radar file is outside its declared overlay root.";
        return false;
    }
    if (!nskinzcs2::runtime::ValidateNativeRadarVtexFile(file, diagnostic))
    {
        diagnostic = L"The externally prepared radar VTEX failed native resource validation. " + diagnostic;
        return false;
    }
    const auto overlay_root_utf8 = WideToUtf8(root.wstring());
    const auto overlay_file_utf8 = WideToUtf8(file.wstring());
    if (overlay_root_utf8.empty() || overlay_root_utf8.size() >= g_radar_overlay_search_path.size() ||
        overlay_file_utf8.empty() || overlay_file_utf8.size() >= sizeof(g_config.source_overview) ||
        profile.radar_overlay_virtual_path.size() >= sizeof(g_config.target_overview))
    {
        diagnostic = L"The externally prepared radar overlay path exceeds an internal path limit.";
        return false;
    }

    if (!TryAddRadarSearchPath(overlay_root_utf8.c_str()))
    {
        diagnostic = L"VFileSystem017 rejected the externally prepared radar overlay search path.";
        return false;
    }
    strcpy_s(g_radar_overlay_search_path.data(), g_radar_overlay_search_path.size(), overlay_root_utf8.c_str());
    g_radar_overlay_mounted = true;
    AcquireSRWLockExclusive(&g_config_lock);
    strcpy_s(g_config.source_overview, overlay_file_utf8.c_str());
    strcpy_s(g_config.target_overview, profile.radar_overlay_virtual_path.c_str());
    ReleaseSRWLockExclusive(&g_config_lock);

    nskinzcs2::Logger::Info(
        L"[MapRemap/RadarTexture] mounted the externally prepared CS2-native radar overlay; no texture decoding or resource compilation ran inside cs2.exe.");
    diagnostic.clear();
    return true;
}

void RollbackHooks()
{
    for (auto& hook : g_hooks)
    {
        RemoveInlineHook(hook);
    }
    RemoveInlineHook(g_resource_request_hook);
    RemoveInlineHook(g_file_system_open_hook);
    RemoveInlineHook(g_file_system_read_file_hook);
    g_assign = nullptr;
    g_set_keyvalues_string = nullptr;
    g_request_resource_original = nullptr;
    g_find_resource_by_hash = nullptr;
    g_file_system_open_original = nullptr;
    g_file_system_read_file_original = nullptr;
    g_file_system_interface = nullptr;
    g_file_system_add_search_path = nullptr;
    g_file_system_remove_search_path = nullptr;
    g_hooks_installed.store(false, std::memory_order_release);
}

bool EnsureHooksInstalled()
{
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_hooks_installed.load(std::memory_order_acquire))
    {
        return true;
    }

    const auto engine_module = GetModuleHandleW(L"engine2.dll");
    const auto client_module = GetModuleHandleW(L"client.dll");
    const auto resource_module = GetModuleHandleW(L"resourcesystem.dll");
    const auto tier0_module = GetModuleHandleW(L"tier0.dll");
    const auto engine_text = GetTextRegion(engine_module);
    const auto client_text = GetTextRegion(client_module);
    const auto resource_text = GetTextRegion(resource_module);
    if (!engine_text.has_value() || !client_text.has_value() || !resource_text.has_value() || tier0_module == nullptr)
    {
        SetIssue(L"A required CS2 module (.text for engine2/client/resourcesystem or tier0) is unavailable.");
        return false;
    }

    const auto header = FindMatches(*engine_text, kDemoHeaderSignature);
    const auto spawn = FindMatches(*engine_text, kSpawnGroupSignature);
    const auto clear = FindMatches(*engine_text, kClearTablesSignature);
    const auto server = FindMatches(*engine_text, kServerInfoSignature);
    // MulNX upstream f2466e18 (2026-09-27) anchors the shared string-parser
    // exit to the following field-10 branch instead of hard-coding relative
    // jump displacements.  The displacements change whenever Valve adds a
    // protobuf field, while this context still uniquely identifies the
    // CSVCMsg_GameSessionConfiguration parser in build 2000918.
    const auto session = FindMaskedMatches(*engine_text, {
        0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F, 0x85,
        -1, -1, -1, -1, 0xE9, -1, -1, -1, -1,
        0x40, 0x80, 0xFF, 0x52, 0x0F, 0x85, -1, -1, -1, -1, 0x83, 0x4E,
    });
    const auto assign = FindMatches(*engine_text, kUtlStringAssignSignature);
    const auto map_key_values = FindMaskedMatches(*client_text, {
        0x89, 0x5C, -1, -1, 0xA9, 0xFF, 0xFF, 0xFF, 0x7F, 0x76, 0x1C, 0xC1, 0xE8, 0x1F,
        0x84, 0xC0, 0x75, 0x15, 0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x48, 0x8B, 0x54, -1, -1,
        0x48, 0x8B, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0x50, -1, 0x48, 0x85, 0xED, 0x48, 0x8B, 0x6C,
    });
    const auto failed_resource = FindMaskedMatches(*resource_text, {
        0x8B, 0x0D, -1, -1, -1, -1, 0x4C, 0x8D, 0x0D, -1, -1, -1, -1, 0x41, 0xB8, 0xFF,
        0x00, 0x00, 0xFF, 0x48, 0x89, 0x5C, -1, -1, 0xBA, 0x03, 0x00, 0x00, 0x00, 0x48,
        0x89, 0x44, -1, -1, 0xFF, 0x15,
    });
    const auto manifest_path = FindMaskedMatches(*engine_text, {
        0xFF, 0x15, -1, -1, -1, -1, 0x0F, 0xB7, 0x85, -1, -1, -1, -1, 0xB9, 0xFF, 0xFF,
        0x00, 0x00, 0x66, 0x3B, 0xC1, 0x74, 0x32, 0x4C, 0x8B, 0xB5,
    });
    const auto request_resource = FindMatches(*resource_text, kRequestResourceSignature);
    const auto find_resource = FindMatches(*resource_text, kFindResourceSignature);
    const auto set_keyvalues_string = reinterpret_cast<KeyValuesSetStringFn>(GetProcAddress(
        tier0_module,
        "?SetString@KeyValues@@QEAAXPEBD0@Z"));
    if (header.size() != 1 || spawn.size() != 1 || clear.size() != 1 ||
        server.size() != 1 || session.size() != 1 || assign.size() != 1 ||
        map_key_values.size() != 1 || failed_resource.size() != 1 || manifest_path.size() != 1 ||
        request_resource.size() != 1 || find_resource.size() != 1 || set_keyvalues_string == nullptr)
    {
        std::wstringstream stream;
        stream << L"Map remap signatures are incomplete or not unique for this CS2 build: header=" << header.size()
               << L", spawn=" << spawn.size() << L", clear=" << clear.size()
               << L", server=" << server.size() << L", session=" << session.size()
               << L", assign=" << assign.size() << L", mapkv=" << map_key_values.size()
               << L", failed=" << failed_resource.size() << L", manifest=" << manifest_path.size()
               << L", request=" << request_resource.size() << L", find=" << find_resource.size()
               << L", kvexport=" << (set_keyvalues_string != nullptr ? 1 : 0)
               << L". Hooks were not installed.";
        SetIssue(stream.str());
        nskinzcs2::Logger::Warn(stream.str());
        return false;
    }

    const auto initialize_status = MH_Initialize();
    if (initialize_status != MH_OK && initialize_status != MH_ERROR_ALREADY_INITIALIZED)
    {
        SetIssue(L"MinHook initialization failed for map remap.");
        nskinzcs2::Logger::Warn(L"Map remap could not initialize MinHook.");
        return false;
    }

    void* file_system_open_target = nullptr;
    void* file_system_read_file_target = nullptr;
    if (!ResolveFileSystemHookTargets(file_system_open_target, file_system_read_file_target))
    {
        SetIssue(L"VFileSystem017 Open/ReadFile/search-path slots could not be resolved for the radar texture route.");
        nskinzcs2::Logger::Warn(L"Map remap radar texture hooks could not resolve VFileSystem017 slots 13, 25, 31, and 32.");
        return false;
    }

    g_assign = reinterpret_cast<UtlStringAssignFn>(assign.front());
    g_set_keyvalues_string = set_keyvalues_string;
    g_find_resource_by_hash = reinterpret_cast<FindResourceByHashFn>(find_resource.front());
    const std::array<void*, 8> targets {
        header.front(),
        spawn.front(),
        clear.front(),
        server.front(),
        session.front(),
        map_key_values.front(),
        failed_resource.front(),
        manifest_path.front(),
    };
    const std::array<MessageKind, 8> kinds {
        MessageKind::DemoHeader,
        MessageKind::SpawnGroup,
        MessageKind::ClearStringTables,
        MessageKind::ServerInfo,
        MessageKind::GameSession,
        MessageKind::MapKeyValues,
        MessageKind::FailedResourceLog,
        MessageKind::ManifestPath,
    };
    for (std::size_t index = 0; index < g_hooks.size(); ++index)
    {
        if (!InstallInlineHook(g_hooks[index], targets[index], kinds[index]))
        {
            RollbackHooks();
            SetIssue(L"A verified map remap bridge could not be installed; all partial hooks were rolled back.");
            nskinzcs2::Logger::Warn(L"Map remap hook install failed and was rolled back.");
            return false;
        }
    }

    void* request_original = nullptr;
    if (MH_CreateHook(
            request_resource.front(),
            reinterpret_cast<void*>(&HookedRequestResource),
            &request_original) != MH_OK || request_original == nullptr)
    {
        RollbackHooks();
        SetIssue(L"The verified resource request hook could not be installed; all partial hooks were rolled back.");
        nskinzcs2::Logger::Warn(L"Map remap resource hook install failed and was rolled back.");
        return false;
    }
    g_request_resource_original = reinterpret_cast<RequestResourceByHashFn>(request_original);
    g_resource_request_hook.target = request_resource.front();
    g_resource_request_hook.trampoline = request_original;
    g_resource_request_hook.installed = true;

    void* file_system_open_original = nullptr;
    if (MH_CreateHook(
            file_system_open_target,
            reinterpret_cast<void*>(&HookedFileSystemOpen),
            &file_system_open_original) != MH_OK || file_system_open_original == nullptr)
    {
        RollbackHooks();
        SetIssue(L"The VFileSystem017 Open hook for radar overview replacement could not be installed.");
        nskinzcs2::Logger::Warn(L"Map remap radar Open hook install failed and was rolled back.");
        return false;
    }
    g_file_system_open_original = reinterpret_cast<FileSystemOpenFn>(file_system_open_original);
    g_file_system_open_hook.target = file_system_open_target;
    g_file_system_open_hook.trampoline = file_system_open_original;
    g_file_system_open_hook.installed = true;

    void* file_system_read_file_original = nullptr;
    if (MH_CreateHook(
            file_system_read_file_target,
            reinterpret_cast<void*>(&HookedFileSystemReadFile),
            &file_system_read_file_original) != MH_OK || file_system_read_file_original == nullptr)
    {
        RollbackHooks();
        SetIssue(L"The VFileSystem017 ReadFile hook for radar overview replacement could not be installed.");
        nskinzcs2::Logger::Warn(L"Map remap radar ReadFile hook install failed and was rolled back.");
        return false;
    }
    g_file_system_read_file_original = reinterpret_cast<FileSystemReadFileFn>(file_system_read_file_original);
    g_file_system_read_file_hook.target = file_system_read_file_target;
    g_file_system_read_file_hook.trampoline = file_system_read_file_original;
    g_file_system_read_file_hook.installed = true;

    for (const auto& hook : g_hooks)
    {
        if (MH_QueueEnableHook(hook.target) != MH_OK)
        {
            RollbackHooks();
            SetIssue(L"A verified map remap hook could not be queued; all hooks were rolled back.");
            nskinzcs2::Logger::Warn(L"Map remap hook queue failed and was rolled back.");
            return false;
        }
    }
    if (MH_QueueEnableHook(g_resource_request_hook.target) != MH_OK)
    {
        RollbackHooks();
        SetIssue(L"The verified resource request hook could not be queued; all hooks were rolled back.");
        nskinzcs2::Logger::Warn(L"Map remap resource hook queue failed and was rolled back.");
        return false;
    }
    if (MH_QueueEnableHook(g_file_system_open_hook.target) != MH_OK ||
        MH_QueueEnableHook(g_file_system_read_file_hook.target) != MH_OK)
    {
        RollbackHooks();
        SetIssue(L"The radar overview filesystem hooks could not be queued; all hooks were rolled back.");
        nskinzcs2::Logger::Warn(L"Map remap radar filesystem hook queue failed and was rolled back.");
        return false;
    }
    if (MH_ApplyQueued() != MH_OK)
    {
        RollbackHooks();
        SetIssue(L"Verified map remap hooks could not be enabled atomically; all hooks were rolled back.");
        nskinzcs2::Logger::Warn(L"Map remap hook activation failed and was rolled back.");
        return false;
    }

    g_hooks_installed.store(true, std::memory_order_release);
    SetIssue({});
    nskinzcs2::Logger::Info(
        L"Map remap ready: the verified engine2 protobuf, client KeyValues, manifest, resource fallback, and VFileSystem radar texture chain is installed.");
    return true;
}
}

bool nskinzcs2::runtime::BuildSourceSkyboxResourceRedirect(
    const std::string_view resource_path,
    const std::string_view source_map_name,
    const std::string_view target_map_name,
    const std::string_view target_skybox_resource_prefix,
    char* replacement,
    const std::size_t replacement_size) noexcept
{
    if (replacement == nullptr || replacement_size == 0)
    {
        return false;
    }
    replacement[0] = '\0';
    try
    {
        const auto source = NormalizeMapLeaf(source_map_name);
        const auto target = NormalizeMapLeaf(target_map_name);
        const auto path = NormalizeResourcePath(resource_path);
        if (source.empty() || target.empty() || source == target || path.empty() ||
            !TargetUsesDifferentKnownSkybox(source_map_name, target_skybox_resource_prefix))
        {
            return false;
        }

        const auto source_prefab = "maps/prefabs/" + source + "/";
        const auto source_offset = path.find(source_prefab);
        if (source_offset == std::string::npos)
        {
            return false;
        }
        const auto tail = path.substr(source_offset + source_prefab.size());
        if (tail.empty() || tail.find('/') != std::string::npos ||
            (tail.find("skybox") == std::string::npos &&
             tail.find("3dsky") == std::string::npos &&
             tail.find("3d_sky") == std::string::npos))
        {
            return false;
        }

        std::string_view suffix;
        if (tail.ends_with(".vmap_c"))
        {
            suffix = ".vmap_c";
        }
        else if (tail.ends_with(".vmap"))
        {
            suffix = ".vmap";
        }
        else
        {
            return false;
        }

        auto target_prefix = NormalizeResourcePath(target_skybox_resource_prefix);
        for (const std::string_view extension : {".vmap_c", ".vmap", ".vpk"})
        {
            if (target_prefix.ends_with(extension))
            {
                target_prefix.resize(target_prefix.size() - extension.size());
                break;
            }
        }
        if (target_prefix.empty())
        {
            return false;
        }
        const auto redirect = target_prefix + std::string(suffix);
        return redirect.size() < replacement_size &&
            strcpy_s(replacement, replacement_size, redirect.c_str()) == 0;
    }
    catch (...)
    {
        replacement[0] = '\0';
        return false;
    }
}

bool nskinzcs2::runtime::BuildSourceMapRuntimeDataRedirect(
    const std::string_view resource_path,
    const std::string_view source_map_name,
    const std::string_view target_map_name,
    char* replacement,
    const std::size_t replacement_size) noexcept
{
    if (replacement == nullptr || replacement_size == 0)
    {
        return false;
    }
    replacement[0] = '\0';
    try
    {
        const auto source = NormalizeMapLeaf(source_map_name);
        const auto target = NormalizeMapLeaf(target_map_name);
        const auto path = NormalizeResourcePath(resource_path);
        if (source.empty() || target.empty() || source == target || path.empty())
        {
            return false;
        }

        const auto source_root = "maps/" + source + "/";
        const auto target_root = "maps/" + target + "/";
        const bool use_compatible_source_probe_atlas =
            source == "de_nuke" && target == "de_nuke_rainy";
        if (use_compatible_source_probe_atlas)
        {
            const auto target_offset = path.find(target_root);
            if (target_offset != std::string::npos)
            {
                const auto target_tail = path.substr(target_offset + target_root.size());
                const bool target_light_probe_atlas =
                    target_tail == "lightmaps/env_light_probe_volume_atlas.vtex" ||
                    target_tail == "lightmaps/env_light_probe_volume_atlas.vtex_c" ||
                    target_tail == "lightmaps/env_light_probe_volume_atlas_dlshd.vtex" ||
                    target_tail == "lightmaps/env_light_probe_volume_atlas_dlshd.vtex_c";
                if (target_light_probe_atlas)
                {
                    // Rainy Nuke was verified to retain stock Nuke's complete
                    // probe-volume layout and atlas coordinates. Its baked
                    // target atlas leaves demo-driven player models too dark,
                    // so keep the Workshop world/weather but supply stock
                    // Nuke's compatible dynamic-model probe samples.
                    const auto redirect = source_root + target_tail;
                    return redirect.size() < replacement_size &&
                        strcpy_s(replacement, replacement_size, redirect.c_str()) == 0;
                }
            }
        }
        const auto source_offset = path.find(source_root);
        if (source_offset == std::string::npos)
        {
            return false;
        }
        const auto tail = path.substr(source_offset + source_root.size());
        const bool default_entity_lump =
            tail == "entities/default_ents.vents" ||
            tail == "entities/default_ents.vents_c";
        const bool light_probe_atlas =
            tail == "lightmaps/env_light_probe_volume_atlas.vtex" ||
            tail == "lightmaps/env_light_probe_volume_atlas.vtex_c" ||
            tail == "lightmaps/env_light_probe_volume_atlas_dlshd.vtex" ||
            tail == "lightmaps/env_light_probe_volume_atlas_dlshd.vtex_c";
        const bool cubemap_data =
            tail == "cubemaps/env_cubemap_array.vtex" ||
            tail == "cubemaps/env_cubemap_array.vtex_c";
        if (use_compatible_source_probe_atlas && light_probe_atlas)
        {
            // This path is already the desired source atlas. Returning false
            // also prevents a target -> source -> target redirect cycle.
            return false;
        }
        if (default_entity_lump || light_probe_atlas || cubemap_data)
        {
            const auto redirect = "maps/" + target + "/" + tail;
            return redirect.size() < replacement_size &&
                strcpy_s(replacement, replacement_size, redirect.c_str()) == 0;
        }

        // Probe-octree handshakes are regenerated whenever Hammer recompiles a
        // map. Keeping the source numeric ID normally points at no target file.
        // Rainy Nuke is a verified geometry-identical Nuke recompile: all 92
        // probe volumes retain their origin, dimensions and atlas coordinates,
        // while their generated IDs are reversed.
        constexpr std::string_view probe_prefix =
            "lightmaps/env_light_probe_volume_";
        constexpr std::string_view probe_suffix = "_octree.dat";
        if (!tail.starts_with(probe_prefix) || !tail.ends_with(probe_suffix) ||
            source != "de_nuke" || target != "de_nuke_rainy")
        {
            return false;
        }
        const auto id_text = tail.substr(
            probe_prefix.size(),
            tail.size() - probe_prefix.size() - probe_suffix.size());
        if (id_text.empty() ||
            !std::all_of(id_text.begin(), id_text.end(), [](const unsigned char character)
            {
                return std::isdigit(character) != 0;
            }))
        {
            return false;
        }
        const auto source_id = std::stoull(id_text);
        constexpr std::uint64_t first_source_id = 1372856349ull;
        constexpr std::uint64_t last_source_id = 1372856443ull;
        constexpr std::uint64_t first_target_id = 303904010ull;
        if (source_id < first_source_id || source_id > last_source_id)
        {
            return false;
        }
        const auto target_id = first_target_id - (source_id - first_source_id);
        const auto redirect = "maps/" + target + "/" +
            std::string(probe_prefix) + std::to_string(target_id) +
            std::string(probe_suffix);
        return redirect.size() < replacement_size &&
            strcpy_s(replacement, replacement_size, redirect.c_str()) == 0;
    }
    catch (...)
    {
        replacement[0] = '\0';
        return false;
    }
}

bool nskinzcs2::runtime::ShouldSuppressSourceMapModelPath(
    const std::string_view resource_path,
    const std::string_view source_map_name,
    const std::string_view target_map_name,
    const std::string_view target_skybox_resource_prefix) noexcept
{
    try
    {
        const auto source = NormalizeMapLeaf(source_map_name);
        const auto target = NormalizeMapLeaf(target_map_name);
        auto path = NormalizeResourcePath(resource_path);
        if (source.empty() || target.empty() || source == target || path.empty())
        {
            return false;
        }
        if (path.ends_with("_c"))
        {
            path.resize(path.size() - 2);
        }
        if (!path.ends_with(".vmdl"))
        {
            return false;
        }

        const auto source_map_root = "maps/" + source + "/";
        if (path.starts_with(source_map_root + "entities/") ||
            path.starts_with(source_map_root + "worldnodes/"))
        {
            return true;
        }

        if (!TargetUsesDifferentKnownSkybox(
                source_map_name,
                target_skybox_resource_prefix))
        {
            return false;
        }
        const auto source_prefab = "maps/prefabs/" + source + "/";
        if (!path.starts_with(source_prefab))
        {
            return false;
        }
        const auto tail = path.substr(source_prefab.size());
        const bool skybox = tail.find("skybox") != std::string::npos ||
            tail.find("3dsky") != std::string::npos ||
            tail.find("3d_sky") != std::string::npos;
        return skybox && tail.find("/worldnodes/") != std::string::npos;
    }
    catch (...)
    {
        return false;
    }
}

void nskinzcs2::runtime::ConfigureMapRemap(
    const nskinzcs2::model::MapRemapProfile& profile,
    const std::wstring& current_level_name)
{
    UnmountRadarOverlay();
    auto source_map = profile.source_map_name;
    if (source_map.empty())
    {
        source_map = WideToUtf8(current_level_name);
    }

    const bool valid = !profile.enabled ||
        (IsSafeMapName(source_map, true) && IsWorkshopId(profile.workshop_id) &&
         IsSafeMapName(profile.target_map_name, false) &&
         IsSafeMapName(profile.target_skybox_resource_prefix, true));
    AcquireSRWLockExclusive(&g_config_lock);
    g_config = {};
    if (profile.enabled && valid)
    {
        g_config.enabled = true;
        g_config.use_source_radar = profile.use_source_radar;
        strcpy_s(g_config.source_map, source_map.c_str());
        strcpy_s(g_config.workshop_id, profile.workshop_id.c_str());
        strcpy_s(g_config.target_map, profile.target_map_name.c_str());
        strcpy_s(
            g_config.target_skybox_resource_prefix,
            profile.target_skybox_resource_prefix.c_str());
        g_config.source_captured = !source_map.empty();
    }
    ReleaseSRWLockExclusive(&g_config_lock);
    AcquireSRWLockExclusive(&g_failed_resource_lock);
    g_failed_model_paths.clear();
    ReleaseSRWLockExclusive(&g_failed_resource_lock);
    g_replacement_count.store(0, std::memory_order_release);
    g_suppressed_source_geometry_count.store(0, std::memory_order_release);
    g_source_geometry_log_count.store(0, std::memory_order_release);
    g_source_runtime_data_log_count.store(0, std::memory_order_release);
    g_source_radar_route_logged.store(false, std::memory_order_release);
    g_radar_overview_route_logged.store(false, std::memory_order_release);
    g_source_skybox_route_logged.store(false, std::memory_order_release);

    if (!profile.enabled)
    {
        SetIssue({});
        nskinzcs2::Logger::Info(L"Map remap disabled by the staged configuration.");
        return;
    }
    if (!valid)
    {
        SetIssue(L"Map remap configuration is invalid; expected a numeric workshop ID and safe map names.");
        nskinzcs2::Logger::Warn(L"Map remap configuration rejected before hook installation.");
        return;
    }

    if (EnsureHooksInstalled())
    {
        std::wstring radar_diagnostic;
        bool radar_ready = false;
        try
        {
            radar_ready = MountPreparedRadarOverlay(profile, radar_diagnostic);
        }
        catch (const std::exception& exception)
        {
            radar_diagnostic = L"Radar overlay preparation threw an exception: " +
                Utf8ToWide(exception.what());
        }
        catch (...)
        {
            radar_diagnostic = L"Radar overlay preparation failed with an unknown exception.";
        }
        if (!radar_ready)
        {
            SetIssue(radar_diagnostic.empty()
                ? L"The selected workshop map radar could not be prepared or mounted."
                : radar_diagnostic);
            nskinzcs2::Logger::Warn(
                L"[MapRemap/RadarTexture] map-specific transparent overlay preparation failed; target radar will remain unchanged. " +
                radar_diagnostic);
        }
        std::wstring message = L"Map remap armed for the next demo load: source=";
        message += source_map.empty() ? L"<capture from DemoHeader>" : Utf8ToWide(source_map);
        message += L", workshop=" + Utf8ToWide(profile.workshop_id);
        message += L", target=" + Utf8ToWide(profile.target_map_name) + L". Reload the same demo now.";
        if (!profile.target_skybox_resource_prefix.empty())
        {
            message += L" Target 3D skybox=" +
                Utf8ToWide(profile.target_skybox_resource_prefix) + L".";
        }
        nskinzcs2::Logger::Info(message);
    }
}

nskinzcs2::runtime::MapRemapStatus nskinzcs2::runtime::SnapshotMapRemapStatus()
{
    MapRemapStatus status;
    AcquireSRWLockShared(&g_config_lock);
    status.enabled = g_config.enabled;
    status.source_captured = g_config.source_captured;
    status.use_source_radar = g_config.use_source_radar;
    status.source_map_name = g_config.source_map;
    status.workshop_id = g_config.workshop_id;
    status.target_map_name = g_config.target_map;
    ReleaseSRWLockShared(&g_config_lock);
    status.hooks_installed = g_hooks_installed.load(std::memory_order_acquire);
    status.replacement_count = g_replacement_count.load(std::memory_order_acquire);
    status.suppressed_source_geometry_count =
        g_suppressed_source_geometry_count.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(g_issue_mutex);
        status.issue = g_issue;
    }
    return status;
}

void nskinzcs2::runtime::ShutdownMapRemap()
{
    std::lock_guard<std::mutex> lock(g_install_mutex);
    UnmountRadarOverlay();
    RollbackHooks();
    AcquireSRWLockExclusive(&g_config_lock);
    g_config = {};
    ReleaseSRWLockExclusive(&g_config_lock);
    AcquireSRWLockExclusive(&g_failed_resource_lock);
    g_failed_model_paths.clear();
    ReleaseSRWLockExclusive(&g_failed_resource_lock);
    g_suppressed_source_geometry_count.store(0, std::memory_order_release);
    g_source_geometry_log_count.store(0, std::memory_order_release);
    g_source_runtime_data_log_count.store(0, std::memory_order_release);
    g_source_skybox_route_logged.store(false, std::memory_order_release);
    nskinzcs2::Logger::Info(L"Map remap hooks removed.");
}
