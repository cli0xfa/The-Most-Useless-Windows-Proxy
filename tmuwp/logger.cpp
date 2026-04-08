#include "logger.h"
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger::Logger()
    : level_(LogLevel::INFO)
    , consoleOutput_(true)
    , fileOutput_(false)
    , maxFileSize_(10 * 1024 * 1024)
    , maxBackupCount_(5) {
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fileStream_.is_open()) {
        fileStream_.close();
    }
}

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

void Logger::Initialize(LogLevel level, const std::string& filePath) {
    std::lock_guard<std::mutex> lock(mutex_);

    level_ = level;

    if (!filePath.empty()) {
        filePath_ = filePath;
        fileOutput_ = true;

        if (fileStream_.is_open()) {
            fileStream_.close();
        }

        fileStream_.open(filePath_, std::ios::app);
    }
}

void Logger::Log(LogLevel level, const std::string& message, const char* file, int line) {
    if (level < level_) {
        return;
    }

    std::string timestamp = GetTimestamp();
    std::string levelStr = LevelToString(level);

    std::ostringstream oss;
    oss << "[" << timestamp << "] [" << levelStr << "] " << message;

    if (file && line > 0) {
        const char* filename = file;
        const char* p = file;
        while (*p) {
            if (*p == '\\' || *p == '/') {
                filename = p + 1;
            }
            p++;
        }
        oss << " [" << filename << ":" << line << "]";
    }

    std::string logLine = oss.str();

    if (consoleOutput_) {
        if (level >= LogLevel::ERR) {
            std::cerr << logLine << std::endl;
        } else {
            std::cout << logLine << std::endl;
        }
    }

    if (fileOutput_) {
        WriteToFile(logLine);
    }
}

void Logger::Debug(const std::string& msg, const char* file, int line) {
    Log(LogLevel::DEBUG, msg, file, line);
}

void Logger::Info(const std::string& msg, const char* file, int line) {
    Log(LogLevel::INFO, msg, file, line);
}

void Logger::Warn(const std::string& msg, const char* file, int line) {
    Log(LogLevel::WARN, msg, file, line);
}

void Logger::Error(const std::string& msg, const char* file, int line) {
    Log(LogLevel::ERR, msg, file, line);
}

void Logger::Fatal(const std::string& msg, const char* file, int line) {
    Log(LogLevel::FATAL, msg, file, line);
}

std::string Logger::GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm localTime;
    localtime_s(&localTime, &time);

    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::LevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO";
    case LogLevel::WARN:  return "WARN";
    case LogLevel::ERR: return "ERR";
    case LogLevel::FATAL: return "FATAL";
    default: return "UNKNOWN";
    }
}

void Logger::WriteToFile(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fileStream_.is_open()) {
        return;
    }

    fileStream_ << message << std::endl;
    fileStream_.flush();

    auto pos = fileStream_.tellp();
    if (pos > static_cast<std::streamoff>(maxFileSize_)) {
        RotateLogFile();
    }
}

void Logger::RotateLogFile() {
    if (fileStream_.is_open()) {
        fileStream_.close();
    }

    std::string oldestBackup = filePath_ + "." + std::to_string(maxBackupCount_);
    remove(oldestBackup.c_str());

    for (int i = maxBackupCount_ - 1; i >= 1; i--) {
        std::string oldName = filePath_ + "." + std::to_string(i);
        std::string newName = filePath_ + "." + std::to_string(i + 1);
        rename(oldName.c_str(), newName.c_str());
    }

    rename(filePath_.c_str(), (filePath_ + ".1").c_str());

    fileStream_.open(filePath_, std::ios::app);
}
