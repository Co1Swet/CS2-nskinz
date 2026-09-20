#include "Runtime/RadarNativeCompiler.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <string_view>
#include <vector>

namespace
{
class UniqueHandle
{
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle()
    {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_ = nullptr;
};

void HashBytes(std::uint64_t& hash, const void* data, const std::size_t size) noexcept
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

void HashPathMetadata(std::uint64_t& hash, const std::filesystem::path& path) noexcept
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error)
    {
        HashBytes(hash, &size, sizeof(size));
    }
    error.clear();
    const auto timestamp = std::filesystem::last_write_time(path, error);
    if (!error)
    {
        const auto ticks = timestamp.time_since_epoch().count();
        HashBytes(hash, &ticks, sizeof(ticks));
    }
    const auto name = path.filename().wstring();
    HashBytes(hash, name.data(), name.size() * sizeof(wchar_t));
}

bool WriteBytes(
    const std::filesystem::path& path,
    const void* bytes,
    const std::size_t byte_count,
    std::wstring& diagnostic)
{
    if (bytes == nullptr || byte_count == 0 || byte_count > std::numeric_limits<DWORD>::max())
    {
        diagnostic = L"The native radar compiler input is empty or too large.";
        return false;
    }
    UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!file.valid())
    {
        diagnostic = L"A native radar compiler input file could not be created: " + path.wstring();
        return false;
    }
    DWORD written = 0;
    const auto expected = static_cast<DWORD>(byte_count);
    if (!WriteFile(file.get(), bytes, expected, &written, nullptr) || written != expected)
    {
        diagnostic = L"A native radar compiler input file could not be written completely: " + path.wstring();
        return false;
    }
    return true;
}

bool ReadBytes(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& bytes,
    std::wstring& diagnostic)
{
    UniqueHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    LARGE_INTEGER size {};
    if (!file.valid() || !GetFileSizeEx(file.get(), &size) || size.QuadPart < 64 ||
        size.QuadPart > 64ll * 1024ll * 1024ll)
    {
        diagnostic = L"CS2 resourcecompiler did not produce a valid-sized radar VTEX: " + path.wstring();
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size())
    {
        bytes.clear();
        diagnostic = L"The native compiled radar VTEX could not be read completely: " + path.wstring();
        return false;
    }
    return true;
}

bool StartsWithInsensitive(const std::wstring_view value, const std::wstring_view prefix) noexcept
{
    return value.size() >= prefix.size() && std::equal(
        prefix.begin(), prefix.end(), value.begin(), [](const wchar_t left, const wchar_t right)
        {
            return towlower(left) == towlower(right);
        });
}

bool LinkCompilerDependency(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::wstring& diagnostic)
{
    if (std::filesystem::exists(destination))
    {
        return true;
    }
    if (CreateHardLinkW(destination.c_str(), source.c_str(), nullptr))
    {
        return true;
    }
    diagnostic = L"A CS2 native compiler dependency could not be linked into the isolated cache: " +
        source.wstring();
    return false;
}

bool ValidateNativeRadarVtexBytes(
    const std::vector<std::uint8_t>& bytes,
    std::wstring& diagnostic)
{
    constexpr std::uint32_t kRed2 = 0x32444552u;
    constexpr std::uint32_t kData = 0x41544144u;
    constexpr std::size_t kResourceHeaderSize = 16;
    constexpr std::size_t kBlockEntrySize = 12;

    if (bytes.size() < 64 || bytes.size() > 64ull * 1024ull * 1024ull)
    {
        diagnostic = L"The compiled radar VTEX size is outside the accepted range.";
        return false;
    }

    std::uint16_t header_version = 0;
    std::uint32_t block_count = 0;
    std::memcpy(&header_version, bytes.data() + 4, sizeof(header_version));
    std::memcpy(&block_count, bytes.data() + 12, sizeof(block_count));
    if (header_version != 12 || block_count < 2 || block_count > 256 ||
        kResourceHeaderSize + static_cast<std::size_t>(block_count) * kBlockEntrySize > bytes.size())
    {
        diagnostic = L"The compiled radar VTEX has an invalid Source 2 resource header.";
        return false;
    }

    std::uint32_t first_block = 0;
    std::memcpy(&first_block, bytes.data() + kResourceHeaderSize, sizeof(first_block));
    if (first_block != kRed2)
    {
        diagnostic = L"The compiled radar VTEX is missing its RED2 metadata block.";
        return false;
    }

    bool has_data = false;
    for (std::uint32_t index = 0; index < block_count; ++index)
    {
        std::uint32_t block_type = 0;
        std::memcpy(
            &block_type,
            bytes.data() + kResourceHeaderSize + static_cast<std::size_t>(index) * kBlockEntrySize,
            sizeof(block_type));
        if (block_type == kData)
        {
            has_data = true;
            break;
        }
    }
    if (!has_data)
    {
        diagnostic = L"The compiled radar VTEX is missing its DATA texture block.";
        return false;
    }
    return true;
}

std::uint64_t RadarOverlayHash(const std::vector<std::uint8_t>& bytes) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : bytes)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}
}

bool nskinzcs2::runtime::CompileRadarOverlayWithCs2(
    const std::filesystem::path& cs2_install_root,
    RadarAlphaOverlay& overlay,
    std::wstring& diagnostic)
{
    overlay.vtex_bytes.clear();
    if (overlay.png_bytes.empty() || overlay.virtual_path.size() < 16 ||
        overlay.virtual_path.find("..") != std::string::npos ||
        overlay.virtual_path.ends_with("_radar_tga.vtex_c") == false)
    {
        diagnostic = L"The generated radar PNG or virtual path is invalid for native compilation.";
        return false;
    }

    const auto game_directory = cs2_install_root / L"game";
    const auto compiler = game_directory / L"bin/win64/resourcecompiler.exe";
    const auto modtools = game_directory / L"csgo/bin/win64/modtools.dll";
    const auto gameinfo = game_directory / L"csgo/gameinfo.gi";
    if (!std::filesystem::is_regular_file(compiler) || !std::filesystem::is_regular_file(modtools) ||
        !std::filesystem::is_regular_file(gameinfo))
    {
        diagnostic = L"CS2 resourcecompiler.exe, modtools.dll, or gameinfo.gi is unavailable.";
        return false;
    }

    constexpr std::array<std::wstring_view, 4> resource_roots {
        L"csgo", L"csgo_imported", L"csgo_core", L"core",
    };
    std::uint64_t environment_hash = 14695981039346656037ull;
    HashPathMetadata(environment_hash, compiler);
    HashPathMetadata(environment_hash, modtools);
    HashPathMetadata(environment_hash, gameinfo);
    std::size_t shader_packages = 0;
    for (const auto root_name : resource_roots)
    {
        std::error_code iterator_error;
        const auto source_root = game_directory / root_name;
        HashPathMetadata(environment_hash, source_root / L"gameinfo.gi");
        for (const auto& entry : std::filesystem::directory_iterator(source_root, iterator_error))
        {
            if (iterator_error || !entry.is_regular_file())
            {
                continue;
            }
            const auto filename = entry.path().filename().wstring();
            if (StartsWithInsensitive(filename, L"shaders_pc") &&
                _wcsicmp(entry.path().extension().c_str(), L".vpk") == 0)
            {
                HashPathMetadata(environment_hash, entry.path());
                ++shader_packages;
            }
        }
        if (iterator_error)
        {
            diagnostic = L"A CS2 resource directory could not be enumerated for native radar compilation: " +
                source_root.wstring();
            return false;
        }
    }
    if (shader_packages == 0)
    {
        diagnostic = L"No CS2 shaders_pc VPK dependencies were found for native radar compilation.";
        return false;
    }

    wchar_t environment_name[64] {};
    swprintf_s(
        environment_name,
        L"native_%016llx",
        static_cast<unsigned long long>(environment_hash));
    const auto cache_root = cs2_install_root.parent_path() /
        L"nSkinzCS2RadarCompiler" / environment_name;
    const auto cache_game = cache_root / L"game";
    const auto cache_content = cache_root / L"content";
    const auto cache_csgo = cache_game / L"csgo";

    std::error_code path_error;
    std::filesystem::create_directories(cache_csgo, path_error);
    if (path_error)
    {
        diagnostic = L"The isolated native radar compiler cache could not be created: " + cache_root.wstring();
        return false;
    }
    if (!CopyFileW(gameinfo.c_str(), (cache_csgo / L"gameinfo.gi").c_str(), FALSE) &&
        GetLastError() != ERROR_FILE_EXISTS)
    {
        diagnostic = L"CS2 gameinfo.gi could not be copied into the isolated native compiler cache.";
        return false;
    }

    for (const auto root_name : resource_roots)
    {
        const auto source_root = game_directory / root_name;
        const auto destination_root = cache_game / root_name;
        std::filesystem::create_directories(destination_root, path_error);
        if (path_error)
        {
            diagnostic = L"A native radar compiler dependency directory could not be created.";
            return false;
        }
        const auto layered_gameinfo = source_root / L"gameinfo.gi";
        if (root_name != L"csgo" && std::filesystem::is_regular_file(layered_gameinfo) &&
            !CopyFileW(layered_gameinfo.c_str(), (destination_root / L"gameinfo.gi").c_str(), FALSE) &&
            GetLastError() != ERROR_FILE_EXISTS)
        {
            diagnostic = L"A layered CS2 gameinfo.gi could not be copied into the native compiler cache: " +
                layered_gameinfo.wstring();
            return false;
        }
        std::error_code iterator_error;
        for (const auto& entry : std::filesystem::directory_iterator(source_root, iterator_error))
        {
            if (iterator_error || !entry.is_regular_file())
            {
                continue;
            }
            const auto filename = entry.path().filename().wstring();
            if (StartsWithInsensitive(filename, L"shaders_pc") &&
                _wcsicmp(entry.path().extension().c_str(), L".vpk") == 0 &&
                !LinkCompilerDependency(entry.path(), destination_root / entry.path().filename(), diagnostic))
            {
                return false;
            }
        }
        if (iterator_error)
        {
            diagnostic = L"A CS2 shader dependency directory changed during native radar compiler setup.";
            return false;
        }
    }

    const auto cached_modtools_directory = cache_csgo / L"bin/win64";
    std::filesystem::create_directories(cached_modtools_directory, path_error);
    if (path_error || !LinkCompilerDependency(
            modtools,
            cached_modtools_directory / L"modtools.dll",
            diagnostic))
    {
        if (diagnostic.empty())
        {
            diagnostic = L"The isolated modtools dependency directory could not be created.";
        }
        return false;
    }

    auto source_vtex_virtual = overlay.virtual_path;
    source_vtex_virtual.resize(source_vtex_virtual.size() - 2);
    auto source_png_virtual = source_vtex_virtual;
    source_png_virtual.replace(source_png_virtual.size() - 5, 5, ".png");
    const auto source_vtex_wide = std::filesystem::path(source_vtex_virtual);
    const auto source_png_wide = std::filesystem::path(source_png_virtual);
    const auto source_vtex = cache_content / L"csgo" / source_vtex_wide;
    const auto source_png = cache_content / L"csgo" / source_png_wide;
    const auto compiled_vtex = cache_csgo / std::filesystem::path(overlay.virtual_path);
    std::filesystem::create_directories(source_vtex.parent_path(), path_error);
    std::filesystem::create_directories(compiled_vtex.parent_path(), path_error);
    if (path_error)
    {
        diagnostic = L"The native radar source or output directory could not be created.";
        return false;
    }
    if (!WriteBytes(source_png, overlay.png_bytes.data(), overlay.png_bytes.size(), diagnostic))
    {
        return false;
    }

    const std::string vtex_source =
        "<!-- dmx encoding keyvalues2_noids 1 format vtex 1 -->\n"
        "\"CDmeVtex\"\n{\n"
        "    \"m_inputTextureArray\" \"element_array\"\n    [\n"
        "        \"CDmeInputTexture\"\n        {\n"
        "            \"m_name\" \"string\" \"InputTexture0\"\n"
        "            \"m_fileName\" \"string\" \"" + source_png_virtual + "\"\n"
        "            \"m_colorSpace\" \"string\" \"srgb\"\n"
        "            \"m_typeString\" \"string\" \"2D\"\n"
        "        }\n    ]\n"
        "    \"m_outputTypeString\" \"string\" \"2D\"\n"
        "    \"m_outputFormat\" \"string\" \"BC7\"\n"
        "    \"m_outputClearColor\" \"vector4\" \"0 0 0 0\"\n"
        "    \"m_textureOutputChannelArray\" \"element_array\"\n    [\n"
        "        \"CDmeTextureOutputChannel\"\n        {\n"
        "            \"m_inputTextureArray\" \"string_array\" [ \"InputTexture0\" ]\n"
        "            \"m_srcChannels\" \"string\" \"rgba\"\n"
        "            \"m_dstChannels\" \"string\" \"rgba\"\n"
        "            \"m_outputColorSpace\" \"string\" \"srgb\"\n"
        "        }\n    ]\n"
        "    \"m_vClamp\" \"vector3\" \"0 0 0\"\n}\n";
    if (!WriteBytes(source_vtex, vtex_source.data(), vtex_source.size(), diagnostic))
    {
        return false;
    }

    const auto compiler_log = cache_root / L"resourcecompiler.log";
    SECURITY_ATTRIBUTES security_attributes {};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;
    UniqueHandle log(CreateFileW(
        compiler_log.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security_attributes,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    std::wstring command_line = L"\"" + compiler.wstring() + L"\" -game \"" +
        cache_csgo.wstring() + L"\" -nop4 -f -novpk -i \"" + source_vtex.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (log.valid())
    {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = log.get();
        startup.hStdError = log.get();
        startup.hStdInput = nullptr;
    }
    PROCESS_INFORMATION process {};
    const auto compiler_directory = compiler.parent_path().wstring();
    if (!CreateProcessW(
            compiler.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            log.valid(),
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            compiler_directory.c_str(),
            &startup,
            &process))
    {
        diagnostic = L"CS2 resourcecompiler.exe could not be started. Log: " + compiler_log.wstring();
        return false;
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    const auto wait_result = WaitForSingleObject(process_handle.get(), 60000);
    if (wait_result != WAIT_OBJECT_0)
    {
        TerminateProcess(process_handle.get(), ERROR_TIMEOUT);
        WaitForSingleObject(process_handle.get(), 5000);
        diagnostic = L"CS2 resourcecompiler.exe timed out while compiling the radar. Log: " +
            compiler_log.wstring();
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process_handle.get(), &exit_code) || exit_code != 0)
    {
        diagnostic = L"CS2 resourcecompiler.exe rejected the generated radar source (exit " +
            std::to_wstring(exit_code) + L"). Log: " + compiler_log.wstring();
        return false;
    }

    std::vector<std::uint8_t> compiled_bytes;
    if (!ReadBytes(compiled_vtex, compiled_bytes, diagnostic))
    {
        diagnostic += L" Compiler log: " + compiler_log.wstring();
        return false;
    }
    if (!ValidateNativeRadarVtexBytes(compiled_bytes, diagnostic))
    {
        diagnostic += L" Compiler log: " + compiler_log.wstring();
        return false;
    }

    overlay.vtex_bytes = std::move(compiled_bytes);
    diagnostic.clear();
    return true;
}

bool nskinzcs2::runtime::MaterializeCompiledRadarOverlay(
    const RadarAlphaOverlay& overlay,
    std::filesystem::path& overlay_root,
    std::filesystem::path& overlay_file,
    std::wstring& diagnostic)
{
    overlay_root.clear();
    overlay_file.clear();
    if (overlay.virtual_path.size() < 16 || overlay.virtual_path.find("..") != std::string::npos ||
        !overlay.virtual_path.ends_with("_radar_tga.vtex_c") ||
        !ValidateNativeRadarVtexBytes(overlay.vtex_bytes, diagnostic))
    {
        if (diagnostic.empty())
        {
            diagnostic = L"The compiled radar overlay or virtual path is invalid.";
        }
        return false;
    }

    std::error_code path_error;
    auto root = std::filesystem::temp_directory_path(path_error);
    if (path_error)
    {
        diagnostic = L"The Windows temporary directory could not be resolved for the radar overlay.";
        return false;
    }
    wchar_t cache_name[64] {};
    swprintf_s(
        cache_name,
        L"radar_overlay_%016llx",
        static_cast<unsigned long long>(RadarOverlayHash(overlay.vtex_bytes)));
    root /= L"nSkinzCS2";
    root /= cache_name;
    const auto file = root / std::filesystem::path(overlay.virtual_path);
    std::filesystem::create_directories(file.parent_path(), path_error);
    if (path_error)
    {
        diagnostic = L"The external radar overlay cache directory could not be created.";
        return false;
    }

    std::vector<std::uint8_t> existing;
    std::wstring ignored;
    const bool already_current = ReadBytes(file, existing, ignored) && existing == overlay.vtex_bytes;
    if (!already_current &&
        !WriteBytes(file, overlay.vtex_bytes.data(), overlay.vtex_bytes.size(), diagnostic))
    {
        return false;
    }
    if (!ValidateNativeRadarVtexFile(file, diagnostic))
    {
        return false;
    }

    overlay_root = std::move(root);
    overlay_file = std::move(file);
    diagnostic.clear();
    return true;
}

bool nskinzcs2::runtime::ValidateNativeRadarVtexFile(
    const std::filesystem::path& path,
    std::wstring& diagnostic)
{
    std::vector<std::uint8_t> bytes;
    if (!ReadBytes(path, bytes, diagnostic))
    {
        return false;
    }
    return ValidateNativeRadarVtexBytes(bytes, diagnostic);
}
