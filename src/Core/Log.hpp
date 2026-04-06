#pragma once

#include <mutex>
#include <string>

namespace nskinzcs2
{
class Logger
{
public:
    static void Initialize(const std::wstring& base_directory);
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
