// Load only the installed tier0 in THIS helper process. No game process access,
// injection, windows, input, shared VData writes, or persistent game-file changes.
#include "Runtime/KnifeModel.hpp"
#include <Windows.h>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

using SymbolFunction = const char* (__fastcall*)(const char*);
using FindHashFunction = const char* (__fastcall*)(std::uint32_t);

bool CheckSymbols(SymbolFunction make, SymbolFunction find, FindHashFunction find_hash)
{
    std::array<const char*, 22> saved {};
    std::size_t index = 0;
    for (const auto& asset : nskinzcs2::runtime::KnifeModelAssets())
    {
        char first[96] {}, second[96] {}, upper[96] {};
        if (asset.animation_class.size() >= sizeof(first)) { return false; }
        std::memcpy(first, asset.animation_class.data(), asset.animation_class.size());
        std::memcpy(second, first, sizeof(first));
        for (std::size_t i = 0; i < asset.animation_class.size(); ++i)
        {
            upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(first[i])));
        }
        const auto a = make(first);
        const auto b = make(second);
        const auto c = find(first);
        const auto d = find(upper);
        std::uint32_t hash = 0;
        if (a == nullptr || a != b || a != c || a != d || a == first || a == second || std::strcmp(a, first) != 0)
        {
            std::printf("FAIL definition=%d canonical_symbol\n", asset.definition);
            return false;
        }
        // The local SDK implementation and tier0 machine code use a hash before
        // the interned string. Test it, do not assume arbitrary strings have one.
        std::memcpy(&hash, a - sizeof(hash), sizeof(hash));
        if (find_hash(hash) != a)
        {
            std::printf("FAIL definition=%d hash_roundtrip\n", asset.definition);
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (saved[previous] == a) { return false; }
        }
        saved[index++] = a;
        std::printf("PASS definition=%d class=%s canonical=1 case_fold=1 hash_roundtrip=1\n", asset.definition, a);
    }
    return find("nskinz_missing_symbol_ea20db8c_9321_42f8") == nullptr;
}

int Run(int argc, wchar_t** argv)
{
    if (argc != 2) { return 2; }
    const std::filesystem::path path = std::filesystem::absolute(argv[1]);
    if (path.filename() != L"tier0.dll") { return 3; }
    const auto module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr)
    {
        std::printf("LoadLibraryExW failed error=%lu\n", GetLastError());
        return 4;
    }
    const auto make = reinterpret_cast<SymbolFunction>(GetProcAddress(module, "_MakeGlobalSymbol"));
    const auto find = reinterpret_cast<SymbolFunction>(GetProcAddress(module, "_FindGlobalSymbol"));
    const auto find_hash = reinterpret_cast<FindHashFunction>(GetProcAddress(module, "_FindGlobalSymbolByHash"));
    const bool ok = make != nullptr && find != nullptr && find_hash != nullptr && CheckSymbols(make, find, find_hash);
    std::printf("native_symbol_result=%s; scope=isolated-tier0-process; cs2_not_started=1\n", ok ? "PASS" : "FAIL");
    // Keep tier0 loaded until helper exit, just as the game does.
    return ok ? 0 : 5;
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    __try { return Run(argc, argv); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        std::printf("FAIL native symbol SEH=0x%08lX\n", GetExceptionCode());
        return 6;
    }
}
