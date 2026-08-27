#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kExpectedFileSize = 5'499'904;
constexpr std::uint32_t kLibraryHooksWRva = 0x36b380;
constexpr std::uint64_t kLibraryHooksWRawOffset = 0x36a780;
constexpr std::uint64_t kRenderHookPatchRawOffset = 0x36aba3;

constexpr std::array<std::uint8_t, 10> kExpectedLibraryHooksWPrologue{
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20};
constexpr std::array<std::uint8_t, 5> kExpectedRenderHookPatch{
    0xc3, 0x90, 0x90, 0x90, 0x90};
constexpr std::array<std::uint8_t, 5> kExpectedOriginalRenderHook{
    0xe9, 0x88, 0x09, 0x01, 0x00};

struct UniqueHandle {
    HANDLE value = nullptr;
    ~UniqueHandle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : value(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
};

struct ModuleInfo {
    std::wstring name;
    std::wstring path;
    std::uintptr_t base = 0;
};

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

bool ReadExact(HANDLE file, std::uint64_t offset, void* destination, DWORD size) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    return ReadFile(file, destination, size, &read, nullptr) && read == size;
}

bool ValidateHookFile(const std::wstring& path) {
    UniqueHandle file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Cannot open HLAE hook file: " << path << L"\n";
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) ||
        static_cast<std::uint64_t>(size.QuadPart) != kExpectedFileSize) {
        std::wcerr << L"Unexpected HLAE hook file size.\n";
        return false;
    }

    std::array<std::uint8_t, kExpectedLibraryHooksWPrologue.size()> prologue{};
    std::array<std::uint8_t, kExpectedRenderHookPatch.size()> renderPatch{};
    if (!ReadExact(file.value, kLibraryHooksWRawOffset, prologue.data(),
                   static_cast<DWORD>(prologue.size())) ||
        prologue != kExpectedLibraryHooksWPrologue) {
        std::wcerr << L"LibraryHooksW prologue validation failed.\n";
        return false;
    }
    if (!ReadExact(file.value, kRenderHookPatchRawOffset, renderPatch.data(),
                   static_cast<DWORD>(renderPatch.size())) ||
        (renderPatch != kExpectedRenderHookPatch &&
         renderPatch != kExpectedOriginalRenderHook)) {
        std::wcerr << L"Render hook validation failed.\n";
        return false;
    }
    return true;
}

std::map<std::wstring, ModuleInfo> ReadModules(DWORD processId) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId));
    if (snapshot.value == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("CreateToolhelp32Snapshot failed");
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::map<std::wstring, ModuleInfo> result;
    if (Module32FirstW(snapshot.value, &entry)) {
        do {
            ModuleInfo info;
            info.name = entry.szModule;
            info.path = entry.szExePath;
            info.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            result.emplace(Lower(info.name), std::move(info));
        } while (Module32NextW(snapshot.value, &entry));
    }
    return result;
}

bool InvokeLibraryHooksW(
    HANDLE process,
    std::uintptr_t function,
    const ModuleInfo& module) {
    const std::size_t pathBytes = (module.path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(
        process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) return false;

    void* remoteCode = nullptr;
    bool success = false;
    do {
        SIZE_T written = 0;
        if (!WriteProcessMemory(
                process, remotePath, module.path.c_str(), pathBytes, &written) ||
            written != pathBytes) {
            break;
        }

        // Windows x64: reserve shadow space and align the stack, pass HMODULE
        // in RCX and the full UTF-16 path in RDX, then call LibraryHooksW.
        std::vector<std::uint8_t> stub{
            0x48, 0x83, 0xec, 0x28,
            0x48, 0xb9};
        const auto append64 = [&stub](std::uint64_t value) {
            for (int shift = 0; shift < 64; shift += 8) {
                stub.push_back(static_cast<std::uint8_t>(value >> shift));
            }
        };
        append64(module.base);
        stub.push_back(0x48); stub.push_back(0xba);
        append64(reinterpret_cast<std::uintptr_t>(remotePath));
        stub.push_back(0x48); stub.push_back(0xb8);
        append64(function);
        stub.insert(stub.end(), {
            0xff, 0xd0,
            0x48, 0x83, 0xc4, 0x28,
            0xc3});

        remoteCode = VirtualAllocEx(
            process, nullptr, stub.size(), MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        if (!remoteCode) break;
        written = 0;
        if (!WriteProcessMemory(
                process, remoteCode, stub.data(), stub.size(), &written) ||
            written != stub.size()) {
            break;
        }
        if (!FlushInstructionCache(process, remoteCode, stub.size())) break;

        UniqueHandle thread(CreateRemoteThread(
            process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteCode),
            nullptr, 0, nullptr));
        if (!thread.value) break;
        // The caller intentionally suspends CS2 after injecting HLAE but
        // before client.dll loads. A thread created in that process inherits
        // the process suspension. Release this helper thread's one suspend
        // count while every pre-existing game thread stays paused.
        const DWORD resumeCount = ResumeThread(thread.value);
        if (resumeCount == 0xffffffff) break;
        const DWORD wait = WaitForSingleObject(thread.value, 30'000);
        if (wait != WAIT_OBJECT_0) break;

        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeThread(thread.value, &exitCode) || exitCode == STILL_ACTIVE) {
            break;
        }
        success = true;
    } while (false);

    if (remoteCode) VirtualFreeEx(process, remoteCode, 0, MEM_RELEASE);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    return success;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::wcerr << L"Usage: HlaeLateModuleInit.exe <cs2-pid> <AfxHookSource2.dll>\n";
        return 2;
    }

    const DWORD processId = wcstoul(argv[1], nullptr, 10);
    std::error_code pathError;
    const std::wstring hookPath = std::filesystem::weakly_canonical(argv[2], pathError).wstring();
    if (!processId || pathError || !ValidateHookFile(hookPath)) return 3;

    auto modules = ReadModules(processId);
    const auto processIt = modules.find(L"cs2.exe");
    const auto hookModuleName = Lower(std::filesystem::path(hookPath).filename().wstring());
    const auto hookIt = modules.find(hookModuleName);
    if (processIt == modules.end() || hookIt == modules.end()) {
        std::wcerr << L"Expected CS2 and patched HLAE modules are not both loaded.\n";
        return 4;
    }
    if (Lower(std::filesystem::weakly_canonical(hookIt->second.path, pathError).wstring()) !=
        Lower(hookPath)) {
        std::wcerr << L"Loaded HLAE module path does not match the validated file.\n";
        return 5;
    }
    if (modules.find(L"client.dll") != modules.end()) {
        std::wcerr << L"Refusing late initialization after client.dll has loaded.\n";
        return 6;
    }

    constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    UniqueHandle process(OpenProcess(access, FALSE, processId));
    if (!process.value) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return 7;
    }

    const std::uintptr_t libraryHooksW = hookIt->second.base + kLibraryHooksWRva;
    const std::array<const wchar_t*, 10> orderedModules{
        L"tier0.dll",
        L"filesystem_stdio.dll",
        L"inputsystem.dll",
        L"sdl3.dll",
        L"engine2.dll",
        L"scenesystem.dll",
        L"materialsystem2.dll",
        L"panorama.dll",
        L"schemasystem.dll",
        L"resourcesystem.dll"};

    std::wcout << L"HLAE base=0x" << std::hex << hookIt->second.base
               << L" LibraryHooksW=0x" << libraryHooksW << std::dec << L"\n";
    for (const wchar_t* name : orderedModules) {
        const auto found = modules.find(name);
        if (found == modules.end()) continue;
        std::wcout << L"late_init module=" << found->second.name
                   << L" base=0x" << std::hex << found->second.base << std::dec
                   << L"\n";
        if (!InvokeLibraryHooksW(process.value, libraryHooksW, found->second)) {
            std::wcerr << L"LibraryHooksW failed for " << found->second.name
                       << L" error=" << GetLastError() << L"\n";
            return 8;
        }
    }

    std::wcout << L"Late HLAE module initialization completed before client.dll.\n";
    return 0;
}
