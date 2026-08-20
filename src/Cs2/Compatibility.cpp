#include "Cs2/Compatibility.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>

namespace
{
using ByteVector = std::vector<std::uint8_t>;

std::optional<ByteVector> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open())
    {
        return std::nullopt;
    }

    const auto length = stream.tellg();
    if (length <= 0)
    {
        return std::nullopt;
    }

    ByteVector bytes(static_cast<std::size_t>(length));
    stream.seekg(0, std::ios::beg);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), length))
    {
        return std::nullopt;
    }
    return bytes;
}

template <typename T>
bool ReadStruct(const ByteVector& file, const std::size_t offset, T& value) noexcept
{
    if (offset > file.size() || sizeof(T) > file.size() - offset)
    {
        return false;
    }
    std::memcpy(&value, file.data() + offset, sizeof(T));
    return true;
}

struct PeText
{
    std::span<const std::uint8_t> bytes;
    std::uint32_t timestamp = 0;
};

std::optional<PeText> GetPeTextSection(const ByteVector& file)
{
    IMAGE_DOS_HEADER dos {};
    if (!ReadStruct(file, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
    {
        return std::nullopt;
    }

    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER file_header {};
    if (!ReadStruct(file, nt_offset, signature) || signature != IMAGE_NT_SIGNATURE ||
        !ReadStruct(file, nt_offset + sizeof(DWORD), file_header))
    {
        return std::nullopt;
    }

    const auto section_offset =
        nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + file_header.SizeOfOptionalHeader;
    for (std::size_t index = 0; index < file_header.NumberOfSections; ++index)
    {
        IMAGE_SECTION_HEADER section {};
        if (!ReadStruct(file, section_offset + index * sizeof(section), section))
        {
            return std::nullopt;
        }

        if (std::memcmp(section.Name, ".text", 5) != 0)
        {
            continue;
        }

        const auto raw_offset = static_cast<std::size_t>(section.PointerToRawData);
        const auto raw_size = static_cast<std::size_t>(section.SizeOfRawData);
        if (raw_offset > file.size() || raw_size > file.size() - raw_offset)
        {
            return std::nullopt;
        }
        return PeText {std::span<const std::uint8_t>(file.data() + raw_offset, raw_size), file_header.TimeDateStamp};
    }
    return std::nullopt;
}

std::size_t CountWearableDefinitionRefreshThunkMatches(
    const std::span<const std::uint8_t> bytes) noexcept
{
    constexpr std::size_t kThunkSize = 27;
    constexpr std::array<std::uint8_t, 9> kPrefix {
        0x40, 0x53,
        0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0xD9};
    constexpr std::array<std::uint8_t, 9> kSuffix {
        0x48, 0x8B, 0xCB,
        0x48, 0x83, 0xC4, 0x20,
        0x5B,
        0xE9};
    constexpr std::array<std::uint8_t, 3> kExpectedBaseNoOp {0xC2, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 10> kExpectedRefreshPrologue {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};

    if (bytes.size() < kThunkSize)
    {
        return 0;
    }

    const auto resolve_relative_target =
        [&](const std::size_t instruction_offset,
            const std::size_t next_instruction_offset,
            std::size_t& target_offset) noexcept
        {
            if (instruction_offset + 5 > bytes.size())
            {
                return false;
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes.data() + instruction_offset + 1, sizeof(displacement));
            const auto signed_target =
                static_cast<std::int64_t>(next_instruction_offset) + displacement;
            if (signed_target < 0 ||
                static_cast<std::uint64_t>(signed_target) >= bytes.size())
            {
                return false;
            }

            target_offset = static_cast<std::size_t>(signed_target);
            return true;
        };

    std::size_t count = 0;
    for (std::size_t offset = 0; offset + kThunkSize <= bytes.size(); ++offset)
    {
        const auto* const thunk = bytes.data() + offset;
        if (!std::equal(kPrefix.begin(), kPrefix.end(), thunk) ||
            thunk[9] != 0xE8 ||
            !std::equal(kSuffix.begin(), kSuffix.end(), thunk + 14))
        {
            continue;
        }

        std::size_t base_target = 0;
        std::size_t refresh_target = 0;
        if (!resolve_relative_target(offset + 9, offset + 14, base_target) ||
            !resolve_relative_target(offset + 22, offset + 27, refresh_target) ||
            base_target + kExpectedBaseNoOp.size() > bytes.size() ||
            refresh_target + kExpectedRefreshPrologue.size() > bytes.size() ||
            !std::equal(
                kExpectedBaseNoOp.begin(),
                kExpectedBaseNoOp.end(),
                bytes.data() + base_target) ||
            !std::equal(
                kExpectedRefreshPrologue.begin(),
                kExpectedRefreshPrologue.end(),
                bytes.data() + refresh_target))
        {
            continue;
        }

        ++count;
    }
    return count;
}

std::map<std::string, std::string> ReadSteamInfo(const std::filesystem::path& path)
{
    std::map<std::string, std::string> values;
    std::ifstream stream(path);
    std::string line;
    while (std::getline(stream, line))
    {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        values.emplace(line.substr(0, equals), line.substr(equals + 1));
    }
    return values;
}

std::wstring WidenAscii(const std::string_view value)
{
    return std::wstring(value.begin(), value.end());
}

std::wstring ModuleVersion(const std::filesystem::path& path)
{
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
    {
        return {};
    }

    std::vector<std::uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
    {
        return {};
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) ||
        info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO))
    {
        return {};
    }

    std::wstringstream stream;
    stream << HIWORD(info->dwFileVersionMS) << L'.' << LOWORD(info->dwFileVersionMS) << L'.'
        << HIWORD(info->dwFileVersionLS) << L'.' << LOWORD(info->dwFileVersionLS);
    return stream.str();
}

std::filesystem::path ModulePath(const HMODULE module)
{
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return {};
    }
    buffer.resize(length);
    return buffer;
}

HMODULE FindHlaeSource2Module() noexcept
{
    if (const auto official = GetModuleHandleW(L"AfxHookSource2.dll"))
    {
        return official;
    }
    return GetModuleHandleW(L"AfxHookSource2.nskinz-test.dll");
}
}

bool nskinzcs2::compatibility::InstalledReport::RequiredSignaturesUnique() const noexcept
{
    return std::all_of(
        signatures.begin(),
        signatures.end(),
        [](const SignatureResult& result)
        {
            return !result.required || (result.module_read && result.match_count == 1);
        });
}

std::size_t nskinzcs2::compatibility::CountPatternMatches(
    const std::span<const std::uint8_t> bytes,
    const std::string_view ida_pattern)
{
    std::vector<int> pattern;
    std::istringstream stream {std::string(ida_pattern)};
    std::string token;
    while (stream >> token)
    {
        pattern.push_back(token == "?" || token == "??" ? -1 : std::stoi(token, nullptr, 16));
    }

    if (pattern.empty() || bytes.size() < pattern.size())
    {
        return 0;
    }

    std::size_t anchor_start = 0;
    std::size_t anchor_length = 0;
    for (std::size_t index = 0; index < pattern.size();)
    {
        if (pattern[index] < 0)
        {
            ++index;
            continue;
        }
        const auto run_start = index;
        while (index < pattern.size() && pattern[index] >= 0)
        {
            ++index;
        }
        if (index - run_start > anchor_length)
        {
            anchor_start = run_start;
            anchor_length = index - run_start;
        }
    }
    if (anchor_length == 0)
    {
        return bytes.size() - pattern.size() + 1;
    }

    std::vector<std::uint8_t> anchor(anchor_length);
    for (std::size_t index = 0; index < anchor_length; ++index)
    {
        anchor[index] = static_cast<std::uint8_t>(pattern[anchor_start + index]);
    }

    std::size_t count = 0;
    auto search_from = bytes.begin();
    const auto anchor_searcher = std::boyer_moore_horspool_searcher(anchor.begin(), anchor.end());
    while (search_from != bytes.end())
    {
        const auto found = std::search(search_from, bytes.end(), anchor_searcher);
        if (found == bytes.end())
        {
            break;
        }
        const auto anchor_offset = static_cast<std::size_t>(found - bytes.begin());
        search_from = found + 1;
        if (anchor_offset < anchor_start)
        {
            continue;
        }
        const auto offset = anchor_offset - anchor_start;
        if (offset + pattern.size() > bytes.size())
        {
            continue;
        }
        bool match = true;
        for (std::size_t index = 0; index < pattern.size(); ++index)
        {
            if (pattern[index] >= 0 && bytes[offset + index] != static_cast<std::uint8_t>(pattern[index]))
            {
                match = false;
                break;
            }
        }
        count += match ? 1u : 0u;
    }
    return count;
}

std::filesystem::path nskinzcs2::compatibility::FindCs2InstallRoot(const std::filesystem::path& starting_path)
{
    auto probe = std::filesystem::is_regular_file(starting_path) ? starting_path.parent_path() : starting_path;
    while (!probe.empty())
    {
        if (std::filesystem::exists(probe / L"game" / L"csgo" / L"steam.inf"))
        {
            return probe;
        }
        const auto parent = probe.parent_path();
        if (parent == probe)
        {
            break;
        }
        probe = parent;
    }
    return {};
}

nskinzcs2::compatibility::InstalledReport nskinzcs2::compatibility::ScanInstalledCs2(
    const std::filesystem::path& requested_root)
{
    InstalledReport report;
    report.install_root = FindCs2InstallRoot(requested_root);
    if (report.install_root.empty() && std::filesystem::exists(requested_root / L"game" / L"csgo" / L"steam.inf"))
    {
        report.install_root = requested_root;
    }

    const auto info = ReadSteamInfo(report.install_root / L"game" / L"csgo" / L"steam.inf");
    const auto value = [&info](const char* key)
    {
        const auto it = info.find(key);
        return it != info.end() ? it->second : std::string {};
    };
    report.client_version = value("ClientVersion");
    report.server_version = value("ServerVersion");
    report.patch_version = value("PatchVersion");
    report.source_revision = value("SourceRevision");
    report.version_date = value("VersionDate");
    report.version_time = value("VersionTime");

    std::map<std::filesystem::path, std::optional<ByteVector>> file_cache;
    std::map<std::filesystem::path, std::optional<PeText>> text_cache;
    for (const auto& spec : kSignatureSpecs)
    {
        SignatureResult result;
        result.name = spec.name;
        result.required = spec.required;
        result.module_path = report.install_root / spec.module_relative_path;

        auto [file_it, inserted] = file_cache.try_emplace(result.module_path, std::nullopt);
        if (inserted)
        {
            file_it->second = ReadBinaryFile(result.module_path);
        }
        if (!file_it->second.has_value())
        {
            result.issue = L"module could not be read";
            report.signatures.push_back(std::move(result));
            continue;
        }

        result.module_read = true;
        std::error_code size_error;
        result.file_size = std::filesystem::file_size(result.module_path, size_error);
        auto [text_it, text_inserted] = text_cache.try_emplace(result.module_path, std::nullopt);
        if (text_inserted)
        {
            text_it->second = GetPeTextSection(*file_it->second);
        }
        if (!text_it->second.has_value())
        {
            result.issue = L"PE .text section could not be read";
            report.signatures.push_back(std::move(result));
            continue;
        }

        result.pe_timestamp = text_it->second->timestamp;
        result.match_count = std::string_view(spec.name) == "wearable_definition_refresh_thunk"
            ? CountWearableDefinitionRefreshThunkMatches(text_it->second->bytes)
            : CountPatternMatches(text_it->second->bytes, spec.ida_pattern);
        if (result.match_count != 1)
        {
            result.issue = result.match_count == 0 ? L"pattern missing" : L"pattern is not unique";
        }
        report.signatures.push_back(std::move(result));
    }
    return report;
}

std::vector<std::wstring> nskinzcs2::compatibility::BuildRuntimeCompatibilityDiagnostics()
{
    std::vector<std::wstring> lines;
    const auto process_path = ModulePath(nullptr);
    const auto report = ScanInstalledCs2(process_path);

    std::wstringstream version;
    version << L"CS2 compatibility baseline: ClientVersion=" << WidenAscii(report.client_version)
        << L", PatchVersion=" << WidenAscii(report.patch_version)
        << L", SourceRevision=" << WidenAscii(report.source_revision)
        << L", VersionDate=" << WidenAscii(report.version_date)
        << L", VersionTime=" << WidenAscii(report.version_time) << L'.';
    lines.push_back(version.str());

    for (const auto& result : report.signatures)
    {
        std::wstringstream line;
        line << L"CS2 signature " << WidenAscii(result.name) << L": module="
            << result.module_path.filename().wstring() << L", matches=" << result.match_count
            << L", size=" << result.file_size << L", pe_timestamp=0x" << std::hex << std::uppercase
            << result.pe_timestamp << std::dec;
        if (!result.issue.empty())
        {
            line << L", issue=" << result.issue;
        }
        line << L'.';
        lines.push_back(line.str());
    }

    const auto hlae_module = FindHlaeSource2Module();
    const auto hlae_path = hlae_module != nullptr ? ModulePath(hlae_module) : std::filesystem::path {};
    std::wstringstream hlae;
    hlae << L"HLAE runtime module: loaded=" << (hlae_module != nullptr ? 1 : 0)
        << L", path=" << (hlae_path.empty() ? L"<unavailable>" : hlae_path.wstring())
        << L", file_version=";
    const auto hlae_version = ModuleVersion(hlae_path);
    hlae << (hlae_version.empty() ? L"<unavailable>" : hlae_version) << L'.';
    lines.push_back(hlae.str());
    return lines;
}

bool nskinzcs2::compatibility::IsHlae21911InterfaceCombination(
    const std::string_view source2_client,
    const std::string_view resource_system,
    const std::string_view file_system) noexcept
{
    return source2_client == kSource2ClientInterface &&
        resource_system == kResourceSystemInterface &&
        file_system == kFileSystemInterface;
}
