#include "Runtime/DemoSkinRewrite.hpp"

#include "Core/Log.hpp"
#include "Cs2/Catalog.hpp"
#include "Resources/ResourceIds.h"
#include "json.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace
{
using json = nlohmann::json;
using nskinzcs2::model::ConfigDocument;
using nskinzcs2::model::GloveProfileMode;
using nskinzcs2::model::KnifeProfileMode;
using nskinzcs2::runtime::DemoSkinRewritePass;
using nskinzcs2::runtime::DemoSkinRewritePlan;
using nskinzcs2::runtime::DemoSkinRewriteResult;

constexpr std::chrono::minutes kRewriteTimeout {10};

struct GeneratedWeaponProfileKey
{
    std::uint64_t xuid = 0;
    int base_item = 0;

    bool operator==(const GeneratedWeaponProfileKey&) const noexcept = default;
};

struct GeneratedWeaponProfileKeyHash
{
    std::size_t operator()(const GeneratedWeaponProfileKey& value) const noexcept
    {
        auto hash = std::hash<std::uint64_t> {}(value.xuid);
        hash ^= std::hash<int> {}(value.base_item) + 0x9E3779B9u + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct GeneratedDemoSkinManifest
{
    std::unordered_set<GeneratedWeaponProfileKey, GeneratedWeaponProfileKeyHash> rewritten_weapons;
    std::unordered_set<std::uint64_t> rewritten_gloves;
};

std::filesystem::path GeneratedDemoSkinManifestPath(const std::filesystem::path& demo)
{
    auto value = demo;
    if (value.is_relative())
    {
        std::wstring process_path(32768, L'\0');
        const auto length = GetModuleFileNameW(
            nullptr,
            process_path.data(),
            static_cast<DWORD>(process_path.size()));
        if (length > 0 && length < process_path.size())
        {
            process_path.resize(length);
            auto executable = std::filesystem::path(process_path);
            if (_wcsicmp(executable.filename().c_str(), L"cs2.exe") == 0)
            {
                auto install_root = executable.parent_path();
                for (int level = 0; level < 3 && !install_root.empty(); ++level)
                {
                    install_root = install_root.parent_path();
                }
                value = install_root / L"game" / L"csgo" / value;
            }
        }
    }
    value += nskinzcs2::runtime::kGeneratedDemoSkinManifestSuffix;
    return value;
}

std::string WideToUtf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const auto size = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const auto size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (size <= 0)
    {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size);
    return result;
}

std::string PathUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

std::optional<std::filesystem::path> ResolveCs2InstallRoot(const DWORD process_id)
{
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr)
    {
        return std::nullopt;
    }
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
    CloseHandle(process);
    if (!queried || size == 0)
    {
        return std::nullopt;
    }
    path.resize(size);
    auto executable = std::filesystem::path(path);
    if (_wcsicmp(executable.filename().c_str(), L"cs2.exe") != 0)
    {
        return std::nullopt;
    }
    auto root = executable.parent_path();
    for (int level = 0; level < 3 && !root.empty(); ++level)
    {
        root = root.parent_path();
    }
    if (root.empty())
    {
        return std::nullopt;
    }
    return root;
}

bool HasDemoMagic(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::array<char, 8> magic {};
    return stream.read(magic.data(), static_cast<std::streamsize>(magic.size())) &&
        std::string_view(magic.data(), magic.size()) == std::string_view("PBDEMS2\0", 8);
}

std::optional<std::filesystem::path> ResolveSourceDemo(
    const std::filesystem::path& captured,
    const std::filesystem::path& cs2_root)
{
    std::vector<std::filesystem::path> candidates;
    if (captured.is_absolute())
    {
        candidates.push_back(captured);
    }
    else
    {
        candidates.push_back(std::filesystem::absolute(captured));
        candidates.push_back(cs2_root / L"game" / L"csgo" / captured);
        candidates.push_back(cs2_root / captured);
    }
    for (auto candidate : candidates)
    {
        std::error_code error;
        candidate = std::filesystem::absolute(candidate, error).lexically_normal();
        if (!error && std::filesystem::is_regular_file(candidate, error) && HasDemoMagic(candidate))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

std::wstring SanitizeStem(std::wstring stem)
{
    std::wstring output;
    output.reserve(std::min<std::size_t>(stem.size(), 48));
    for (const auto character : stem)
    {
        if (output.size() >= 48) break;
        if ((character >= L'a' && character <= L'z') ||
            (character >= L'A' && character <= L'Z') ||
            (character >= L'0' && character <= L'9') ||
            character == L'_' || character == L'-')
        {
            output.push_back(character);
        }
        else if (!output.empty() && output.back() != L'_')
        {
            output.push_back(L'_');
        }
    }
    if (output.empty()) output = L"demo";
    return output;
}

std::wstring QuoteWindowsArgument(const std::wstring_view argument)
{
    std::wstring result(1, L'"');
    std::size_t backslashes = 0;
    for (const auto character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::string ReadTextFileLimited(const std::filesystem::path& path, const std::size_t limit)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::string result;
    result.resize(limit);
    stream.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(stream.gcount()));
    return result;
}

json ParseItemJson(const std::string& text)
{
    return json::parse(text);
}

json BuildRequestJson(
    const DemoSkinRewritePlan& plan,
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const std::filesystem::path& cs2_game_directory)
{
    json passes = json::array();
    for (const auto& pass : plan.passes)
    {
        json items = json::array();
        for (const auto& item : pass.item_json)
        {
            items.push_back(ParseItemJson(item));
        }
        passes.push_back({
            {"steam_id64", std::to_string(pass.xuid)},
            {"items", std::move(items)},
        });
    }
    return {
        {"schema_version", 1},
        {"input_dem", PathUtf8(input)},
        {"output_dem", PathUtf8(output)},
        {"cs2_game_dir", PathUtf8(cs2_game_directory)},
        {"passes", std::move(passes)},
    };
}

bool ExtractEmbeddedDemoRewriter(
    std::filesystem::path& destination,
    std::wstring& issue)
{
    issue.clear();
    destination.clear();
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ExtractEmbeddedDemoRewriter),
            &module))
    {
        issue = L"Could not resolve the nSkinz module handle (Win32 " +
            std::to_wstring(GetLastError()) + L").";
        return false;
    }
    const auto resource = FindResourceW(
        module,
        MAKEINTRESOURCEW(IDR_NSKINZ_DEMO_REWRITER),
        RT_RCDATA);
    if (resource == nullptr)
    {
        issue = L"The embedded nSkinz demo rewriter resource is missing.";
        return false;
    }
    const auto size = SizeofResource(module, resource);
    const auto loaded = LoadResource(module, resource);
    const auto* bytes = loaded == nullptr ? nullptr : LockResource(loaded);
    if (size == 0 || bytes == nullptr)
    {
        issue = L"The embedded nSkinz demo rewriter resource is invalid.";
        return false;
    }

    std::uint64_t fingerprint = 1469598103934665603ull;
    const auto* resource_bytes = static_cast<const unsigned char*>(bytes);
    for (DWORD index = 0; index < size; ++index)
    {
        fingerprint ^= resource_bytes[index];
        fingerprint *= 1099511628211ull;
    }
    std::error_code error;
    const auto tool_directory = std::filesystem::temp_directory_path() /
        L"nSkinzCS2" / L"tools";
    std::filesystem::create_directories(tool_directory, error);
    if (error)
    {
        issue = L"Could not create the embedded rewriter cache directory.";
        return false;
    }
    destination = tool_directory /
        (L"nskinz-demo-rewriter-" + std::to_wstring(size) + L"-" +
            std::to_wstring(fingerprint) + L".exe");
    const auto existing_size = std::filesystem::file_size(destination, error);
    if (!error && existing_size == size)
    {
        return true;
    }
    error.clear();
    auto temporary = destination;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        issue = L"Could not extract the embedded nSkinz demo rewriter.";
        return false;
    }
    stream.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    if (!stream)
    {
        issue = L"Writing the embedded nSkinz demo rewriter failed.";
        return false;
    }
    stream.close();
    if (!MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        issue = L"Could not publish the cached embedded demo rewriter (Win32 " +
            std::to_wstring(GetLastError()) + L").";
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool RunNskinDemoRewriter(
    const std::filesystem::path& request_path,
    const std::filesystem::path& response_path,
    const std::filesystem::path& log_path,
    std::wstring& issue)
{
    issue.clear();
    std::filesystem::path executable;
    if (!ExtractEmbeddedDemoRewriter(executable, issue))
    {
        return false;
    }
    std::wstring command = QuoteWindowsArgument(executable.wstring()) +
        L" --request " + QuoteWindowsArgument(request_path.wstring()) +
        L" --response " + QuoteWindowsArgument(response_path.wstring());
    command.push_back(L'\0');

    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    const auto log = CreateFileW(
        log_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &attributes,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (log == INVALID_HANDLE_VALUE)
    {
        issue = L"Could not create the temporary rewrite log.";
        return false;
    }
    const auto null_input = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (null_input == INVALID_HANDLE_VALUE)
    {
        CloseHandle(log);
        issue = L"Could not open the null input handle for the nSkinz demo rewriter.";
        return false;
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process {};
    const auto created = CreateProcessW(
        executable.c_str(),
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        request_path.parent_path().c_str(),
        &startup,
        &process);
    const auto create_error = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(null_input);
    CloseHandle(log);
    if (!created)
    {
        issue = L"Failed to start the embedded nSkinz demo rewriter (Win32 " +
            std::to_wstring(create_error) + L").";
        return false;
    }
    CloseHandle(process.hThread);

    const auto wait = WaitForSingleObject(
        process.hProcess,
        static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kRewriteTimeout).count()));
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        issue = L"Temporary demo rewrite timed out after 10 minutes.";
        return false;
    }
    if (wait == WAIT_FAILED)
    {
        const auto wait_error = GetLastError();
        CloseHandle(process.hProcess);
        issue = L"Waiting for the nSkinz demo rewriter failed (Win32 " +
            std::to_wstring(wait_error) + L").";
        return false;
    }
    DWORD exit_code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exit_code != 0)
    {
        const auto response = ReadTextFileLimited(response_path, 32768);
        const auto log_tail = ReadTextFileLimited(log_path, 32768);
        std::wstring detail;
        try
        {
            const auto parsed = json::parse(response);
            detail = Utf8ToWide(parsed.value("error", std::string {}));
        }
        catch (...)
        {
        }
        if (detail.empty()) detail = Utf8ToWide(log_tail);
        issue = L"nSkinz demo rewriter failed (exit " + std::to_wstring(exit_code) + L")";
        if (!detail.empty()) issue += L": " + detail.substr(0, 1000);
        return false;
    }
    return true;
}

json BuildBatchItem(
    const int source_definition,
    const int target_definition,
    const int paint_kit,
    const int quality,
    const int seed,
    const float wear,
    const char* team,
    const bool preserve_source_definition = false,
    const std::optional<std::uint64_t> mesh_group_mask = std::nullopt,
    const std::string_view custom_name = {},
    const std::array<nskinzcs2::model::StickerProfile, 5>* stickers = nullptr)
{
    json item {
        {"item_id64", "0"},
        {"definition_index", source_definition},
        {"paint_kit", paint_kit},
        {"quality", quality},
        {"pattern_seed", static_cast<double>(seed)},
        {"wear", static_cast<double>(wear)},
        {"team", team},
    };
    if (target_definition != source_definition)
    {
        item["replacement_definition_index"] = target_definition;
    }
    if (preserve_source_definition)
    {
        item["preserve_source_definition"] = true;
    }
    if (mesh_group_mask.has_value())
    {
        item["mesh_group_mask"] = *mesh_group_mask;
    }
    item["custom_name"] = nskinzcs2::model::ResolveCosmeticCustomName(custom_name);
    if (stickers != nullptr)
    {
        item["stickers"] = json::array();
        for (const auto& sticker : *stickers)
        {
            item["stickers"].push_back(std::max(0, sticker.kit_id));
        }
    }
    return item;
}

std::optional<std::uint64_t> ResolveRebuiltWeaponMeshGroupMask(
    const nskinzcs2::model::Catalog* catalog,
    const int base_item,
    const int paint_kit) noexcept
{
    // Every standard firearm can carry a legacy/modern model selection in
    // paint-kit metadata. The demo writer must serialize that selection just
    // like the live runtime does; otherwise a reconstructed finish can be
    // rendered against the wrong mesh family. Knives are routed separately.
    if (base_item <= 0 || base_item >= 500 || base_item == 42 || base_item == 59 ||
        paint_kit <= 0)
    {
        return std::nullopt;
    }
    if (catalog != nullptr)
    {
        if (const auto found = catalog->weapon_paint_uses_legacy_model.find(paint_kit);
            found != catalog->weapon_paint_uses_legacy_model.end())
        {
            return found->second ? std::uint64_t {2} : std::uint64_t {1};
        }
        return std::nullopt;
    }
    return std::uint64_t {2};
}

void AddPassItem(
    std::map<std::uint64_t, DemoSkinRewritePass>& passes,
    const std::uint64_t xuid,
    const json& item)
{
    auto& pass = passes[xuid];
    pass.xuid = xuid;
    pass.item_json.push_back(item.dump());
}

bool WriteGeneratedDemoSkinManifest(
    const std::filesystem::path& output_demo,
    const ConfigDocument& config,
    const std::uint64_t painted_weapon_passthroughs,
    const std::uint64_t painted_glove_passthroughs,
    std::wstring& issue)
{
    json rewritten_weapons = json::array();
    if (painted_weapon_passthroughs == 0)
    {
        for (const auto& profile : config.profiles)
        {
            if (!profile.enabled || profile.xuid == 0 || profile.base_item <= 0 ||
                profile.base_item >= 500 || profile.base_item == 42 || profile.base_item == 59 ||
                profile.paint_kit <= 0)
            {
                continue;
            }
            rewritten_weapons.push_back({
                {"xuid", std::to_string(profile.xuid)},
                {"base_item", profile.base_item},
            });
        }
    }

    json rewritten_gloves = json::array();
    if (painted_glove_passthroughs == 0)
    {
        for (const auto& profile : config.glove_profiles)
        {
            if (!profile.enabled || profile.xuid == 0 || profile.paint_kit <= 0 ||
                (profile.mode == GloveProfileMode::ModelAndFinish && profile.model_definition <= 0))
            {
                continue;
            }
            rewritten_gloves.push_back(std::to_string(profile.xuid));
        }
    }

    const json manifest {
        {"schema_version", 1},
        {"output_demo", PathUtf8(output_demo)},
        {"rewritten_weapon_profiles", std::move(rewritten_weapons)},
        {"rewritten_glove_xuids", std::move(rewritten_gloves)},
        {"painted_weapon_passthrough_count", painted_weapon_passthroughs},
        {"painted_glove_passthrough_count", painted_glove_passthroughs},
    };
    const auto destination = GeneratedDemoSkinManifestPath(output_demo);
    auto temporary = destination;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            issue = L"Could not create the generated-demo skin manifest.";
            return false;
        }
        const auto serialized = manifest.dump();
        stream.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        if (!stream)
        {
            issue = L"Could not write the generated-demo skin manifest.";
            return false;
        }
    }
    if (!MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        issue = L"Could not publish the generated-demo skin manifest (Win32 " +
            std::to_wstring(GetLastError()) + L").";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::optional<GeneratedDemoSkinManifest> LoadGeneratedDemoSkinManifest(
    const std::filesystem::path& demo)
{
    const auto text = ReadTextFileLimited(GeneratedDemoSkinManifestPath(demo), 1024 * 1024);
    if (text.empty()) return std::nullopt;
    const auto document = json::parse(text);
    if (document.value("schema_version", 0) != 1) return std::nullopt;

    GeneratedDemoSkinManifest result;
    for (const auto& value : document.value("rewritten_weapon_profiles", json::array()))
    {
        const auto xuid_text = value.value("xuid", std::string {});
        const auto base_item = value.value("base_item", 0);
        if (xuid_text.empty() || base_item <= 0) continue;
        result.rewritten_weapons.insert({std::stoull(xuid_text), base_item});
    }
    for (const auto& value : document.value("rewritten_glove_xuids", json::array()))
    {
        if (!value.is_string()) continue;
        result.rewritten_gloves.insert(std::stoull(value.get<std::string>()));
    }
    return result;
}

std::mutex g_generated_manifest_mutex;
std::filesystem::path g_cached_generated_manifest_demo;
std::optional<GeneratedDemoSkinManifest> g_cached_generated_manifest;

void RefreshGeneratedDemoSkinManifestCacheLocked(const std::filesystem::path& demo)
{
    if (demo == g_cached_generated_manifest_demo) return;
    g_cached_generated_manifest_demo = demo;
    try
    {
        g_cached_generated_manifest = LoadGeneratedDemoSkinManifest(demo);
    }
    catch (...)
    {
        g_cached_generated_manifest.reset();
    }
}
}

bool nskinzcs2::runtime::PublishCurrentDemoPathSidecar(
    const std::filesystem::path& dll_directory,
    const std::filesystem::path& demo_path,
    std::wstring& issue)
{
    issue.clear();
    if (dll_directory.empty())
    {
        issue = L"DLL directory is empty.";
        return false;
    }
    const auto destination = dll_directory / kCurrentDemoPathSidecarName;
    const auto temporary = dll_directory / L"nSkinzCS2.current-demo.tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            issue = L"Could not write the current-demo sidecar.";
            return false;
        }
        const auto encoded = PathUtf8(demo_path);
        stream.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        stream.put('\n');
    }
    if (!MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        issue = L"Could not publish the current-demo sidecar (Win32 " +
            std::to_wstring(GetLastError()) + L").";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> nskinzcs2::runtime::ReadCurrentDemoPathSidecar(
    const std::filesystem::path& dll_directory)
{
    const auto text = ReadTextFileLimited(dll_directory / kCurrentDemoPathSidecarName, 32768);
    if (text.empty()) return std::nullopt;
    auto line = text.substr(0, text.find_first_of("\r\n"));
    if (line.empty()) return std::nullopt;
    return std::filesystem::path(Utf8ToWide(line));
}

bool nskinzcs2::runtime::IsGeneratedDemoSkinPath(const std::filesystem::path& path) noexcept
{
    auto filename = path.filename().wstring();
    const auto lower = [](const wchar_t character)
    {
        return static_cast<wchar_t>(std::towlower(character));
    };
    std::transform(filename.begin(), filename.end(), filename.begin(), lower);
    std::wstring prefix = kGeneratedDemoFilePrefix;
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), lower);
    return filename.starts_with(prefix);
}

bool nskinzcs2::runtime::IsGeneratedDemoRewrittenWeaponProfile(
    const std::filesystem::path& generated_demo,
    const std::uint64_t xuid,
    const int base_item) noexcept
{
    if (xuid == 0 || base_item <= 0 || !IsGeneratedDemoSkinPath(generated_demo)) return false;
    std::scoped_lock lock(g_generated_manifest_mutex);
    RefreshGeneratedDemoSkinManifestCacheLocked(generated_demo);
    return g_cached_generated_manifest.has_value() &&
        g_cached_generated_manifest->rewritten_weapons.contains({xuid, base_item});
}

bool nskinzcs2::runtime::ShouldSuppressGeneratedDemoGloveProfile(
    const std::filesystem::path& generated_demo,
    const std::uint64_t xuid) noexcept
{
    if (xuid == 0 || !IsGeneratedDemoSkinPath(generated_demo)) return false;
    std::scoped_lock lock(g_generated_manifest_mutex);
    RefreshGeneratedDemoSkinManifestCacheLocked(generated_demo);
    return g_cached_generated_manifest.has_value() &&
        g_cached_generated_manifest->rewritten_gloves.contains(xuid);
}

DemoSkinRewritePlan nskinzcs2::runtime::BuildDefaultDemoSkinRewritePlan(
    const ConfigDocument& config,
    const model::Catalog* catalog)
{
    std::map<std::uint64_t, DemoSkinRewritePass> grouped;
    DemoSkinRewritePlan result;

    for (const auto& profile : config.profiles)
    {
        const auto sticker_slot_count = static_cast<int>(std::count_if(
            profile.stickers.begin(),
            profile.stickers.end(),
            [](const auto& sticker) { return sticker.kit_id > 0; }));
        if (!profile.enabled || profile.xuid == 0 || profile.base_item <= 0 ||
            profile.base_item >= 500 || profile.base_item == 42 || profile.base_item == 59 ||
            (profile.paint_kit <= 0 && sticker_slot_count == 0))
        {
            continue;
        }
        if (catalog != nullptr && profile.paint_kit > 0 &&
            CatalogBuilder::CheckWeaponPaintCompatibility(
                catalog,
                profile.base_item,
                profile.paint_kit) != WeaponPaintCompatibility::Compatible)
        {
            result.validation_issue = L"Weapon paint compatibility rejected definition " +
                std::to_wstring(profile.base_item) + L" with paint kit " +
                std::to_wstring(profile.paint_kit) + L".";
            return result;
        }
        const auto weapon_mesh_group_mask = profile.paint_kit > 0
            ? ResolveRebuiltWeaponMeshGroupMask(catalog, profile.base_item, profile.paint_kit)
            : std::nullopt;
        if (catalog != nullptr && profile.paint_kit > 0 && !weapon_mesh_group_mask.has_value())
        {
            result.validation_issue = L"Weapon paint mesh metadata is unavailable for paint kit " +
                std::to_wstring(profile.paint_kit) + L".";
            return result;
        }
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            profile.base_item,
            profile.base_item,
            profile.paint_kit,
            profile.quality,
            model::NormalizeCosmeticSeed(profile.seed),
            model::NormalizeCosmeticWear(profile.wear),
            "ANY",
            false,
            weapon_mesh_group_mask,
            profile.custom_name,
            &profile.stickers));
        ++result.weapon_item_count;
        result.sticker_slot_request_count += sticker_slot_count;
    }

    for (const auto& profile : config.knife_profiles)
    {
        if (!profile.enabled || profile.xuid == 0 ||
            profile.mode != KnifeProfileMode::ModelAndFinish ||
            profile.model_definition <= 0)
        {
            continue;
        }
        if (catalog != nullptr && profile.paint_kit > 0 &&
            CatalogBuilder::CheckWeaponPaintCompatibility(
                catalog,
                profile.model_definition,
                profile.paint_kit) != WeaponPaintCompatibility::Compatible)
        {
            result.validation_issue = L"Knife paint compatibility rejected definition " +
                std::to_wstring(profile.model_definition) + L" with paint kit " +
                std::to_wstring(profile.paint_kit) + L".";
            return result;
        }
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            59,
            profile.model_definition,
            profile.paint_kit,
            3,
            model::NormalizeCosmeticSeed(profile.seed),
            model::NormalizeCosmeticWear(profile.wear),
            "T",
            false,
            std::nullopt,
            profile.custom_name));
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            42,
            profile.model_definition,
            profile.paint_kit,
            3,
            model::NormalizeCosmeticSeed(profile.seed),
            model::NormalizeCosmeticWear(profile.wear),
            "CT",
            false,
            std::nullopt,
            profile.custom_name));
        result.knife_item_count += 2;
    }

    for (const auto& profile : config.glove_profiles)
    {
        if (!profile.enabled || profile.xuid == 0 || profile.paint_kit <= 0 ||
            (profile.mode == GloveProfileMode::ModelAndFinish && profile.model_definition <= 0))
        {
            continue;
        }
        if (catalog != nullptr && profile.mode == GloveProfileMode::ModelAndFinish)
        {
            const auto compatible = catalog->glove_paint_compatibility.find(profile.paint_kit);
            if (compatible == catalog->glove_paint_compatibility.end() ||
                compatible->second != profile.model_definition)
            {
                result.validation_issue = L"Glove paint compatibility rejected definition " +
                    std::to_wstring(profile.model_definition) + L" with paint kit " +
                    std::to_wstring(profile.paint_kit) + L".";
                return result;
            }
        }
        const bool preserve_source_definition = profile.mode == GloveProfileMode::FinishOnly;
        const int t_target_definition = preserve_source_definition ? 5028 : profile.model_definition;
        const int ct_target_definition = preserve_source_definition ? 5029 : profile.model_definition;
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            5028,
            t_target_definition,
            profile.paint_kit,
            profile.quality,
            model::NormalizeCosmeticSeed(profile.seed),
            model::NormalizeCosmeticWear(profile.wear),
            "T",
            preserve_source_definition,
            std::nullopt,
            profile.custom_name));
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            5029,
            ct_target_definition,
            profile.paint_kit,
            profile.quality,
            model::NormalizeCosmeticSeed(profile.seed),
            model::NormalizeCosmeticWear(profile.wear),
            "CT",
            preserve_source_definition,
            std::nullopt,
            profile.custom_name));
        result.glove_item_count += 2;
    }

    for (auto& [xuid, pass] : grouped)
    {
        static_cast<void>(xuid);
        result.passes.push_back(std::move(pass));
    }
    return result;
}

DemoSkinRewriteResult nskinzcs2::runtime::GenerateTemporarySkinnedDemo(
    const ConfigDocument& config,
    const model::Catalog* catalog,
    const std::filesystem::path& captured_source_demo,
    const DWORD cs2_process_id)
{
    DemoSkinRewriteResult result;
    const auto plan = BuildDefaultDemoSkinRewritePlan(config, catalog);
    if (!plan.validation_issue.empty())
    {
        result.detail = plan.validation_issue;
        return result;
    }
    if (plan.passes.empty())
    {
        result.detail =
            L"No eligible firearm/glove finish or model_and_finish knife profile with a non-zero XUID was found.";
        return result;
    }
    const auto cs2_root = ResolveCs2InstallRoot(cs2_process_id);
    if (!cs2_root)
    {
        result.detail = L"The running CS2 installation root could not be resolved.";
        return result;
    }
    const auto source = ResolveSourceDemo(captured_source_demo, *cs2_root);
    if (!source)
    {
        result.detail = L"The captured original demo path is missing or is not a PBDEMS2 file: " +
            captured_source_demo.wstring();
        return result;
    }
    if (IsGeneratedDemoSkinPath(*source))
    {
        result.detail = L"Refusing to rewrite an nSkinz-generated demo. Load the original demo first.";
        return result;
    }

    const auto output_directory = *cs2_root / L"game" / L"csgo" / kGeneratedDemoDirectoryName;
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    if (error)
    {
        result.detail = L"Could not create the CS2 temporary demo directory: " + output_directory.wstring();
        return result;
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto filename = std::wstring(kGeneratedDemoFilePrefix) +
        SanitizeStem(source->stem().wstring()) + L"_" + std::to_wstring(timestamp) + L".dem";
    const auto output = output_directory / filename;

    const auto job_directory = std::filesystem::temp_directory_path() /
        L"nSkinzCS2" /
        (L"skin-rewrite-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(job_directory, error);
    if (error)
    {
        result.detail = L"Could not create the temporary rewrite job directory.";
        return result;
    }
    const auto request_path = job_directory / L"request.json";
    const auto response_path = job_directory / L"response.json";
    const auto log_path = job_directory / L"rewriter.log";
    {
        std::ofstream stream(request_path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            result.detail = L"Could not create the temporary rewrite request.";
            return result;
        }
        const auto request = BuildRequestJson(
            plan,
            *source,
            output,
            *cs2_root / L"game" / L"csgo").dump();
        stream.write(request.data(), static_cast<std::streamsize>(request.size()));
    }

    Logger::Info(
        L"Temporary demo cosmetics rewrite started: source=" + source->wstring() +
        L", output=" + output.wstring() +
        L", passes=" + std::to_wstring(plan.passes.size()) +
        L", weapon_items=" + std::to_wstring(plan.weapon_item_count) +
        L", knife_items=" + std::to_wstring(plan.knife_item_count) +
        L", glove_items=" + std::to_wstring(plan.glove_item_count) + L".");

    std::wstring issue;
    if (!RunNskinDemoRewriter(request_path, response_path, log_path, issue))
    {
        result.detail = std::move(issue);
        Logger::Error(L"Temporary demo cosmetics rewrite failed: " + result.detail);
        return result;
    }
    std::uint64_t rewritten_pawns = 0;
    std::uint64_t matched_players = 0;
    std::uint64_t skipped_players = 0;
    std::uint64_t matched_knife_requests = 0;
    std::uint64_t matched_glove_requests = 0;
    std::uint64_t matched_sticker_slot_requests = 0;
    std::uint64_t rewritten_knives = 0;
    std::uint64_t rewritten_gloves = 0;
    std::uint64_t rewritten_weapons = 0;
    std::uint64_t painted_glove_passthroughs = 0;
    std::uint64_t painted_weapon_passthroughs = 0;
    std::uint64_t rewritten_sticker_weapons = 0;
    std::uint64_t rewritten_sticker_slots = 0;
    std::uint64_t missing_sticker_slots = 0;
    std::uint64_t inserted_animation_resources = 0;
    std::wstring animation_resource_source;
    std::uint64_t input_read_ms = 0;
    std::uint64_t discovery_ms = 0;
    std::uint64_t rewrite_ms = 0;
    std::uint64_t verification_ms = 0;
    std::uint64_t total_ms = 0;
    std::wstring input_mode;
    try
    {
        const auto response = json::parse(ReadTextFileLimited(response_path, 1024 * 1024));
        if (response.value("ok", false) != true)
        {
            result.detail = Utf8ToWide(response.value("error", std::string("Unknown nSkinz rewrite failure.")));
            return result;
        }
        rewritten_pawns = response.value("pawn_count", std::uint64_t {0});
        matched_players = response.value(
            "matched_player_count", static_cast<std::uint64_t>(plan.passes.size()));
        skipped_players = response.value("skipped_player_count", std::uint64_t {0});
        matched_knife_requests = response.value(
            "matched_knife_request_count",
            plan.knife_item_count > 0 ? std::uint64_t {1} : std::uint64_t {0});
        matched_glove_requests = response.value(
            "matched_glove_request_count",
            plan.glove_item_count > 0 ? std::uint64_t {1} : std::uint64_t {0});
        matched_sticker_slot_requests = response.value(
            "requested_sticker_slot_count",
            static_cast<std::uint64_t>(plan.sticker_slot_request_count));
        rewritten_knives = response.value("knife_count", std::uint64_t {0});
        rewritten_gloves = response.value("glove_count", std::uint64_t {0});
        rewritten_weapons = response.value("weapon_count", std::uint64_t {0});
        painted_glove_passthroughs =
            response.value("painted_glove_passthrough_count", std::uint64_t {0});
        painted_weapon_passthroughs =
            response.value("painted_weapon_passthrough_count", std::uint64_t {0});
        rewritten_sticker_weapons =
            response.value("sticker_weapon_count", std::uint64_t {0});
        rewritten_sticker_slots =
            response.value("sticker_slot_count", std::uint64_t {0});
        missing_sticker_slots =
            response.value("missing_sticker_slot_count", std::uint64_t {0});
        inserted_animation_resources =
            response.value("animation_resource_count", std::uint64_t {0});
        animation_resource_source = Utf8ToWide(
            response.value("animation_resource_source", std::string {}));
        input_read_ms = response.value("input_read_ms", std::uint64_t {0});
        discovery_ms = response.value("discovery_ms", std::uint64_t {0});
        rewrite_ms = response.value("rewrite_ms", std::uint64_t {0});
        verification_ms = response.value("verification_ms", std::uint64_t {0});
        total_ms = response.value("total_ms", std::uint64_t {0});
        input_mode = Utf8ToWide(response.value("input_mode", std::string {}));
    }
    catch (const std::exception& exception)
    {
        result.detail = L"The nSkinz demo rewriter response was invalid: " + Utf8ToWide(exception.what());
        return result;
    }
    if ((matched_knife_requests > 0 && rewritten_knives == 0) ||
        (matched_glove_requests > 0 && rewritten_gloves + painted_glove_passthroughs == 0))
    {
        result.detail = L"The nSkinz demo rewriter completed but did not resolve every requested cosmetic class: "
            L"pawns=" + std::to_wstring(rewritten_pawns) +
            L", knives=" + std::to_wstring(rewritten_knives) +
            L", gloves=" + std::to_wstring(rewritten_gloves) +
            L", painted_glove_passthroughs=" + std::to_wstring(painted_glove_passthroughs) + L".";
        Logger::Error(result.detail);
        return result;
    }
    if (matched_sticker_slot_requests > 0 && rewritten_sticker_slots == 0)
    {
        result.detail = L"The selected weapons in this demo do not contain any of the requested existing sticker slots. "
            L"Existing-slot replacement cannot create a new sticker; choose a weapon that already has a sticker in that slot. "
            L"Missing slots=" + std::to_wstring(missing_sticker_slots) + L".";
        Logger::Error(result.detail);
        return result;
    }
    if (!std::filesystem::is_regular_file(output, error) || error || !HasDemoMagic(output))
    {
        result.detail = L"The rewrite process returned success but did not produce a valid PBDEMS2 file.";
        return result;
    }
    std::wstring manifest_issue;
    if (!WriteGeneratedDemoSkinManifest(
            output,
            config,
            painted_weapon_passthroughs,
            painted_glove_passthroughs,
            manifest_issue))
    {
        result.detail = std::move(manifest_issue);
        Logger::Error(L"Temporary demo cosmetics rewrite manifest failed: " + result.detail);
        return result;
    }

    result.success = true;
    result.source_demo = *source;
    result.output_demo = output;
    result.playdemo_command = "playdemo \"" +
        WideToUtf8(std::wstring(kGeneratedDemoDirectoryName)) + "/" +
        WideToUtf8(filename) + "\"";
    result.detail = L"Temporary skinned demo is ready: " + output.wstring() +
        L" (matched_players=" + std::to_wstring(matched_players) +
        L", skipped_players=" + std::to_wstring(skipped_players) +
        L", pawns=" + std::to_wstring(rewritten_pawns) +
        L", weapons=" + std::to_wstring(rewritten_weapons) +
        L", painted_weapon_passthroughs=" + std::to_wstring(painted_weapon_passthroughs) +
        L", sticker_weapons=" + std::to_wstring(rewritten_sticker_weapons) +
        L", sticker_slots=" + std::to_wstring(rewritten_sticker_slots) +
        L", missing_sticker_slots=" + std::to_wstring(missing_sticker_slots) +
        L", knives=" + std::to_wstring(rewritten_knives) +
        L", gloves=" + std::to_wstring(rewritten_gloves) +
        L", painted_glove_passthroughs=" + std::to_wstring(painted_glove_passthroughs) +
        L", animation_resources=" + std::to_wstring(inserted_animation_resources) +
        L", animation_resource_source=" + animation_resource_source +
        L", mode=" + input_mode +
        L", read_ms=" + std::to_wstring(input_read_ms) +
        L", discovery_ms=" + std::to_wstring(discovery_ms) +
        L", rewrite_ms=" + std::to_wstring(rewrite_ms) +
        L", verification_ms=" + std::to_wstring(verification_ms) +
        L", total_ms=" + std::to_wstring(total_ms) + L")";
    Logger::Info(result.detail + L"; queued command=" + Utf8ToWide(result.playdemo_command) + L".");
    return result;
}
