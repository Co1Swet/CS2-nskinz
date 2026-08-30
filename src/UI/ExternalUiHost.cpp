#include "UI/ExternalUiHost.hpp"

#include "Core/Config.hpp"
#include "Core/Log.hpp"
#include "Cs2/Catalog.hpp"
#include "Cs2/CatalogStore.hpp"
#include "UI/MainWindow.hpp"

#include <charconv>
#include <filesystem>
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
    const model::ConfigDocument& document)
{
    if (runtime_process_id == 0 || !ConfigStore::Save(config_path, document))
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
