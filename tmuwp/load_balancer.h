#pragma once

#include "config.h"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <random>

struct BackendServer {
    std::string address;
    int port;
    int weight;
    std::atomic<int> activeConnections;
    std::atomic<bool> healthy;
    std::atomic<int64_t> lastCheckTime;
    int64_t responseTime;

    BackendServer(const std::string& addr, int w = 1)
        : port(0)
        , weight(w)
        , activeConnections(0)
        , healthy(true)
        , lastCheckTime(0)
        , responseTime(0) {
        size_t pos = addr.rfind(':');
        if (pos != std::string::npos) {
            address = addr.substr(0, pos);
            try {
                port = std::stoi(addr.substr(pos + 1));
            } catch (...) {
                port = 80;
            }
        } else {
            address = addr;
            port = 80;
        }
    }

    std::string GetFullAddress() const {
        return address + ":" + std::to_string(port);
    }
};

class ILoadBalancer {
public:
    virtual ~ILoadBalancer() = default;
    virtual std::shared_ptr<BackendServer> GetNextServer() = 0;
    virtual void ReportSuccess(const std::string& address) {}
    virtual void ReportFailure(const std::string& address) {}
    virtual void UpdateConnectionCount(const std::string& address, int delta) {}
};

class RoundRobinBalancer : public ILoadBalancer {
public:
    RoundRobinBalancer(const std::vector<std::string>& targets);
    std::shared_ptr<BackendServer> GetNextServer() override;

private:
    std::vector<std::shared_ptr<BackendServer>> servers_;
    std::atomic<size_t> currentIndex_{0};
};

class WeightedRoundRobinBalancer : public ILoadBalancer {
public:
    WeightedRoundRobinBalancer(const std::vector<std::string>& targets,
                                const std::vector<int>& weights);
    std::shared_ptr<BackendServer> GetNextServer() override;

private:
    std::vector<std::shared_ptr<BackendServer>> servers_;
    std::vector<size_t> weightedIndices_;
    std::atomic<size_t> currentIndex_{0};
};

class LeastConnectionsBalancer : public ILoadBalancer {
public:
    LeastConnectionsBalancer(const std::vector<std::string>& targets);
    std::shared_ptr<BackendServer> GetNextServer() override;
    void UpdateConnectionCount(const std::string& address, int delta) override;

private:
    std::vector<std::shared_ptr<BackendServer>> servers_;
    std::mutex mutex_;
};

class RandomBalancer : public ILoadBalancer {
public:
    RandomBalancer(const std::vector<std::string>& targets);
    std::shared_ptr<BackendServer> GetNextServer() override;

private:
    std::vector<std::shared_ptr<BackendServer>> servers_;
    std::mutex mutex_;
    std::mt19937 rng_;
};

class LoadBalancerFactory {
public:
    static std::unique_ptr<ILoadBalancer> Create(const std::vector<std::string>& targets,
                                                  BalanceStrategy strategy,
                                                  const std::vector<int>& weights = {});
};
