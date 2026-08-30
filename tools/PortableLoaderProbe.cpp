// Optional /MT x64 diagnostic. Never injects, opens CS2 or starts a UI.
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace
{
void PrintWide(const wchar_t* text)
{
    char utf8[131072] {};
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr))
        std::printf("%s", utf8);
}

int Failure(const char* operation, const DWORD error)
{
    wchar_t message[2048] {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, message, static_cast<DWORD>(std::size(message)), nullptr);
    std::printf("FAIL: %s, GetLastError=%lu (0x%08lX): ", operation, error, error);
    PrintWide(message);
    std::puts("");
    return error != 0 ? static_cast<int>(error) : 1;
}

DWORD WINAPI EmptyThread(void*) { return 0; }
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc < 2 || argc > 3 ||
        (argc == 3 && std::wcscmp(argv[2], L"--system-only") != 0))
    {
        std::puts("Usage: nSkinzCS2.LoadCheck.exe <absolute-dll-path> [--system-only]");
        return 2;
    }
    wchar_t path[32768] {};
    const auto length = GetFullPathNameW(argv[1], static_cast<DWORD>(std::size(path)), path, nullptr);
    if (length == 0 || length >= std::size(path))
        return Failure("GetFullPathNameW", length ? ERROR_FILENAME_EXCED_RANGE : GetLastError());
    std::printf("Host: x64 /MT, %s\nDLL: ", argc == 3 ? "System32-only dependency search" : "HLAE-style LoadLibraryW");
    PrintWide(path);
    std::puts("");
    const auto attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return Failure("DLL file lookup", GetLastError());
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return Failure("Path is a directory, not a DLL", ERROR_DIRECTORY);
    if (argc == 3 && !SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32))
        return Failure("SetDefaultDllDirectories", GetLastError());

    // Ordinary executable name deliberately avoids nSkinz's cs2.exe bootstrap.
    // Do not rename this diagnostic to cs2.exe.
    const auto module = LoadLibraryW(path);
    if (!module)
        return Failure("LoadLibraryW", GetLastError());
    if (!GetProcAddress(module, "nSkinzUiHost"))
    {
        const auto error = GetLastError();
        FreeLibrary(module);
        return Failure("nSkinzUiHost export", error);
    }
    for (int i = 0; i != 16; ++i)
    {
        const auto thread = CreateThread(nullptr, 0, EmptyThread, nullptr, 0, nullptr);
        if (!thread)
            return Failure("CRT thread attach/detach", GetLastError());
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }

    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return Failure("Module snapshot", GetLastError());
    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot, &entry))
    {
        const auto error = GetLastError();
        CloseHandle(snapshot);
        return Failure("Module32FirstW", error);
    }
    do
    {
        std::printf("MODULE: ");
        PrintWide(entry.szExePath);
        std::puts("");
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (!FreeLibrary(module))
        return Failure("FreeLibrary", GetLastError());
    std::puts("PASS: LoadLibraryW, UI export, CRT thread callbacks, FreeLibrary.");
    std::puts("This checks Windows loading only, not HLAE/CS2 gameplay compatibility.");
    return 0;
}
