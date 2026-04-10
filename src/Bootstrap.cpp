#include "Core/AppState.hpp"
#include "Core/BuildConfig.hpp"
#include "Core/Config.hpp"
#include "Core/Log.hpp"
#include "Cs2/Catalog.hpp"
#include "Cs2/Runtime.hpp"
#include "UI/MainWindow.hpp"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
std::unique_ptr<nskinzcs2::AppState> g_state;
std::unique_ptr<nskinzcs2::RuntimeController> g_runtime;
std::unique_ptr<nskinzcs2::MainWindow> g_window;
std::filesystem::path g_base_directory;
std::filesystem::path g_config_path;

struct ResolvedCatalogText
{
    std::string items_game_text;
    std::string localization_text;
    std::wstring source_description;
};

struct VpkHeader
{
    std::uint32_t version = 0;
    std::uint32_t tree_size = 0;
    std::uint32_t file_data_section_size = 0;
    std::uint64_t header_size = 0;
};

struct VpkEntryLocation
{
    std::uint16_t archive_index = 0;
    std::uint16_t preload_bytes = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint64_t preload_offset = 0;
};

constexpr std::uint32_t kVpkSignature = 0x55AA1234;
constexpr std::uint16_t kVpkDirectoryArchiveIndex = 0x7FFF;

LONG LogStructuredThreadException(const wchar_t* thread_name, EXCEPTION_POINTERS* exception_info)
{
    std::wstringstream stream;
    stream << thread_name << L" raised SEH 0x";
    if (exception_info != nullptr && exception_info->ExceptionRecord != nullptr)
    {
        stream << std::hex << std::uppercase << exception_info->ExceptionRecord->ExceptionCode;
        if (exception_info->ExceptionRecord->ExceptionAddress != nullptr)
        {
            stream << L" at " << exception_info->ExceptionRecord->ExceptionAddress;
        }
    }
    else
    {
        stream << L"UNKNOWN";
    }

    nskinzcs2::Logger::Error(stream.str());
    return EXCEPTION_EXECUTE_HANDLER;
}

std::filesystem::path ResolveModuleDirectory(HMODULE module)
{
    std::wstring buffer(MAX_PATH, L'\0');
    const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path ResolveCurrentProcessPath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::optional<std::string> ReadFileText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        return std::nullopt;
    }

    stream.seekg(0, std::ios::end);
    const auto length = stream.tellg();
    if (length < 0)
    {
        return std::nullopt;
    }

    std::string text(static_cast<std::size_t>(length), '\0');
    stream.seekg(0, std::ios::beg);
    if (!text.empty())
    {
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream)
        {
            return std::nullopt;
        }
    }

    if (text.size() >= 2)
    {
        const auto first = static_cast<unsigned char>(text[0]);
        const auto second = static_cast<unsigned char>(text[1]);
        if ((first == 0xFF && second == 0xFE) || (first == 0xFE && second == 0xFF))
        {
            if (((text.size() - 2) % 2) != 0)
            {
                return std::nullopt;
            }

            const bool little_endian = first == 0xFF;
            auto read_code_unit = [&](const std::size_t index) noexcept
            {
                const auto low = static_cast<unsigned char>(text[index]);
                const auto high = static_cast<unsigned char>(text[index + 1]);
                return little_endian
                    ? static_cast<char16_t>(low | (high << 8))
                    : static_cast<char16_t>(high | (low << 8));
            };

            auto append_utf8 = [](std::string& target, const char32_t codepoint)
            {
                if (codepoint <= 0x7F)
                {
                    target.push_back(static_cast<char>(codepoint));
                    return;
                }

                if (codepoint <= 0x7FF)
                {
                    target.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                    target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    return;
                }

                if (codepoint <= 0xFFFF)
                {
                    target.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                    target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                    target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    return;
                }

                target.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                target.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            };

            std::string utf8_text;
            utf8_text.reserve(text.size());
            for (std::size_t index = 2; index + 1 < text.size(); index += 2)
            {
                const auto first_unit = read_code_unit(index);
                char32_t codepoint = first_unit;
                if (first_unit >= 0xD800 && first_unit <= 0xDBFF)
                {
                    if (index + 3 < text.size())
                    {
                        const auto second_unit = read_code_unit(index + 2);
                        if (second_unit >= 0xDC00 && second_unit <= 0xDFFF)
                        {
                            codepoint =
                                0x10000u +
                                ((static_cast<char32_t>(first_unit) - 0xD800u) << 10u) +
                                (static_cast<char32_t>(second_unit) - 0xDC00u);
                            index += 2;
                        }
                        else
                        {
                            codepoint = 0xFFFD;
                        }
                    }
                    else
                    {
                        codepoint = 0xFFFD;
                    }
                }
                else if (first_unit >= 0xDC00 && first_unit <= 0xDFFF)
                {
                    codepoint = 0xFFFD;
                }

                append_utf8(utf8_text, codepoint);
            }

            return utf8_text;
        }
    }

    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
    {
        text.erase(0, 3);
    }

    return text;
}

template <typename T>
bool ReadBinaryValue(std::ifstream& stream, T& value)
{
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

bool ReadNullTerminatedString(std::ifstream& stream, std::string& value)
{
    value.clear();

    char character = '\0';
    while (stream.get(character))
    {
        if (character == '\0')
        {
            return true;
        }

        value.push_back(character);
    }

    return false;
}

std::optional<VpkHeader> ReadVpkHeader(std::ifstream& stream)
{
    VpkHeader header;

    std::uint32_t signature = 0;
    if (!ReadBinaryValue(stream, signature) || signature != kVpkSignature)
    {
        return std::nullopt;
    }

    if (!ReadBinaryValue(stream, header.version) || !ReadBinaryValue(stream, header.tree_size))
    {
        return std::nullopt;
    }

    header.header_size = sizeof(std::uint32_t) * 3;
    if (header.version == 2)
    {
        std::uint32_t archive_md5_section_size = 0;
        std::uint32_t other_md5_section_size = 0;
        std::uint32_t signature_section_size = 0;
        if (!ReadBinaryValue(stream, header.file_data_section_size) ||
            !ReadBinaryValue(stream, archive_md5_section_size) ||
            !ReadBinaryValue(stream, other_md5_section_size) ||
            !ReadBinaryValue(stream, signature_section_size))
        {
            return std::nullopt;
        }

        (void)archive_md5_section_size;
        (void)other_md5_section_size;
        (void)signature_section_size;
        header.header_size += sizeof(std::uint32_t) * 4;
    }
    else if (header.version != 1)
    {
        return std::nullopt;
    }

    return header;
}

std::filesystem::path BuildVpkArchivePath(const std::filesystem::path& directory_vpk_path, const std::uint16_t archive_index)
{
    auto archive_path = directory_vpk_path;
    const auto stem = directory_vpk_path.stem().wstring();
    const auto dir_suffix = std::wstring(L"_dir");
    const auto base_name = stem.ends_with(dir_suffix)
        ? stem.substr(0, stem.size() - dir_suffix.size())
        : stem;

    std::wstringstream name;
    name << base_name << L'_' << (archive_index / 100) % 10 << (archive_index / 10) % 10 << archive_index % 10
        << directory_vpk_path.extension().wstring();
    archive_path.replace_filename(name.str());
    return archive_path;
}

std::optional<std::string> ReadTextFromVpk(
    const std::filesystem::path& directory_vpk_path,
    const std::string_view requested_entry_path)
{
    std::ifstream directory_stream(directory_vpk_path, std::ios::binary);
    if (!directory_stream.is_open())
    {
        return std::nullopt;
    }

    const auto header = ReadVpkHeader(directory_stream);
    if (!header.has_value())
    {
        return std::nullopt;
    }

    const auto tree_end_offset = header->header_size + header->tree_size;
    std::string extension;
    std::string directory;
    std::string filename;
    std::optional<VpkEntryLocation> match;

    while (ReadNullTerminatedString(directory_stream, extension) && !extension.empty())
    {
        while (ReadNullTerminatedString(directory_stream, directory) && !directory.empty())
        {
            while (ReadNullTerminatedString(directory_stream, filename) && !filename.empty())
            {
                VpkEntryLocation entry;
                std::uint32_t crc = 0;
                std::uint16_t terminator = 0;
                if (!ReadBinaryValue(directory_stream, crc) ||
                    !ReadBinaryValue(directory_stream, entry.preload_bytes) ||
                    !ReadBinaryValue(directory_stream, entry.archive_index) ||
                    !ReadBinaryValue(directory_stream, entry.offset) ||
                    !ReadBinaryValue(directory_stream, entry.length) ||
                    !ReadBinaryValue(directory_stream, terminator))
                {
                    return std::nullopt;
                }
                if (terminator != 0xFFFF)
                {
                    return std::nullopt;
                }

                entry.preload_offset = static_cast<std::uint64_t>(directory_stream.tellg());
                (void)crc;

                std::string full_path;
                if (!directory.empty() && directory != " ")
                {
                    full_path.append(directory);
                    full_path.push_back('/');
                }
                full_path.append(filename);
                if (!extension.empty() && extension != " ")
                {
                    full_path.push_back('.');
                    full_path.append(extension);
                }

                if (full_path == requested_entry_path)
                {
                    match = entry;
                }

                if (entry.preload_bytes > 0)
                {
                    directory_stream.seekg(entry.preload_bytes, std::ios::cur);
                    if (!directory_stream)
                    {
                        return std::nullopt;
                    }
                }

                if (match.has_value())
                {
                    break;
                }
            }

            if (match.has_value())
            {
                break;
            }
        }

        if (match.has_value())
        {
            break;
        }
    }

    if (!match.has_value())
    {
        return std::nullopt;
    }

    std::string text;
    text.reserve(match->preload_bytes + match->length);

    if (match->preload_bytes > 0)
    {
        directory_stream.clear();
        directory_stream.seekg(static_cast<std::streamoff>(match->preload_offset), std::ios::beg);
        if (!directory_stream)
        {
            return std::nullopt;
        }

        std::vector<char> preload(static_cast<std::size_t>(match->preload_bytes));
        if (!preload.empty())
        {
            directory_stream.read(preload.data(), static_cast<std::streamsize>(preload.size()));
            if (!directory_stream)
            {
                return std::nullopt;
            }
            text.append(preload.data(), preload.size());
        }
    }

    if (match->length == 0)
    {
        return text;
    }

    const auto payload_path = match->archive_index == kVpkDirectoryArchiveIndex
        ? directory_vpk_path
        : BuildVpkArchivePath(directory_vpk_path, match->archive_index);
    std::ifstream payload_stream(payload_path, std::ios::binary);
    if (!payload_stream.is_open())
    {
        return std::nullopt;
    }

    const auto payload_offset = match->archive_index == kVpkDirectoryArchiveIndex
        ? static_cast<std::uint64_t>(tree_end_offset) + header->file_data_section_size + match->offset
        : static_cast<std::uint64_t>(match->offset);
    payload_stream.seekg(static_cast<std::streamoff>(payload_offset), std::ios::beg);
    if (!payload_stream)
    {
        return std::nullopt;
    }

    std::vector<char> payload(static_cast<std::size_t>(match->length));
    payload_stream.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!payload_stream)
    {
        return std::nullopt;
    }

    text.append(payload.data(), payload.size());
    return text;
}

std::optional<ResolvedCatalogText> TryResolveGameDataText()
{
    auto probe = ResolveCurrentProcessPath().parent_path();
    while (!probe.empty())
    {
        const auto content_root = probe / L"csgo";
        const auto items_game_path = content_root / L"scripts" / L"items" / L"items_game.txt";
        const auto localization_path =
            content_root / L"resource" / nskinzcs2::build::kCatalogLocalizationFileName;
        if (std::filesystem::exists(items_game_path) && std::filesystem::exists(localization_path))
        {
            const auto items_game_text = ReadFileText(items_game_path);
            const auto localization_text = ReadFileText(localization_path);
            if (items_game_text.has_value() && localization_text.has_value())
            {
                return ResolvedCatalogText {
                    std::move(*items_game_text),
                    std::move(*localization_text),
                    L"loose files: " + items_game_path.wstring()
                };
            }
        }

        const auto directory_vpk_path = content_root / L"pak01_dir.vpk";
        if (std::filesystem::exists(directory_vpk_path))
        {
            const auto items_game_text = ReadTextFromVpk(directory_vpk_path, "scripts/items/items_game.txt");
            const auto localization_text =
                ReadTextFromVpk(
                    directory_vpk_path,
                    std::string(nskinzcs2::build::kCatalogLocalizationVpkPath));
            if (items_game_text.has_value() && localization_text.has_value())
            {
                return ResolvedCatalogText {
                    std::move(*items_game_text),
                    std::move(*localization_text),
                    L"VPK: " + directory_vpk_path.wstring()
                };
            }
        }

        const auto parent = probe.parent_path();
        if (parent == probe)
        {
            break;
        }

        probe = parent;
    }

    return std::nullopt;
}

DWORD BootstrapThreadImpl(void* instance)
{
    auto* module = static_cast<HMODULE>(instance);
    g_base_directory = ResolveModuleDirectory(module);
    g_config_path = g_base_directory / L"nSkinzCS2.json";

    nskinzcs2::Logger::Initialize(g_base_directory.wstring());
    nskinzcs2::Logger::Info(L"nSkinzCS2 bootstrap starting (CrashGuard-SafeStart-v2).");
    nskinzcs2::Logger::Info(L"Bootstrap note: UI creation is deferred until Insert, and runtime hook attach is deferred until Update.");

    std::wstringstream module_stream;
    module_stream << L"DLL directory: " << g_base_directory.wstring();
    nskinzcs2::Logger::Info(module_stream.str());

    g_state = std::make_unique<nskinzcs2::AppState>();

    auto catalog = nskinzcs2::CatalogBuilder::BuildFallbackCatalog();
    try
    {
        if (const auto game_data = TryResolveGameDataText(); game_data.has_value())
        {
            if (const auto parsed_catalog =
                    nskinzcs2::CatalogBuilder::BuildCatalogFromGameText(
                        game_data->items_game_text,
                        game_data->localization_text);
                parsed_catalog.has_value())
            {
                catalog = std::move(*parsed_catalog);
                nskinzcs2::Logger::Info(L"Loaded catalog from local CS2 game files.");
                nskinzcs2::Logger::Info(L"Catalog source: " + game_data->source_description);
                std::wstringstream stream;
                stream << L"Catalog counts: base=" << catalog.base_items.size()
                    << L", knives=" << catalog.knife_items.size()
                    << L", gloves=" << catalog.glove_items.size()
                    << L", weapon paints=" << catalog.weapon_paint_kits.size()
                    << L", glove paints=" << catalog.glove_paint_kits.size()
                    << L", stickers=" << catalog.sticker_kits.size()
                    << L", qualities=" << catalog.qualities.size() << L'.';
                nskinzcs2::Logger::Info(stream.str());
            }
            else
            {
                nskinzcs2::Logger::Info(L"Failed to parse resolved local CS2 game data, continuing with fallback catalog.");
            }
        }
        else
        {
            nskinzcs2::Logger::Info(L"Could not locate local CS2 game data, continuing with fallback catalog.");
        }
    }
    catch (const std::exception& exception)
    {
        (void)exception;
        nskinzcs2::Logger::Warn(L"Catalog parsing raised a C++ exception; continuing with fallback catalog.");
    }
    catch (...)
    {
        nskinzcs2::Logger::Warn(L"Catalog parsing raised an unknown exception; continuing with fallback catalog.");
    }

    g_state->ReplaceCatalog(std::move(catalog));
    g_state->ReplaceConfig(nskinzcs2::ConfigStore::Load(g_config_path));

    g_runtime = std::make_unique<nskinzcs2::RuntimeController>(*g_state, module);
    g_runtime->Start();

    g_window = std::make_unique<nskinzcs2::MainWindow>(*g_state);
    g_window->Start();

    nskinzcs2::Logger::Info(L"nSkinzCS2 bootstrap completed.");
    return 0;
}

DWORD BootstrapThreadCatchAll(void* instance)
{
    try
    {
        return BootstrapThreadImpl(instance);
    }
    catch (const std::exception& exception)
    {
        (void)exception;
        nskinzcs2::Logger::Error(L"Bootstrap thread raised a C++ exception.");
        return 0;
    }
    catch (...)
    {
        nskinzcs2::Logger::Error(L"Bootstrap thread raised an unknown C++ exception.");
        return 0;
    }
}
}

extern "C" DWORD WINAPI BootstrapThread(void* instance)
{
    __try
    {
        return BootstrapThreadCatchAll(instance);
    }
    __except (LogStructuredThreadException(L"Bootstrap thread", GetExceptionInformation()))
    {
        return 0;
    }
}
