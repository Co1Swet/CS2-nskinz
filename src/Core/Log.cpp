#include "Core/Log.hpp"

#include <Windows.h>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace
{
std::mutex g_log_mutex;
std::filesystem::path g_log_path;
thread_local unsigned int g_deferred_depth = 0;
struct DeferredLine
{
    std::chrono::system_clock::time_point timestamp;
    std::wstring level;
    std::wstring message;
};
std::mutex g_deferred_mutex;
std::vector<DeferredLine> g_deferred_lines;
std::atomic<unsigned int> g_deferred_dropped {0};
constexpr std::size_t kMaxDeferredLines = 512;
constexpr std::size_t kMaxDeferredMessageCharacters = 16384;

std::wstring Timestamp(const std::chrono::system_clock::time_point now = std::chrono::system_clock::now())
{
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm {};
    localtime_s(&local_tm, &now_time);

    std::wstringstream stream;
    stream << std::put_time(&local_tm, L"%Y-%m-%d %H:%M:%S");
    return stream.str();
}
}

nskinzcs2::Logger::DeferredScope::DeferredScope() noexcept { ++g_deferred_depth; }
nskinzcs2::Logger::DeferredScope::~DeferredScope() { --g_deferred_depth; }

void nskinzcs2::Logger::FlushDeferred()
{
    if (g_deferred_depth != 0) return;
    std::vector<DeferredLine> lines;
    // Reserve before taking the producer lock, keeping the callback's try-lock
    // path short even when the file system/antivirus is slow.
    lines.reserve(kMaxDeferredLines);
    unsigned int dropped = 0;
    {
        std::scoped_lock lock(g_deferred_mutex);
        lines.swap(g_deferred_lines);
        dropped = g_deferred_dropped.exchange(0);
    }
    if (lines.empty() && dropped == 0) return;
    std::scoped_lock lock(g_log_mutex);
    if (g_log_path.empty()) return;
    std::wofstream stream(g_log_path, std::ios::app);
    for (const auto& line : lines)
        stream << L"[" << Timestamp(line.timestamp) << L"] [" << line.level << L"] " << line.message << L"\n";
    if (dropped != 0)
        stream << L"[" << Timestamp() << L"] [WARN] Deferred runtime diagnostics dropped=" << dropped
            << L" (bounded queue busy/full; gameplay was not blocked).\n";
    stream.flush();
}

void nskinzcs2::Logger::Initialize(
    const std::wstring& base_directory,
    const std::wstring& file_name)
{
    std::scoped_lock lock(g_log_mutex);
    std::filesystem::path directory(base_directory);
    std::filesystem::create_directories(directory);
    g_log_path = directory / file_name;
    {
        std::scoped_lock queue_lock(g_deferred_mutex);
        g_deferred_lines.clear();
        g_deferred_lines.reserve(kMaxDeferredLines);
        g_deferred_dropped.store(0);
    }

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
    if (g_deferred_depth != 0)
    {
        std::unique_lock lock(g_deferred_mutex, std::try_to_lock);
        if (!lock.owns_lock() || g_deferred_lines.size() >= kMaxDeferredLines)
        {
            g_deferred_dropped.fetch_add(1);
            return;
        }
        try
        {
            auto bounded_message = message.substr(0, kMaxDeferredMessageCharacters);
            if (bounded_message.size() != message.size()) bounded_message += L" [truncated]";
            g_deferred_lines.push_back({std::chrono::system_clock::now(), level, std::move(bounded_message)});
        }
        catch (...)
        {
            g_deferred_dropped.fetch_add(1);
        }
        return;
    }
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
    if (message != nullptr) WriteLine(level, std::wstring(message));
}
