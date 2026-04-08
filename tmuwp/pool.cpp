#include "pool.h"
#include <sstream>

// IOContextPool 实现
IOContextPool& IOContextPool::GetInstance() {
    static IOContextPool instance;
    return instance;
}

void IOContextPool::Initialize(size_t initialSize, size_t maxSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxSize_ = maxSize;

    pool_.reserve(initialSize);
    for (size_t i = 0; i < initialSize; i++) {
        auto ctx = std::make_unique<IOContext>();
        available_.push(ctx.get());
        pool_.push_back(std::move(ctx));
    }
}

IOContextPool::~IOContextPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!available_.empty()) {
        available_.pop();
    }
    pool_.clear();
}

IOContext* IOContextPool::Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!available_.empty()) {
        IOContext* ctx = available_.top();
        available_.pop();
        inUse_++;
        ctx->Reset();
        return ctx;
    }

    
    if (pool_.size() < maxSize_) {
        auto ctx = std::make_unique<IOContext>();
        IOContext* ptr = ctx.get();
        pool_.push_back(std::move(ctx));
        inUse_++;
        return ptr;
    }

    return nullptr;
}

void IOContextPool::Release(IOContext* context) {
    if (!context) return;

    context->Reset();

    std::lock_guard<std::mutex> lock(mutex_);
    available_.push(context);
    inUse_--;
}

size_t IOContextPool::GetTotalAllocated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_.size();
}

size_t IOContextPool::GetAvailableCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_.size();
}

size_t IOContextPool::GetInUseCount() const {
    return inUse_.load();
}

// StatsManager 实现
StatsManager& StatsManager::GetInstance() {
    static StatsManager instance;
    return instance;
}

void StatsManager::OnConnectionAccepted() {
    stats_.totalConnections++;
    stats_.activeConnections++;
}

void StatsManager::OnConnectionClosed() {
    stats_.activeConnections--;
}

void StatsManager::OnConnectionFailed() {
    stats_.failedConnections++;
}

void StatsManager::OnBytesReceived(uint64_t bytes) {
    stats_.totalBytesReceived += bytes;
}

void StatsManager::OnBytesSent(uint64_t bytes) {
    stats_.totalBytesSent += bytes;
}

ConnectionStats StatsManager::GetStats() const {
    return stats_;
}

std::string StatsManager::GetStatsString() const {
    std::ostringstream oss;
    oss << "Connections: total=" << stats_.totalConnections.load()
        << ", active=" << stats_.activeConnections.load()
        << ", failed=" << stats_.failedConnections.load()
        << " | Bytes: recv=" << stats_.totalBytesReceived.load()
        << ", sent=" << stats_.totalBytesSent.load();
    return oss.str();
}

void StatsManager::Reset() {
    stats_.Reset();
}

// PerformanceTimer 实现
PerformanceTimer::PerformanceTimer() : running_(false) {
}

void PerformanceTimer::Start() {
    startTime_ = Clock::now();
    running_ = true;
}

void PerformanceTimer::Stop() {
    endTime_ = Clock::now();
    running_ = false;
}

double PerformanceTimer::GetElapsedMs() const {
    TimePoint end = running_ ? Clock::now() : endTime_;
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - startTime_);
    return duration.count() / 1000.0;
}

double PerformanceTimer::GetElapsedUs() const {
    TimePoint end = running_ ? Clock::now() : endTime_;
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - startTime_);
    return static_cast<double>(duration.count());
}
