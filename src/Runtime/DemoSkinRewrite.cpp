#include "Runtime/DemoSkinRewrite.hpp"

#include "Core/Log.hpp"
#include "Resources/ResourceIds.h"
#include "json.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

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
    const std::filesystem::path& output)
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
        {"passes", std::move(passes)},
    };
}

bool ExtractEmbeddedDemoRewriter(
    const std::filesystem::path& destination,
    std::wstring& issue)
{
    issue.clear();
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
    std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
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
    return true;
}

bool RunNskinDemoRewriter(
    const std::filesystem::path& request_path,
    const std::filesystem::path& response_path,
    const std::filesystem::path& log_path,
    std::wstring& issue)
{
    issue.clear();
    const auto executable = request_path.parent_path() / L"nskinz-demo-rewriter.exe";
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
    const int seed,
    const float wear,
    const char* team)
{
    json item {
        {"item_id64", "0"},
        {"definition_index", source_definition},
        {"paint_kit", paint_kit},
        {"pattern_seed", static_cast<double>(seed)},
        {"wear", static_cast<double>(wear)},
        {"team", team},
    };
    if (target_definition != source_definition)
    {
        item["replacement_definition_index"] = target_definition;
    }
    return item;
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

DemoSkinRewritePlan nskinzcs2::runtime::BuildDefaultDemoSkinRewritePlan(
    const ConfigDocument& config)
{
    std::map<std::uint64_t, DemoSkinRewritePass> grouped;
    DemoSkinRewritePlan result;

    for (const auto& profile : config.knife_profiles)
    {
        if (!profile.enabled || profile.xuid == 0 ||
            profile.mode != KnifeProfileMode::ModelAndFinish ||
            profile.model_definition <= 0)
        {
            continue;
        }
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            59,
            profile.model_definition,
            profile.paint_kit,
            profile.seed,
            model::NormalizeCosmeticWear(profile.wear),
            "T"));
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            42,
            profile.model_definition,
            profile.paint_kit,
            profile.seed,
            model::NormalizeCosmeticWear(profile.wear),
            "CT"));
        result.knife_item_count += 2;
    }

    for (const auto& profile : config.glove_profiles)
    {
        if (!profile.enabled || profile.xuid == 0 ||
            profile.mode != GloveProfileMode::ModelAndFinish ||
            profile.model_definition <= 0)
        {
            continue;
        }
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            5028,
            profile.model_definition,
            profile.paint_kit,
            profile.seed,
            model::NormalizeCosmeticWear(profile.wear),
            "T"));
        AddPassItem(grouped, profile.xuid, BuildBatchItem(
            5029,
            profile.model_definition,
            profile.paint_kit,
            profile.seed,
            model::NormalizeCosmeticWear(profile.wear),
            "CT"));
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
    const std::filesystem::path& captured_source_demo,
    const DWORD cs2_process_id)
{
    DemoSkinRewriteResult result;
    const auto plan = BuildDefaultDemoSkinRewritePlan(config);
    if (plan.passes.empty())
    {
        result.detail =
            L"No enabled model_and_finish knife/glove profile with a non-zero XUID and target model was found.";
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
        const auto request = BuildRequestJson(plan, *source, output).dump();
        stream.write(request.data(), static_cast<std::streamsize>(request.size()));
    }

    Logger::Info(
        L"Temporary demo cosmetics rewrite started: source=" + source->wstring() +
        L", output=" + output.wstring() +
        L", passes=" + std::to_wstring(plan.passes.size()) +
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
    std::uint64_t rewritten_knives = 0;
    std::uint64_t rewritten_gloves = 0;
    std::uint64_t inserted_animation_resources = 0;
    try
    {
        const auto response = json::parse(ReadTextFileLimited(response_path, 1024 * 1024));
        if (response.value("ok", false) != true)
        {
            result.detail = Utf8ToWide(response.value("error", std::string("Unknown nSkinz rewrite failure.")));
            return result;
        }
        rewritten_pawns = response.value("pawn_count", std::uint64_t {0});
        rewritten_knives = response.value("knife_count", std::uint64_t {0});
        rewritten_gloves = response.value("glove_count", std::uint64_t {0});
        inserted_animation_resources =
            response.value("animation_resource_count", std::uint64_t {0});
    }
    catch (const std::exception& exception)
    {
        result.detail = L"The nSkinz demo rewriter response was invalid: " + Utf8ToWide(exception.what());
        return result;
    }
    if ((plan.knife_item_count > 0 && rewritten_knives == 0) ||
        (plan.glove_item_count > 0 && rewritten_gloves == 0))
    {
        result.detail = L"The nSkinz demo rewriter completed but did not resolve every requested cosmetic class: "
            L"pawns=" + std::to_wstring(rewritten_pawns) +
            L", knives=" + std::to_wstring(rewritten_knives) +
            L", gloves=" + std::to_wstring(rewritten_gloves) + L".";
        Logger::Error(result.detail);
        return result;
    }
    if (!std::filesystem::is_regular_file(output, error) || error || !HasDemoMagic(output))
    {
        result.detail = L"The rewrite process returned success but did not produce a valid PBDEMS2 file.";
        return result;
    }

    result.success = true;
    result.source_demo = *source;
    result.output_demo = output;
    result.playdemo_command = "playdemo \"" +
        WideToUtf8(std::wstring(kGeneratedDemoDirectoryName)) + "/" +
        WideToUtf8(filename) + "\"";
    result.detail = L"Temporary skinned demo is ready: " + output.wstring() +
        L" (pawns=" + std::to_wstring(rewritten_pawns) +
        L", knives=" + std::to_wstring(rewritten_knives) +
        L", gloves=" + std::to_wstring(rewritten_gloves) +
        L", animation_resources=" + std::to_wstring(inserted_animation_resources) + L")";
    Logger::Info(result.detail + L"; queued command=" + Utf8ToWide(result.playdemo_command) + L".");
    return result;
}
