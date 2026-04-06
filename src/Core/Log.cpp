#include "Core/Log.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
std::mutex g_log_mutex;
std::filesystem::path g_log_path;

std::wstring Timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm {};
    localtime_s(&local_tm, &now_time);

    std::wstringstream stream;
    stream << std::put_time(&local_tm, L"%Y-%m-%d %H:%M:%S");
    return stream.str();
}
}

void nskinzcs2::Logger::Initialize(const std::wstring& base_directory)
{
    std::scoped_lock lock(g_log_mutex);
    std::filesystem::path directory(base_directory);
    std::filesystem::create_directories(directory);
    g_log_path = directory / L"nSkinzCS2.log";

    std::wofstream stream(g_log_path, std::ios::trunc);
    stream << L"[" << Timestamp() << L"] [INFO] Logger initialized\n";
}

void nskinzcs2::Logger::Info(const std::wstring& message)
{
    WriteLine(L"INFO", message);
}

void nskinzcs2::Logger::InfoAnsi(const char* message)
{
    if (message == nullptr)
    {
        return;
    }

    wchar_t wide_message[1024] {};
    const int converted = MultiByteToWideChar(
        CP_ACP,
        0,
        message,
        -1,
        wide_message,
        static_cast<int>(std::size(wide_message)));
    if (converted <= 0)
    {
        WriteLine(L"INFO", L"[ansi-conversion-failed]");
        return;
    }

    WriteLine(L"INFO", wide_message);
}

void nskinzcs2::Logger::Warn(const std::wstring& message)
{
    WriteLine(L"WARN", message);
}

void nskinzcs2::Logger::Error(const std::wstring& message)
{
    WriteLine(L"ERROR", message);
}

std::wstring nskinzcs2::Logger::LogPath()
{
    std::scoped_lock lock(g_log_mutex);
    return g_log_path.wstring();
}

void nskinzcs2::Logger::WriteLine(const wchar_t* level, const std::wstring& message)
{
    std::scoped_lock lock(g_log_mutex);

    if (g_log_path.empty())
    {
        return;
    }

    std::wofstream stream(g_log_path, std::ios::app);
    stream << L"[" << Timestamp() << L"] [" << level << L"] " << message << L"\n";
    stream.flush();
}

void nskinzcs2::Logger::WriteLine(const wchar_t* level, const wchar_t* message)
{
    std::scoped_lock lock(g_log_mutex);

    if (g_log_path.empty() || message == nullptr)
    {
        return;
    }

    std::wofstream stream(g_log_path, std::ios::app);
    stream << L"[" << Timestamp() << L"] [" << level << L"] " << message << L"\n";
    stream.flush();
}
