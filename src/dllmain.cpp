#include <Windows.h>

#include <cwchar>

extern "C" DWORD WINAPI BootstrapThread(void* instance);

namespace
{
bool IsCs2HostProcess()
{
    wchar_t path[32768] {};
    constexpr DWORD kPathCapacity = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    const auto length = GetModuleFileNameW(nullptr, path, kPathCapacity);
    if (length == 0 || length >= kPathCapacity)
    {
        return false;
    }

    const auto* slash = std::wcsrchr(path, L'\\');
    const auto* name = slash != nullptr ? slash + 1 : path;
    return _wcsicmp(name, L"cs2.exe") == 0;
}

DWORD WINAPI EntryPoint(void* instance)
{
    BootstrapThread(instance);
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // /MT uses the static CRT, which needs thread attach/detach callbacks.
        // Do not call DisableThreadLibraryCalls for this portable build.
        // The complete control panel is hosted by rundll32.exe.  Never start
        // the injected runtime when this DLL is acting as the isolated UI.
        if (!IsCs2HostProcess())
        {
            return TRUE;
        }
        HANDLE thread = CreateThread(nullptr, 0, &EntryPoint, instance, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
