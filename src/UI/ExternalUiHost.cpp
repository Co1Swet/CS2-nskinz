#include "UI/ExternalUiHost.hpp"

#include "Core/Config.hpp"
#include "Core/Log.hpp"
#include "Cs2/Catalog.hpp"
#include "Cs2/CatalogStore.hpp"
#include "Runtime/RadarAlphaFix.hpp"
#include "Runtime/RadarNativeCompiler.hpp"
#include "UI/MainWindow.hpp"

#include <charconv>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
std::filesystem::path ModulePath(const HMODULE module)
{
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size())
    {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path);
}

std::filesystem::path CurrentDllPath()
{
    // rundll32's callback HINSTANCE is not a reliable handle to the loaded
    // target DLL.  Resolve the owner of an address that is guaranteed to be
    // inside nSkinzCS2 instead, so catalog/config sidecars are always found
    // beside the DLL regardless of its install directory.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&CurrentDllPath),
            &module) ||
        module == nullptr)
    {
        return {};
    }
    return ModulePath(module);
}

std::wstring RuntimeEventName(const DWORD process_id)
{
    std::wstringstream name;
    name << L"Local\\nSkinzCS2.ManualRefresh." << process_id;
    return name.str();
}

std::wstring RuntimeDemoCommandEventName(const DWORD process_id)
{
    std::wstringstream name;
    name << L"Local\\nSkinzCS2.DemoCommand." << process_id;
    return name.str();
}

std::wstring UiHostMutexName(const DWORD process_id)
{
    std::wstringstream name;
    name << L"Local\\nSkinzCS2.UiHost." << process_id;
    return name.str();
}

bool ParseProcessId(const char* text, DWORD& process_id)
{
    process_id = 0;
    if (text == nullptr)
    {
        return false;
    }
    std::string_view input(text);
    while (!input.empty() && input.front() == ' ')
    {
        input.remove_prefix(1);
    }
    const auto end = input.find(' ');
    const auto token = input.substr(0, end);
    unsigned long value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 10);
    if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size() ||
        value == 0 || value > MAXDWORD)
    {
        return false;
    }
    process_id = static_cast<DWORD>(value);
    return true;
}

bool IsExpectedCs2Process(const HANDLE process)
{
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size) || size == 0)
    {
        return false;
    }
    path.resize(size);
    return _wcsicmp(std::filesystem::path(path).filename().c_str(), L"cs2.exe") == 0;
}

std::string WideToUtf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        count,
        nullptr,
        nullptr);
    return result;
}

std::filesystem::path RuntimeCs2InstallRoot(const DWORD process_id)
{
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr)
    {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
    CloseHandle(process);
    if (!queried || size == 0)
    {
        return {};
    }
    path.resize(size);
    if (_wcsicmp(std::filesystem::path(path).filename().c_str(), L"cs2.exe") != 0)
    {
        return {};
    }
    auto root = std::filesystem::path(path).parent_path();
    for (int level = 0; level < 3 && !root.empty(); ++level)
    {
        root = root.parent_path();
    }
    return root;
}

void ClearPreparedRadar(nskinzcs2::model::MapRemapProfile& profile)
{
    profile.radar_overlay_request_virtual_path.clear();
    profile.radar_overlay_virtual_path.clear();
    profile.radar_overlay_root_path.clear();
    profile.radar_overlay_file_path.clear();
}

void PrepareExternalRadarOverlay(
    const DWORD runtime_process_id,
    nskinzcs2::model::MapRemapProfile& profile)
{
    ClearPreparedRadar(profile);
    if (!profile.enabled || !profile.use_source_radar)
    {
        return;
    }

    nskinzcs2::Logger::Info(
        L"[MapRemap/RadarTexture] external radar preparation started; CS2 will not decode or compile this texture.");
    try
    {
        nskinzcs2::runtime::RadarAlphaOverlay overlay;
        nskinzcs2::runtime::RadarAlphaMaskSettings mask_settings;
        mask_settings.black_tolerance = profile.radar_black_tolerance;
        mask_settings.dark_detail_radius = profile.radar_dark_detail_radius;
        mask_settings.background_alpha_percent = profile.radar_background_alpha_percent;
        std::wstring diagnostic;
        const auto outcome = nskinzcs2::runtime::BuildWorkshopRadarAlphaOverlay(
            profile.workshop_id,
            profile.target_map_name,
            overlay,
            diagnostic,
            runtime_process_id,
            mask_settings);
        if (outcome == nskinzcs2::runtime::RadarAlphaFixOutcome::NotNeeded)
        {
            nskinzcs2::Logger::Info(
                L"[MapRemap/RadarTexture] target radar already has usable transparency; no external overlay is needed.");
            return;
        }
        if (outcome != nskinzcs2::runtime::RadarAlphaFixOutcome::Generated)
        {
            nskinzcs2::Logger::Warn(
                L"[MapRemap/RadarTexture] external radar decode was skipped; map replacement will continue without an alpha overlay. " +
                diagnostic);
            return;
        }

        nskinzcs2::Logger::Info(
            L"[MapRemap/RadarTexture] external radar PNG generated; starting native CS2 resource compilation.");
        const auto cs2_root = RuntimeCs2InstallRoot(runtime_process_id);
        if (cs2_root.empty() ||
            !nskinzcs2::runtime::CompileRadarOverlayWithCs2(cs2_root, overlay, diagnostic))
        {
            nskinzcs2::Logger::Warn(
                L"[MapRemap/RadarTexture] external native compilation failed; map replacement will continue without an alpha overlay. " +
                diagnostic);
            return;
        }

        std::filesystem::path overlay_root;
        std::filesystem::path overlay_file;
        if (!nskinzcs2::runtime::MaterializeCompiledRadarOverlay(
                overlay,
                overlay_root,
                overlay_file,
                diagnostic))
        {
            nskinzcs2::Logger::Warn(
                L"[MapRemap/RadarTexture] external compiled radar could not be materialized; map replacement will continue without it. " +
                diagnostic);
            return;
        }

        profile.radar_overlay_request_virtual_path = overlay.request_virtual_path;
        profile.radar_overlay_virtual_path = overlay.virtual_path;
        profile.radar_overlay_root_path = WideToUtf8(overlay_root.wstring());
        profile.radar_overlay_file_path = WideToUtf8(overlay_file.wstring());
        if (profile.radar_overlay_root_path.empty() || profile.radar_overlay_file_path.empty())
        {
            ClearPreparedRadar(profile);
            nskinzcs2::Logger::Warn(
                L"[MapRemap/RadarTexture] external overlay paths could not be encoded as UTF-8; map replacement will continue without it.");
            return;
        }

        std::wstringstream message;
        message << L"[MapRemap/RadarTexture] external native overlay ready ("
                << overlay.width << L'x' << overlay.height << L", visible="
                << overlay.visible_pixels << L", transparent=" << overlay.transparent_pixels
                << L", soft_edge=" << overlay.softened_edge_pixels
                << L", recovered_dark_detail=" << overlay.recovered_detail_pixels
                << L", black_tolerance=" << mask_settings.black_tolerance
                << L", applied_black_tolerance=" << overlay.applied_black_tolerance
                << L", neutral_mode=" << overlay.detected_neutral_mode
                << L", neutral_pixels=" << overlay.detected_neutral_pixels
                << L", neutral_mode_pixels=" << overlay.detected_neutral_mode_pixels
                << L", detail_radius=" << mask_settings.dark_detail_radius
                << L", background_alpha=" << mask_settings.background_alpha_percent << L'%'
                << L", request="
                << std::wstring(overlay.request_virtual_path.begin(), overlay.request_virtual_path.end())
                << L", alias="
                << std::wstring(overlay.virtual_path.begin(), overlay.virtual_path.end())
                << L"): " << overlay_file.wstring();
        nskinzcs2::Logger::Info(message.str());
    }
    catch (const std::exception& exception)
    {
        ClearPreparedRadar(profile);
        nskinzcs2::Logger::Warn(
            L"[MapRemap/RadarTexture] external radar preparation threw an exception; map replacement will continue without it: " +
            std::wstring(exception.what(), exception.what() + std::strlen(exception.what())));
    }
    catch (...)
    {
        ClearPreparedRadar(profile);
        nskinzcs2::Logger::Warn(
            L"[MapRemap/RadarTexture] external radar preparation failed unexpectedly; map replacement will continue without it.");
    }
}
}

bool nskinzcs2::ui::LaunchExternalUiHost(
    const HMODULE module,
    const DWORD runtime_process_id)
{
    const auto module_path = ModulePath(module);
    if (module_path.empty() || runtime_process_id == 0)
    {
        return false;
    }

    std::wstring system_directory(32768, L'\0');
    const auto system_size = GetSystemDirectoryW(
        system_directory.data(),
        static_cast<UINT>(system_directory.size()));
    if (system_size == 0 || system_size >= system_directory.size())
    {
        return false;
    }
    system_directory.resize(system_size);
    const auto rundll32 = std::filesystem::path(system_directory) / L"rundll32.exe";

    std::wstringstream command;
    command << L'\"' << rundll32.wstring() << L"\" \""
        << module_path.wstring() << L"\",nSkinzUiHost "
        << runtime_process_id;
    auto mutable_command = command.str();
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(
            rundll32.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            module_path.parent_path().c_str(),
            &startup,
            &process))
    {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool nskinzcs2::ui::PublishExternalConfigUpdate(
    const DWORD runtime_process_id,
    const std::filesystem::path& config_path,
    model::ConfigDocument& document)
{
    if (runtime_process_id == 0)
    {
        return false;
    }
    PrepareExternalRadarOverlay(runtime_process_id, document.map_remap);
    if (!ConfigStore::Save(config_path, document))
    {
        return false;
    }

    const auto event_name = RuntimeEventName(runtime_process_id);
    const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
    if (event == nullptr)
    {
        return false;
    }
    const auto signaled = SetEvent(event) != FALSE;
    CloseHandle(event);
    return signaled;
}

bool nskinzcs2::ui::PublishExternalDemoCommand(
    const DWORD runtime_process_id,
    const std::filesystem::path& dll_directory,
    const std::string& command)
{
    if (runtime_process_id == 0 || dll_directory.empty() || command.empty() ||
        command.find_first_of("\r\n") != std::string::npos)
    {
        return false;
    }

    const auto destination = dll_directory / L"nSkinzCS2.demo-command.txt";
    const auto temporary = dll_directory / L"nSkinzCS2.demo-command.tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream.write(command.data(), static_cast<std::streamsize>(command.size()));
        stream.put('\n');
    }
    if (!MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    const auto event = OpenEventW(
        EVENT_MODIFY_STATE,
        FALSE,
        RuntimeDemoCommandEventName(runtime_process_id).c_str());
    if (event == nullptr)
    {
        return false;
    }
    const auto signaled = SetEvent(event) != FALSE;
    CloseHandle(event);
    return signaled;
}

extern "C" __declspec(dllexport) void CALLBACK nSkinzUiHost(
    HWND,
    HINSTANCE,
    LPSTR command_line,
    int)
{
    DWORD runtime_process_id = 0;
    if (!ParseProcessId(command_line, runtime_process_id))
    {
        return;
    }

    const auto runtime_process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        runtime_process_id);
    if (runtime_process == nullptr || !IsExpectedCs2Process(runtime_process))
    {
        if (runtime_process != nullptr)
        {
            CloseHandle(runtime_process);
        }
        return;
    }

    const auto mutex_name = UiHostMutexName(runtime_process_id);
    const auto host_mutex = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (host_mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (host_mutex != nullptr)
        {
            CloseHandle(host_mutex);
        }
        CloseHandle(runtime_process);
        return;
    }

    const auto module_path = CurrentDllPath();
    if (module_path.empty())
    {
        CloseHandle(host_mutex);
        CloseHandle(runtime_process);
        return;
    }
    const auto base_directory = module_path.parent_path();
    nskinzcs2::Logger::Initialize(
        base_directory.wstring(),
        L"nSkinzCS2.ui.log");
    nskinzcs2::AppState state;
    if (const auto catalog = nskinzcs2::CatalogStore::Load(
            base_directory / L"nSkinzCS2.catalog.json");
        catalog.has_value())
    {
        state.ReplaceCatalog(*catalog);
        nskinzcs2::Logger::Info(
            L"Loaded the complete catalog snapshot beside the nSkinzCS2 DLL.");
    }
    else
    {
        state.ReplaceCatalog(nskinzcs2::CatalogBuilder::BuildFallbackCatalog());
        nskinzcs2::Logger::Warn(
            L"Catalog snapshot could not be loaded; the control panel is using the small fallback catalog.");
    }
    state.ReplaceConfig(nskinzcs2::ConfigStore::Load(base_directory / L"nSkinzCS2.json"));

    nskinzcs2::runtime::Status status;
    status.availability = nskinzcs2::runtime::Availability::Ready;
    status.process_detected = true;
    status.hlae_detected = true;
    const auto catalog_counts = state.SnapshotCatalog();
    status.runtime_catalog_populated = catalog_counts.runtime_populated;
    status.detail =
        L"Control panel isolated from the CS2/ReShade render process. Catalog: " +
        std::to_wstring(catalog_counts.weapon_paint_kits.size()) +
        L" weapon finishes, " +
        std::to_wstring(catalog_counts.glove_paint_kits.size()) +
        L" glove finishes.";
    nskinzcs2::Logger::Info(status.detail);
    status.process_path = L"cs2.exe (PID " + std::to_wstring(runtime_process_id) + L")";
    state.UpdateStatus(std::move(status));

    nskinzcs2::MainWindow window(state, runtime_process_id);
    window.Start();
    while (WaitForSingleObject(runtime_process, 250) == WAIT_TIMEOUT &&
           !state.ShutdownRequested())
    {
    }
    window.Stop();

    CloseHandle(host_mutex);
    CloseHandle(runtime_process);
}
