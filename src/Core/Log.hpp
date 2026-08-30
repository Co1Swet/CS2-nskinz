#pragma once

#include <mutex>
#include <string>

namespace nskinzcs2
{
class Logger
{
public:
    // Hook callbacks enqueue diagnostics; only the existing runtime worker
    // flushes them. The scope itself never performs file I/O or waits for it.
    class DeferredScope
    {
    public:
        DeferredScope() noexcept;
        ~DeferredScope();
        DeferredScope(const DeferredScope&) = delete;
        DeferredScope& operator=(const DeferredScope&) = delete;
    };
    static void FlushDeferred();
    static void Initialize(
        const std::wstring& base_directory,
        const std::wstring& file_name = L"nSkinzCS2.log");
    static void Info(const std::wstring& message);
    static void InfoAnsi(const char* message);
    static void Warn(const std::wstring& message);
    static void Error(const std::wstring& message);
    static std::wstring LogPath();

private:
    static void WriteLine(const wchar_t* level, const std::wstring& message);
    static void WriteLine(const wchar_t* level, const wchar_t* message);
};
}
