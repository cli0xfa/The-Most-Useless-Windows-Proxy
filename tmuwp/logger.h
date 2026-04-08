#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>


enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERR = 3,    // ERROR conflicts with Windows macro
    FATAL = 4
};


class Logger {
public:
    static Logger& GetInstance();

    
    void Initialize(LogLevel level, const std::string& filePath = "");

    
    void SetLevel(LogLevel level) { level_ = level; }

    // Enable/disable console output
    void SetConsoleOutput(bool enable) { consoleOutput_ = enable; }

    // Enable/disable file output
    void SetFileOutput(bool enable) { fileOutput_ = enable; }

    
    void Log(LogLevel level, const std::string& message, const char* file = nullptr, int line = 0);

    // 便捷方法
    void Debug(const std::string& msg, const char* file = nullptr, int line = 0);
    void Info(const std::string& msg, const char* file = nullptr, int line = 0);
    void Warn(const std::string& msg, const char* file = nullptr, int line = 0);
    void Error(const std::string& msg, const char* file = nullptr, int line = 0);
    void Fatal(const std::string& msg, const char* file = nullptr, int line = 0);

    
    template<typename... Args>
    void LogFormat(LogLevel level, const char* format, Args... args) {
        if (level < level_) return;
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), format, args...);
        Log(level, buffer, nullptr, 0);
    }

private:
    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::string GetTimestamp();
    std::string LevelToString(LogLevel level);
    void WriteToFile(const std::string& message);
    void RotateLogFile();

    LogLevel level_;
    bool consoleOutput_;
    bool fileOutput_;
    std::string filePath_;
    std::ofstream fileStream_;
    std::mutex mutex_;
    size_t maxFileSize_;
    int maxBackupCount_;
};

// 便捷的宏定义
#define LOG_DEBUG(msg) Logger::GetInstance().Debug(msg, __FILE__, __LINE__)
#define LOG_INFO(msg) Logger::GetInstance().Info(msg, __FILE__, __LINE__)
#define LOG_WARN(msg) Logger::GetInstance().Warn(msg, __FILE__, __LINE__)
#define LOG_ERROR(msg) Logger::GetInstance().Error(msg, __FILE__, __LINE__)
#define LOG_FATAL(msg) Logger::GetInstance().Fatal(msg, __FILE__, __LINE__)

#define LOG_DEBUG_FMT(format, ...) Logger::GetInstance().LogFormat(LogLevel::DEBUG, format, __VA_ARGS__)
#define LOG_INFO_FMT(format, ...) Logger::GetInstance().LogFormat(LogLevel::INFO, format, __VA_ARGS__)
#define LOG_WARN_FMT(format, ...) Logger::GetInstance().LogFormat(LogLevel::WARN, format, __VA_ARGS__)
#define LOG_ERROR_FMT(format, ...) Logger::GetInstance().LogFormat(LogLevel::ERR, format, __VA_ARGS__)
#define LOG_FATAL_FMT(format, ...) Logger::GetInstance().LogFormat(LogLevel::FATAL, format, __VA_ARGS__)
