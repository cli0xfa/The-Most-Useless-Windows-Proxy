#pragma once

#include <string>
#include <vector>
#include <memory>

// Proxy type enumeration
enum class ProxyType {
    TCP,
    HTTP,
    SOCKS5
};

// Load balance strategy
enum class BalanceStrategy {
    ROUND_ROBIN,
    WEIGHTED_ROUND_ROBIN,
    LEAST_CONNECTIONS
};


struct ForwardRule {
    std::string name;
    std::string bind;           // Bind address and port, format: "0.0.0.0:8080"
    ProxyType type;
    std::vector<std::string> targets;  // Target server list
    BalanceStrategy balance;
    int connectionTimeout;      // Connection timeout (ms)
    int idleTimeout;            // Idle timeout (ms)

    ForwardRule();
};


class Config {
public:
    Config();
    ~Config();

    
    bool LoadFromFile(const std::string& filePath);
    bool LoadFromJson(const std::string& jsonContent);

    
    const std::vector<ForwardRule>& GetRules() const { return rules_; }
    int GetWorkerThreads() const { return workerThreads_; }
    int GetLogLevel() const { return logLevel_; }

    
    static ProxyType ParseProxyType(const std::string& type);
    static std::string ProxyTypeToString(ProxyType type);
    static BalanceStrategy ParseBalanceStrategy(const std::string& strategy);

private:
    std::vector<ForwardRule> rules_;
    int workerThreads_;
    int logLevel_;

    bool ParseRules(const std::string& json);
};
