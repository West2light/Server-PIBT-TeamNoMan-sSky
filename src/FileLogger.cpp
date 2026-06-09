#include "FileLogger.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace
{

std::mutex     g_logger_mutex;
std::ofstream  g_logger_stream;
std::string    g_logger_path;
bool           g_logger_initialized = false;

std::string BuildTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::tm tm_value {};
#if defined(_WIN32)
    localtime_s(&tm_value, &now_time);
#else
    localtime_r(&now_time, &tm_value);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_value, "%Y-%m-%d %H:%M:%S")
        << '.'
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

} // namespace

bool FileLogger::Initialize(const std::string& path)
{
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    if (g_logger_initialized)
    {
        return g_logger_path == path;
    }

    try
    {
        const std::filesystem::path log_path(path);
        if (log_path.has_parent_path())
        {
            std::filesystem::create_directories(log_path.parent_path());
        }

        g_logger_stream.open(path, std::ios::out | std::ios::app);
        if (!g_logger_stream.is_open())
        {
            std::cerr << "[FileLogger] failed to open log file: " << path << "\n";
            return false;
        }

        g_logger_path = path;
        g_logger_initialized = true;
        g_logger_stream << BuildTimestamp() << " [INFO] [FileLogger] initialized path=" << path << '\n';
        g_logger_stream.flush();
        return true;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[FileLogger] initialize exception: " << ex.what() << "\n";
        return false;
    }
}

void FileLogger::Info(const std::string& line)
{
    Log("INFO", line);
}

void FileLogger::Warn(const std::string& line)
{
    Log("WARN", line);
}

void FileLogger::Error(const std::string& line)
{
    Log("ERROR", line);
}

void FileLogger::Log(const char* level, const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    if (!g_logger_initialized || !g_logger_stream.is_open())
    {
        std::cerr << "[" << level << "] " << line << "\n";
        return;
    }

    g_logger_stream << BuildTimestamp() << " [" << level << "] " << line << '\n';
    g_logger_stream.flush();
}
