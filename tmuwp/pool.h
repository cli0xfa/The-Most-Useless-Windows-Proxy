#pragma once

#include "utils.h"
#include <vector>
#include <stack>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>

// Object pool template
template<typename T>
class ObjectPool {
public:
    ObjectPool(size_t initialSize = 100, size_t maxSize = 1000)
        : maxSize_(maxSize)
        , allocated_(0) {
        for (size_t i = 0; i < initialSize; i++) {
            auto obj = std::make_unique<T>();
            available_.push(std::move(obj));
            allocated_++;
        }
    }

    ~ObjectPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!available_.empty()) {
            available_.pop();
        }
    }

    std::unique_ptr<T> Acquire() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!available_.empty()) {
            auto obj = std::move(available_.top());
            available_.pop();
            return obj;
        }

        
        if (allocated_.load() < maxSize_) {
            allocated_++;
            return std::make_unique<T>();
        }

        // Pool full, return nullptr
        return nullptr;
    }

    void Release(std::unique_ptr<T> obj) {
        if (!obj) return;

        obj->Reset();

        std::lock_guard<std::mutex> lock(mutex_);
        if (available_.size() < maxSize_) {
            available_.push(std::move(obj));
        } else {
            allocated_--;
        }
    }

    size_t GetAvailableCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return available_.size();
    }

    size_t GetAllocatedCount() const {
        return allocated_.load();
    }

private:
    mutable std::mutex mutex_;
    std::stack<std::unique_ptr<T>> available_;
    size_t maxSize_;
    std::atomic<size_t> allocated_;
};

// IOContext pool
class IOContextPool {
public:
    static IOContextPool& GetInstance();

    void Initialize(size_t initialSize = 1000, size_t maxSize = 10000);

    IOContext* Acquire();
    void Release(IOContext* context);

    
    size_t GetTotalAllocated() const;
    size_t GetAvailableCount() const;
    size_t GetInUseCount() const;

private:
    IOContextPool() = default;
    ~IOContextPool();

    IOContextPool(const IOContextPool&) = delete;
    IOContextPool& operator=(const IOContextPool&) = delete;

    std::vector<std::unique_ptr<IOContext>> pool_;
    std::stack<IOContext*> available_;
    mutable std::mutex mutex_;
    std::atomic<size_t> inUse_{0};
    size_t maxSize_ = 10000;
};


struct ConnectionStats {
    std::atomic<uint64_t> totalConnections{0};
    std::atomic<uint64_t> activeConnections{0};
    std::atomic<uint64_t> totalBytesReceived{0};
    std::atomic<uint64_t> totalBytesSent{0};
    std::atomic<uint64_t> failedConnections{0};

    ConnectionStats() = default;

    // Copy constructor
    ConnectionStats(const ConnectionStats& other)
        : totalConnections(other.totalConnections.load())
        , activeConnections(other.activeConnections.load())
        , totalBytesReceived(other.totalBytesReceived.load())
        , totalBytesSent(other.totalBytesSent.load())
        , failedConnections(other.failedConnections.load()) {}

    void Reset() {
        totalConnections = 0;
        activeConnections = 0;
        totalBytesReceived = 0;
        totalBytesSent = 0;
        failedConnections = 0;
    }
};


class StatsManager {
public:
    static StatsManager& GetInstance();

    
    void OnConnectionAccepted();
    void OnConnectionClosed();
    void OnConnectionFailed();

    
    void OnBytesReceived(uint64_t bytes);
    void OnBytesSent(uint64_t bytes);

    
    ConnectionStats GetStats() const;
    std::string GetStatsString() const;

    void Reset();

private:
    StatsManager() = default;

    ConnectionStats stats_;
};

// Performance timer
class PerformanceTimer {
public:
    PerformanceTimer();

    void Start();
    void Stop();
    double GetElapsedMs() const;
    double GetElapsedUs() const;

private:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    TimePoint startTime_;
    TimePoint endTime_;
    bool running_;
};
