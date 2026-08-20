#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace {

using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LibraryHooksWFn = void (*)(HMODULE, LPCWSTR);

constexpr std::uintptr_t kLibraryHooksWRva = 0x36b380;
constexpr std::uintptr_t kRenderHookPatchRva = 0x36b7a3;
constexpr std::array<std::uint8_t, 10> kExpectedLibraryHooksWPrologue{
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20};
constexpr std::array<std::uint8_t, 5> kExpectedRenderHookPatch{
    0xc3, 0x90, 0x90, 0x90, 0x90};

LoadLibraryExWFn g_TrueLoadLibraryExW = nullptr;
LibraryHooksWFn g_LibraryHooksW = nullptr;
volatile LONG g_LateInitState = 0;  // 0=pending, 1=running, 2=complete, -1=failed

HMODULE WINAPI BridgeLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags);

bool EqualAsciiInsensitive(const char* left, const char* right) {
    if (!left || !right) return false;
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return *left == *right;
}

bool ValidateBytes(const void* address, const std::uint8_t* expected, std::size_t size) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] != expected[index]) return false;
    }
    return true;
}

bool ResolveHlaeInternals() {
    HMODULE hlae = GetModuleHandleW(L"AfxHookSource2.nskinz-test.dll");
    if (!hlae) return false;

    const auto base = reinterpret_cast<std::uintptr_t>(hlae);
    void* libraryHooksW = reinterpret_cast<void*>(base + kLibraryHooksWRva);
    void* renderPatch = reinterpret_cast<void*>(base + kRenderHookPatchRva);
    if (!ValidateBytes(
            libraryHooksW,
            kExpectedLibraryHooksWPrologue.data(),
            kExpectedLibraryHooksWPrologue.size()) ||
        !ValidateBytes(
            renderPatch,
            kExpectedRenderHookPatch.data(),
            kExpectedRenderHookPatch.size())) {
        return false;
    }

    g_LibraryHooksW = reinterpret_cast<LibraryHooksWFn>(libraryHooksW);
    return true;
}

bool PatchModuleImport(HMODULE module) {
    if (!module) return false;
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }

    const auto& importDirectory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDirectory.VirtualAddress || !importDirectory.Size) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        const_cast<std::uint8_t*>(base) + importDirectory.VirtualAddress);

    for (; descriptor->Name; ++descriptor) {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (!EqualAsciiInsensitive(dllName, "kernel32.dll")) continue;

        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            const_cast<std::uint8_t*>(base) + descriptor->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            const_cast<std::uint8_t*>(base) + descriptor->FirstThunk);
        if (!descriptor->OriginalFirstThunk) names = addresses;

        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name),
                            "LoadLibraryExW") != 0) {
                continue;
            }

            auto* slot = reinterpret_cast<void**>(&addresses->u1.Function);
            DWORD oldProtection = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection)) {
                return false;
            }
            InterlockedExchangePointer(
                slot, reinterpret_cast<void*>(&BridgeLoadLibraryExW));
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), oldProtection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    return false;
}

bool RunLateInitialization() {
    if (!g_LibraryHooksW) return false;
    if (GetModuleHandleW(L"client.dll")) return false;

    constexpr std::array<const wchar_t*, 10> orderedModules{
        L"tier0.dll",
        L"filesystem_stdio.dll",
        L"inputsystem.dll",
        L"SDL3.dll",
        L"engine2.dll",
        L"scenesystem.dll",
        L"materialsystem2.dll",
        L"panorama.dll",
        L"schemasystem.dll",
        L"resourcesystem.dll"};

    wchar_t path[32768]{};
    for (const wchar_t* name : orderedModules) {
        HMODULE module = GetModuleHandleW(name);
        if (!module) continue;
        const DWORD length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return false;
        g_LibraryHooksW(module, path);
    }
    return true;
}

HMODULE WINAPI BridgeLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags) {
    if (InterlockedCompareExchange(&g_LateInitState, 1, 0) == 0) {
        const bool success = RunLateInitialization();
        InterlockedExchange(&g_LateInitState, success ? 2 : -1);
        OutputDebugStringW(success
            ? L"nSkinz HLAE late module initialization completed.\n"
            : L"nSkinz HLAE late module initialization failed closed.\n");
    }
    HMODULE result = g_TrueLoadLibraryExW(fileName, file, flags);
    if (result && g_LibraryHooksW) g_LibraryHooksW(result, fileName);
    return result;
}

}  // namespace

extern "C" __declspec(dllexport) LONG WINAPI NSkinzHlaeLateInitState() {
    return g_LateInitState;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (!ResolveHlaeInternals()) return FALSE;

        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        g_TrueLoadLibraryExW = reinterpret_cast<LoadLibraryExWFn>(
            GetProcAddress(kernel32, "LoadLibraryExW"));
        if (!g_TrueLoadLibraryExW) return FALSE;

        const std::array<const wchar_t*, 11> hookModules{
            nullptr,
            L"tier0.dll",
            L"filesystem_stdio.dll",
            L"launcher.dll",
            L"inputsystem.dll",
            L"SDL3.dll",
            L"engine2.dll",
            L"scenesystem.dll",
            L"materialsystem2.dll",
            L"schemasystem.dll",
            L"resourcesystem.dll"};
        bool executablePatched = false;
        for (const wchar_t* name : hookModules) {
            HMODULE module = name ? GetModuleHandleW(name) : GetModuleHandleW(nullptr);
            if (!module) continue;
            const bool patched = PatchModuleImport(module);
            if (!name) executablePatched = patched;
        }
        if (!executablePatched) return FALSE;
    }
    return TRUE;
}
