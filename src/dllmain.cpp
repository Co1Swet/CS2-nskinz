#include <Windows.h>

extern "C" DWORD WINAPI BootstrapThread(void* instance);

namespace
{
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
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, &EntryPoint, instance, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
