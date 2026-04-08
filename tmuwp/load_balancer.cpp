#include "load_balancer.h"
#include <algorithm>
#include <limits>

// RoundRobinBalancer 实现
RoundRobinBalancer::RoundRobinBalancer(const std::vector<std::string>& targets) {
    for (const auto& target : targets) {
        servers_.push_back(std::make_shared<BackendServer>(target, 1));
    }
}

std::shared_ptr<BackendServer> RoundRobinBalancer::GetNextServer() {
    if (servers_.empty()) {
        return nullptr;
    }

    size_t index = currentIndex_.fetch_add(1, std::memory_order_relaxed) % servers_.size();
    return servers_[index];
}

// WeightedRoundRobinBalancer 实现
WeightedRoundRobinBalancer::WeightedRoundRobinBalancer(
    const std::vector<std::string>& targets,
    const std::vector<int>& weights) {

    for (size_t i = 0; i < targets.size(); i++) {
        int weight = (i < weights.size()) ? weights[i] : 1;
        auto server = std::make_shared<BackendServer>(targets[i], weight);
        servers_.push_back(server);

        // 根据权重展开索引
        for (int j = 0; j < weight; j++) {
            weightedIndices_.push_back(i);
        }
    }
}

std::shared_ptr<BackendServer> WeightedRoundRobinBalancer::GetNextServer() {
    if (weightedIndices_.empty()) {
        return nullptr;
    }

    size_t index = currentIndex_.fetch_add(1, std::memory_order_relaxed) % weightedIndices_.size();
    return servers_[weightedIndices_[index]];
}

// LeastConnectionsBalancer 实现
LeastConnectionsBalancer::LeastConnectionsBalancer(const std::vector<std::string>& targets) {
    for (const auto& target : targets) {
        servers_.push_back(std::make_shared<BackendServer>(target, 1));
    }
}

std::shared_ptr<BackendServer> LeastConnectionsBalancer::GetNextServer() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (servers_.empty()) {
        return nullptr;
    }

    std::shared_ptr<BackendServer> selected = nullptr;
    int minConnections = std::numeric_limits<int>::max();

    for (auto& server : servers_) {
        int connections = server->activeConnections.load();
        if (connections < minConnections) {
            minConnections = connections;
            selected = server;
        }
    }

    if (selected) {
        selected->activeConnections.fetch_add(1);
    }

    return selected;
}

void LeastConnectionsBalancer::UpdateConnectionCount(const std::string& address, int delta) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& server : servers_) {
        if (server->GetFullAddress() == address) {
            int current = server->activeConnections.load();
            int newVal = std::max(0, current + delta);
            server->activeConnections.store(newVal);
            break;
        }
    }
}

// RandomBalancer 实现
RandomBalancer::RandomBalancer(const std::vector<std::string>& targets)
    : rng_(std::random_device{}()) {
    for (const auto& target : targets) {
        servers_.push_back(std::make_shared<BackendServer>(target, 1));
    }
}

std::shared_ptr<BackendServer> RandomBalancer::GetNextServer() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (servers_.empty()) {
        return nullptr;
    }

    std::uniform_int_distribution<size_t> dist(0, servers_.size() - 1);
    return servers_[dist(rng_)];
}

// LoadBalancerFactory 实现
std::unique_ptr<ILoadBalancer> LoadBalancerFactory::Create(
    const std::vector<std::string>& targets,
    BalanceStrategy strategy,
    const std::vector<int>& weights) {

    switch (strategy) {
    case BalanceStrategy::ROUND_ROBIN:
        return std::make_unique<RoundRobinBalancer>(targets);

    case BalanceStrategy::WEIGHTED_ROUND_ROBIN:
        return std::make_unique<WeightedRoundRobinBalancer>(targets, weights);

    case BalanceStrategy::LEAST_CONNECTIONS:
        return std::make_unique<LeastConnectionsBalancer>(targets);

    default:
        return std::make_unique<RoundRobinBalancer>(targets);
    }
}
