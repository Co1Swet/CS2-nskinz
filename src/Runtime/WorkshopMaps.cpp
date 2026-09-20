// SPDX-License-Identifier: AGPL-3.0-only
// nSkinzCS2 Workshop discovery/integration for the MulNX-derived map-remap path;
// see NOTICE.md.

#include "Runtime/WorkshopMaps.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

namespace
{
constexpr std::uint32_t kVpkSignature = 0x55AA1234u;

struct VpkHeader
{
    std::uint32_t version = 0;
    std::uint32_t tree_size = 0;
    std::uint32_t header_size = 0;
};

struct VpkEntryRecord
{
    std::string path;
    std::uint16_t archive_index = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::vector<std::uint8_t> preload;
};

std::string Trim(std::string value)
{
    const auto is_space = [](const unsigned char character) { return std::isspace(character) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](const char character)
    {
        return !is_space(static_cast<unsigned char>(character));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](const char character)
    {
        return !is_space(static_cast<unsigned char>(character));
    }).base(), value.end());
    return value;
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* key_path, const wchar_t* value_name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, key_path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return {};
    }

    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return {};
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key,
            value_name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(value.data()),
            &size) != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);

    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return value;
}

std::filesystem::path ProcessImagePath(const DWORD process_id)
{
    if (process_id == 0 || process_id == GetCurrentProcessId())
    {
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return path;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr)
    {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool success = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    if (!success)
    {
        return {};
    }
    path.resize(length);
    return path;
}

std::optional<std::filesystem::path> SteamLibraryFromCs2Path(const std::filesystem::path& image_path)
{
    const auto generic = image_path.lexically_normal().generic_wstring();
    auto lower = generic;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](const wchar_t character)
    {
        return static_cast<wchar_t>(std::towlower(character));
    });
    constexpr std::wstring_view marker = L"/steamapps/common/";
    const auto position = lower.find(marker);
    if (position == std::wstring::npos)
    {
        return std::nullopt;
    }
    return std::filesystem::path(generic.substr(0, position));
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::vector<std::filesystem::path> FindSteamLibraries(const DWORD process_id)
{
    std::vector<std::filesystem::path> roots;
    if (const auto derived = SteamLibraryFromCs2Path(ProcessImagePath(process_id)); derived.has_value())
    {
        roots.push_back(*derived);
    }

    std::wstring steam_root = ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam_root.empty())
    {
        steam_root = ReadRegistryString(HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    }
    if (!steam_root.empty())
    {
        roots.emplace_back(steam_root);
        const auto library_text = ReadTextFile(std::filesystem::path(steam_root) / L"steamapps" / L"libraryfolders.vdf");
        const std::regex path_pattern(R"re("path"\s*"([^"]+)")re", std::regex::icase);
        for (auto iterator = std::sregex_iterator(library_text.begin(), library_text.end(), path_pattern);
             iterator != std::sregex_iterator();
             ++iterator)
        {
            auto path = (*iterator)[1].str();
            std::string unescaped;
            unescaped.reserve(path.size());
            for (std::size_t index = 0; index < path.size(); ++index)
            {
                if (path[index] == '\\' && index + 1 < path.size() && path[index + 1] == '\\')
                {
                    ++index;
                }
                unescaped.push_back(path[index]);
            }
            roots.emplace_back(std::filesystem::path(unescaped));
        }
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

std::set<std::string> ParseInstalledWorkshopIds(const std::filesystem::path& manifest_path)
{
    const auto text = ReadTextFile(manifest_path);
    std::set<std::string> ids;
    const std::regex item_pattern(R"re("([0-9]{6,20})"\s*\{)re");
    for (auto iterator = std::sregex_iterator(text.begin(), text.end(), item_pattern);
         iterator != std::sregex_iterator();
         ++iterator)
    {
        ids.emplace((*iterator)[1].str());
    }
    return ids;
}

std::string ParsePublishValue(const std::string& text, const char* key)
{
    const std::regex pattern(std::string(R"re(")re") + key + R"re("\s*"([^"]*)")re", std::regex::icase);
    std::smatch match;
    return std::regex_search(text, match, pattern) ? Trim(match[1].str()) : std::string {};
}

template <typename T>
bool ReadBinary(std::ifstream& stream, T& value)
{
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

bool ReadCString(std::ifstream& stream, std::string& value, const std::streamoff tree_end)
{
    value.clear();
    char character = '\0';
    while (stream && stream.tellg() < tree_end && stream.get(character))
    {
        if (character == '\0')
        {
            return true;
        }
        if (value.size() >= 4096)
        {
            return false;
        }
        value.push_back(character);
    }
    return false;
}

std::optional<VpkHeader> ReadVpkHeader(std::ifstream& stream)
{
    std::uint32_t signature = 0;
    VpkHeader header;
    if (!ReadBinary(stream, signature) || signature != kVpkSignature ||
        !ReadBinary(stream, header.version) || !ReadBinary(stream, header.tree_size))
    {
        return std::nullopt;
    }

    header.header_size = 12;
    if (header.version == 2)
    {
        std::array<std::uint32_t, 4> ignored {};
        if (!stream.read(reinterpret_cast<char*>(ignored.data()), sizeof(ignored)))
        {
            return std::nullopt;
        }
        header.header_size = 28;
    }
    else if (header.version != 1)
    {
        return std::nullopt;
    }
    return header;
}

std::vector<std::string> EnumerateMapsFromVpk(const std::filesystem::path& vpk_path)
{
    std::ifstream stream(vpk_path, std::ios::binary);
    const auto header = ReadVpkHeader(stream);
    if (!header.has_value() || header->tree_size > 128u * 1024u * 1024u)
    {
        return {};
    }

    const auto tree_end = static_cast<std::streamoff>(header->header_size + header->tree_size);
    std::vector<std::string> maps;
    std::string extension;
    std::string directory;
    std::string filename;

    while (ReadCString(stream, extension, tree_end) && !extension.empty())
    {
        while (ReadCString(stream, directory, tree_end) && !directory.empty())
        {
            while (ReadCString(stream, filename, tree_end) && !filename.empty())
            {
                std::uint32_t crc = 0;
                std::uint16_t preload_bytes = 0;
                std::uint16_t archive_index = 0;
                std::uint32_t offset = 0;
                std::uint32_t length = 0;
                std::uint16_t terminator = 0;
                if (!ReadBinary(stream, crc) || !ReadBinary(stream, preload_bytes) ||
                    !ReadBinary(stream, archive_index) || !ReadBinary(stream, offset) ||
                    !ReadBinary(stream, length) || !ReadBinary(stream, terminator) || terminator != 0xFFFF)
                {
                    return {};
                }
                (void)crc;
                (void)archive_index;
                (void)offset;
                (void)length;

                // Only top-level maps/<name>.vpk entries are loadable map
                // packages. Nested maps/prefabs/*.vpk entries are resources,
                // not selectable workshop maps.
                if (_stricmp(extension.c_str(), "vpk") == 0 && directory == "maps")
                {
                    maps.push_back(filename);
                }

                if (preload_bytes != 0)
                {
                    stream.seekg(preload_bytes, std::ios::cur);
                    if (!stream || stream.tellg() > tree_end)
                    {
                        return {};
                    }
                }
            }
        }
    }

    std::sort(maps.begin(), maps.end());
    maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
    return maps;
}

std::vector<VpkEntryRecord> EnumerateVpkEntries(const std::filesystem::path& vpk_path)
{
    std::ifstream stream(vpk_path, std::ios::binary);
    const auto header = ReadVpkHeader(stream);
    if (!header.has_value() || header->tree_size > 128u * 1024u * 1024u)
    {
        return {};
    }

    const auto tree_end = static_cast<std::streamoff>(header->header_size + header->tree_size);
    std::vector<VpkEntryRecord> entries;
    std::string extension;
    std::string directory;
    std::string filename;
    while (ReadCString(stream, extension, tree_end) && !extension.empty())
    {
        while (ReadCString(stream, directory, tree_end) && !directory.empty())
        {
            while (ReadCString(stream, filename, tree_end) && !filename.empty())
            {
                std::uint32_t crc = 0;
                std::uint16_t preload_bytes = 0;
                VpkEntryRecord entry;
                std::uint16_t terminator = 0;
                if (!ReadBinary(stream, crc) || !ReadBinary(stream, preload_bytes) ||
                    !ReadBinary(stream, entry.archive_index) || !ReadBinary(stream, entry.offset) ||
                    !ReadBinary(stream, entry.length) || !ReadBinary(stream, terminator) || terminator != 0xFFFF)
                {
                    return {};
                }
                (void)crc;
                const auto normalized_directory = directory == " " ? std::string {} : directory;
                entry.path = normalized_directory.empty()
                    ? filename
                    : normalized_directory + "/" + filename;
                if (extension != " ")
                {
                    entry.path += "." + extension;
                }
                entry.preload.resize(preload_bytes);
                if (preload_bytes != 0 &&
                    !stream.read(reinterpret_cast<char*>(entry.preload.data()), preload_bytes))
                {
                    return {};
                }
                if (!stream || stream.tellg() > tree_end)
                {
                    return {};
                }
                entries.push_back(std::move(entry));
            }
        }
    }
    return entries;
}

bool ReadVpkEntryBytes(
    const std::filesystem::path& index_path,
    const VpkEntryRecord& entry,
    std::vector<std::uint8_t>& output)
{
    constexpr std::uint16_t directory_archive_index = 0x7FFF;
    std::filesystem::path archive_path = index_path;
    std::uint64_t absolute_offset = entry.offset;
    if (entry.archive_index == directory_archive_index)
    {
        std::ifstream index_stream(index_path, std::ios::binary);
        const auto header = ReadVpkHeader(index_stream);
        if (!header.has_value())
        {
            return false;
        }
        absolute_offset += static_cast<std::uint64_t>(header->header_size) + header->tree_size;
    }
    else
    {
        auto stem = index_path.filename().wstring();
        constexpr std::wstring_view directory_suffix = L"_dir.vpk";
        if (!stem.ends_with(directory_suffix))
        {
            return false;
        }
        stem.resize(stem.size() - directory_suffix.size());
        wchar_t archive_suffix[16] {};
        swprintf_s(archive_suffix, L"_%03u.vpk", static_cast<unsigned>(entry.archive_index));
        archive_path = index_path.parent_path() / (stem + archive_suffix);
    }

    if (entry.length > 64u * 1024u * 1024u)
    {
        return false;
    }
    std::ifstream stream(archive_path, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(absolute_offset));
    if (!stream)
    {
        return false;
    }
    output = entry.preload;
    const auto original_size = output.size();
    output.resize(original_size + entry.length);
    return entry.length == 0 || static_cast<bool>(stream.read(
        reinterpret_cast<char*>(output.data() + original_size),
        static_cast<std::streamsize>(entry.length)));
}

std::string LowerPath(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

const VpkEntryRecord* SelectRadarEntry(
    const std::vector<VpkEntryRecord>& entries,
    const std::string_view target_map_name)
{
    auto target = LowerPath(std::string(target_map_name));
    if (const auto slash = target.find_last_of('/'); slash != std::string::npos)
    {
        target.erase(0, slash + 1);
    }
    const auto exact = "panorama/images/overheadmaps/" + target + "_radar_tga.vtex_c";
    const VpkEntryRecord* fallback = nullptr;
    std::size_t fallback_count = 0;
    for (const auto& entry : entries)
    {
        const auto normalized = LowerPath(entry.path);
        if (normalized == exact)
        {
            return &entry;
        }
        constexpr std::string_view prefix = "panorama/images/overheadmaps/";
        constexpr std::string_view suffix = "_radar_tga.vtex_c";
        if (normalized.starts_with(prefix) && normalized.ends_with(suffix))
        {
            const auto leaf = normalized.substr(prefix.size());
            if (leaf.starts_with(target + "_radar"))
            {
                return &entry;
            }
            fallback = &entry;
            ++fallback_count;
        }
    }
    return fallback_count == 1 ? fallback : nullptr;
}

std::string SourceFolderName(const std::string& source_folder)
{
    auto normalized = source_folder;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    while (!normalized.empty() && normalized.back() == '/')
    {
        normalized.pop_back();
    }
    const auto slash = normalized.find_last_of('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

std::string SelectWorkshopSkyboxResourcePrefix(
    const std::vector<VpkEntryRecord>& entries,
    const std::string_view target_map_name)
{
    std::vector<std::string> candidates;
    for (const auto& entry : entries)
    {
        auto normalized = LowerPath(entry.path);
        if (!normalized.starts_with("maps/prefabs/") || !normalized.ends_with(".vpk"))
        {
            continue;
        }
        const auto leaf_offset = normalized.find_last_of('/');
        const auto leaf = normalized.substr(
            leaf_offset == std::string::npos ? 0 : leaf_offset + 1);
        if (leaf.find("skybox") == std::string::npos &&
            leaf.find("3dsky") == std::string::npos &&
            leaf.find("3d_sky") == std::string::npos)
        {
            continue;
        }
        normalized.resize(normalized.size() - 4); // .vpk
        candidates.push_back(std::move(normalized));
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() == 1)
    {
        return candidates.front();
    }

    auto target = LowerPath(std::string(target_map_name));
    if (const auto slash = target.find_last_of('/'); slash != std::string::npos)
    {
        target.erase(0, slash + 1);
    }
    const std::string target_segment = "/" + target + "/";
    const std::string target_leaf = "/" + target;
    const auto match = std::find_if(candidates.begin(), candidates.end(), [&](const std::string& candidate)
    {
        return candidate.find(target_segment) != std::string::npos ||
            candidate.ends_with(target_leaf) ||
            candidate.find(target + "_sky") != std::string::npos;
    });
    return match != candidates.end() ? *match : std::string {};
}
}

bool nskinzcs2::runtime::IsLikelyAuxiliaryWorkshopMap(const std::string_view map_name)
{
    std::string normalized(map_name);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (const auto slash = normalized.find_last_of('/'); slash != std::string::npos)
    {
        normalized.erase(0, slash + 1);
    }
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    // Source 2 workshop bundles commonly expose their separately compiled 3D
    // skybox as maps/<main>_sky_<variant>.vpk. Loading that package directly
    // produces a valid level with the wrong scale/origin and looks like severe
    // camera clipping. Keep the checks separator-aware so playable names such
    // as "skyline" are not hidden.
    constexpr std::array<std::string_view, 7> auxiliary_markers {
        "_sky_",
        "-sky-",
        "_3dsky",
        "_3d_sky",
        "_skybox",
        "_background",
        "_preview",
    };
    if (normalized.starts_with("skybox_") || normalized.starts_with("3dsky_") ||
        normalized.ends_with("_sky"))
    {
        return true;
    }
    return std::any_of(auxiliary_markers.begin(), auxiliary_markers.end(), [&](const std::string_view marker)
    {
        return normalized.find(marker) != std::string::npos;
    });
}

bool nskinzcs2::runtime::IsWorkshopVpkIndexFile(const std::wstring_view filename)
{
    std::wstring normalized(filename);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const wchar_t character)
    {
        return static_cast<wchar_t>(std::towlower(character));
    });
    if (!normalized.ends_with(L".vpk"))
    {
        return false;
    }
    if (normalized.ends_with(L"_dir.vpk"))
    {
        return true;
    }

    // Split VPK payloads end in _000.vpk, _001.vpk, ... and do not contain a
    // directory tree. Everything else ending in .vpk can be a self-contained
    // Workshop addon and is safe to inspect as a possible index.
    constexpr std::size_t suffix_length = 8; // _000.vpk
    if (normalized.size() >= suffix_length)
    {
        const auto suffix = normalized.substr(normalized.size() - suffix_length);
        if (suffix[0] == L'_' && std::iswdigit(suffix[1]) && std::iswdigit(suffix[2]) &&
            std::iswdigit(suffix[3]) && suffix.substr(4) == L".vpk")
        {
            return false;
        }
    }
    return true;
}

nskinzcs2::runtime::WorkshopMapScanResult nskinzcs2::runtime::DiscoverSubscribedWorkshopMaps(
    const DWORD cs2_process_id)
{
    WorkshopMapScanResult result;
    std::size_t skipped_auxiliary_maps = 0;
    std::size_t standalone_vpk_packages = 0;
    const auto libraries = FindSteamLibraries(cs2_process_id);
    if (libraries.empty())
    {
        result.diagnostics.push_back(L"Steam library roots could not be resolved.");
        return result;
    }

    for (const auto& library : libraries)
    {
        const auto workshop_root = library / L"steamapps" / L"workshop";
        const auto manifest = workshop_root / L"appworkshop_730.acf";
        const auto content_root = workshop_root / L"content" / L"730";
        if (!std::filesystem::is_regular_file(manifest) || !std::filesystem::is_directory(content_root))
        {
            continue;
        }

        const auto installed_ids = ParseInstalledWorkshopIds(manifest);
        for (const auto& id : installed_ids)
        {
            const auto item_directory = content_root / std::filesystem::path(id);
            if (!std::filesystem::is_directory(item_directory))
            {
                continue;
            }

            const auto publish_text = ReadTextFile(item_directory / L"publish_data.txt");
            auto title = ParsePublishValue(publish_text, "title");
            auto addon_name = SourceFolderName(ParsePublishValue(publish_text, "source_folder"));
            if (title.empty())
            {
                title = "Workshop " + id;
            }

            std::vector<std::string> maps;
            std::map<std::string, std::string> skybox_by_map;
            for (const auto& file : std::filesystem::directory_iterator(item_directory))
            {
                if (!file.is_regular_file())
                {
                    continue;
                }
                auto filename = file.path().filename().wstring();
                std::transform(filename.begin(), filename.end(), filename.begin(), [](const wchar_t character)
                {
                    return static_cast<wchar_t>(std::towlower(character));
                });
                if (!IsWorkshopVpkIndexFile(filename))
                {
                    continue;
                }
                if (!filename.ends_with(L"_dir.vpk"))
                {
                    ++standalone_vpk_packages;
                }
                auto vpk_maps = EnumerateMapsFromVpk(file.path());
                const auto entries = EnumerateVpkEntries(file.path());
                for (const auto& map_name : vpk_maps)
                {
                    const auto skybox_prefix =
                        SelectWorkshopSkyboxResourcePrefix(entries, map_name);
                    if (!skybox_prefix.empty())
                    {
                        skybox_by_map[map_name] = skybox_prefix;
                    }
                }
                maps.insert(maps.end(), vpk_maps.begin(), vpk_maps.end());
                if (addon_name.empty())
                {
                    addon_name = file.path().stem().string();
                    if (addon_name.ends_with("_dir"))
                    {
                        addon_name.resize(addon_name.size() - 4);
                    }
                }
            }

            std::sort(maps.begin(), maps.end());
            maps.erase(std::unique(maps.begin(), maps.end()), maps.end());
            if (addon_name.empty())
            {
                continue;
            }
            for (const auto& map_name : maps)
            {
                if (IsLikelyAuxiliaryWorkshopMap(map_name))
                {
                    ++skipped_auxiliary_maps;
                    continue;
                }
                const auto skybox = skybox_by_map.find(map_name);
                result.entries.push_back({
                    id,
                    title,
                    addon_name,
                    map_name,
                    item_directory,
                    skybox != skybox_by_map.end() ? skybox->second : std::string {},
                });
            }
        }
    }

    std::sort(result.entries.begin(), result.entries.end(), [](const WorkshopMapEntry& lhs, const WorkshopMapEntry& rhs)
    {
        if (lhs.title != rhs.title)
        {
            return lhs.title < rhs.title;
        }
        if (lhs.workshop_id != rhs.workshop_id)
        {
            return lhs.workshop_id < rhs.workshop_id;
        }
        return lhs.map_name < rhs.map_name;
    });
    result.entries.erase(std::unique(result.entries.begin(), result.entries.end(), [](const auto& lhs, const auto& rhs)
    {
        return lhs.workshop_id == rhs.workshop_id && lhs.addon_name == rhs.addon_name && lhs.map_name == rhs.map_name;
    }), result.entries.end());

    if (skipped_auxiliary_maps != 0)
    {
        result.diagnostics.push_back(
            L"Skipped " + std::to_wstring(skipped_auxiliary_maps) +
            L" auxiliary skybox/background map package(s).");
    }

    if (standalone_vpk_packages != 0)
    {
        result.diagnostics.push_back(
            L"Detected " + std::to_wstring(standalone_vpk_packages) +
            L" self-contained Workshop VPK package(s).");
    }

    if (result.entries.empty())
    {
        result.diagnostics.push_back(L"No installed CS2 workshop map packages were found. Legacy CS:GO packages are skipped.");
    }
    else
    {
        result.diagnostics.push_back(
            L"Detected " + std::to_wstring(result.entries.size()) + L" installed CS2 workshop map entries.");
    }
    return result;
}

std::optional<nskinzcs2::runtime::WorkshopRadarTexture>
nskinzcs2::runtime::ReadWorkshopRadarTexture(
    const std::string_view workshop_id,
    const std::string_view target_map_name,
    const DWORD cs2_process_id,
    std::wstring* diagnostic)
{
    const auto set_diagnostic = [&](std::wstring value)
    {
        if (diagnostic != nullptr)
        {
            *diagnostic = std::move(value);
        }
    };
    if (workshop_id.empty() || target_map_name.empty() ||
        !std::all_of(workshop_id.begin(), workshop_id.end(), [](const unsigned char character)
        {
            return std::isdigit(character) != 0;
        }))
    {
        set_diagnostic(L"The Workshop radar request contains an invalid item ID or map name.");
        return std::nullopt;
    }

    for (const auto& library : FindSteamLibraries(cs2_process_id))
    {
        const auto item_directory = library / L"steamapps" / L"workshop" / L"content" / L"730" /
            std::filesystem::path(workshop_id);
        std::error_code iterator_error;
        if (!std::filesystem::is_directory(item_directory, iterator_error))
        {
            continue;
        }
        for (const auto& file : std::filesystem::directory_iterator(item_directory, iterator_error))
        {
            if (iterator_error || !file.is_regular_file())
            {
                continue;
            }
            if (!IsWorkshopVpkIndexFile(file.path().filename().wstring()))
            {
                continue;
            }
            const auto entries = EnumerateVpkEntries(file.path());
            const auto* radar = SelectRadarEntry(entries, target_map_name);
            if (radar == nullptr)
            {
                continue;
            }
            WorkshopRadarTexture result;
            result.virtual_path = radar->path;
            if (ReadVpkEntryBytes(file.path(), *radar, result.bytes) && !result.bytes.empty())
            {
                set_diagnostic({});
                return result;
            }
        }
    }

    set_diagnostic(
        L"No unique compiled radar texture was found in Workshop item " +
        std::wstring(workshop_id.begin(), workshop_id.end()) + L" for map " +
        std::wstring(target_map_name.begin(), target_map_name.end()) + L".");
    return std::nullopt;
}
