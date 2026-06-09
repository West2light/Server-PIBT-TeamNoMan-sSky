#pragma once

#include <string>

class FileLogger
{
public:
    static bool Initialize(const std::string& path);

    static void Info(const std::string& line);
    static void Warn(const std::string& line);
    static void Error(const std::string& line);

private:
    static void Log(const char* level, const std::string& line);
};
