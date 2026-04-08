#include "config.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>


namespace {
    // Trim whitespace from both ends of string
    std::string Trim(const std::string& str) {
        size_t start = 0;
        while (start < str.length() && std::isspace(str[start])) start++;
        size_t end = str.length();
        while (end > start && std::isspace(str[end - 1])) end--;
        return str.substr(start, end - start);
    }

    // Extract JSON string value
    std::string ExtractString(const std::string& json, const std::string& key) {
        std::string searchKey = "\"" + key + "\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos) return "";

        pos = json.find(':', pos);
        if (pos == std::string::npos) return "";
        pos++;

        while (pos < json.length() && std::isspace(json[pos])) pos++;

        if (pos < json.length() && json[pos] == '"') {
            pos++;
            size_t end = json.find('"', pos);
            if (end == std::string::npos) return "";
            return json.substr(pos, end - pos);
        }
        return "";
    }

    // Extract JSON array
    std::vector<std::string> ExtractStringArray(const std::string& json, const std::string& key) {
        std::vector<std::string> result;
        std::string searchKey = "\"" + key + "\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos) return result;

        pos = json.find(':', pos);
        if (pos == std::string::npos) return result;
        pos++;

        while (pos < json.length() && std::isspace(json[pos])) pos++;

        if (pos >= json.length() || json[pos] != '[') return result;
        pos++;

        while (pos < json.length()) {
            while (pos < json.length() && std::isspace(json[pos])) pos++;
            if (pos >= json.length() || json[pos] == ']') break;

            if (json[pos] == '"') {
                pos++;
                size_t end = json.find('"', pos);
                if (end == std::string::npos) break;
                result.push_back(json.substr(pos, end - pos));
                pos = end + 1;
            }

            while (pos < json.length() && std::isspace(json[pos])) pos++;
            if (pos < json.length() && json[pos] == ',') pos++;
        }

        return result;
    }

    // Extract JSON object array
    std::vector<std::string> ExtractObjectArray(const std::string& json, const std::string& key) {
        std::vector<std::string> result;
        std::string searchKey = "\"" + key + "\"";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos) return result;

        pos = json.find(':', pos);
        if (pos == std::string::npos) return result;
        pos++;

        while (pos < json.length() && std::isspace(json[pos])) pos++;

        if (pos >= json.length() || json[pos] != '[') return result;
        pos++;

        int braceCount = 0;
        size_t start = 0;
        bool inString = false;

        while (pos < json.length()) {
            char c = json[pos];

            if (c == '"' && (pos == 0 || json[pos - 1] != '\\')) {
                inString = !inString;
            } else if (!inString) {
                if (c == '{') {
                    if (braceCount == 0) start = pos;
                    braceCount++;
                } else if (c == '}') {
                    braceCount--;
                    if (braceCount == 0) {
                        result.push_back(json.substr(start, pos - start + 1));
                    }
                } else if (c == ']' && braceCount == 0) {
                    break;
                }
            }
            pos++;
        }

        return result;
    }
}


ForwardRule::ForwardRule()
    : type(ProxyType::TCP)
    , balance(BalanceStrategy::ROUND_ROBIN)
    , connectionTimeout(30000)
    , idleTimeout(300000) {
}

Config::Config() : workerThreads_(0), logLevel_(1) {}

Config::~Config() {}

bool Config::LoadFromFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << filePath << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return LoadFromJson(buffer.str());
}

bool Config::LoadFromJson(const std::string& jsonContent) {
    
    std::string workers = ExtractString(jsonContent, "worker_threads");
    if (!workers.empty()) {
        try {
            workerThreads_ = std::stoi(workers);
        } catch (...) {
            workerThreads_ = 0;
        }
    }

    std::string logLevel = ExtractString(jsonContent, "log_level");
    if (!logLevel.empty()) {
        try {
            logLevel_ = std::stoi(logLevel);
        } catch (...) {
            logLevel_ = 1;
        }
    }

    
    return ParseRules(jsonContent);
}

bool Config::ParseRules(const std::string& json) {
    rules_.clear();

    std::vector<std::string> listeners = ExtractObjectArray(json, "listeners");

    for (const auto& listenerJson : listeners) {
        ForwardRule rule;

        rule.name = ExtractString(listenerJson, "name");
        rule.bind = ExtractString(listenerJson, "bind");

        std::string type = ExtractString(listenerJson, "type");
        rule.type = ParseProxyType(type);

        rule.targets = ExtractStringArray(listenerJson, "targets");

        std::string balance = ExtractString(listenerJson, "balance");
        rule.balance = ParseBalanceStrategy(balance);

        std::string connTimeout = ExtractString(listenerJson, "connection_timeout");
        if (!connTimeout.empty()) {
            try {
                rule.connectionTimeout = std::stoi(connTimeout);
            } catch (...) {}
        }

        std::string idleTimeout = ExtractString(listenerJson, "idle_timeout");
        if (!idleTimeout.empty()) {
            try {
                rule.idleTimeout = std::stoi(idleTimeout);
            } catch (...) {}
        }

        
        if (rule.name.empty() || rule.bind.empty() || rule.targets.empty()) {
            std::cerr << "Invalid rule: missing required fields" << std::endl;
            continue;
        }

        rules_.push_back(rule);
    }

    return !rules_.empty();
}

ProxyType Config::ParseProxyType(const std::string& type) {
    std::string lower = type;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "http") return ProxyType::HTTP;
    if (lower == "socks5") return ProxyType::SOCKS5;
    return ProxyType::TCP;
}

std::string Config::ProxyTypeToString(ProxyType type) {
    switch (type) {
        case ProxyType::HTTP: return "http";
        case ProxyType::SOCKS5: return "socks5";
        default: return "tcp";
    }
}

BalanceStrategy Config::ParseBalanceStrategy(const std::string& strategy) {
    std::string lower = strategy;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "weighted_round_robin") return BalanceStrategy::WEIGHTED_ROUND_ROBIN;
    if (lower == "least_connections") return BalanceStrategy::LEAST_CONNECTIONS;
    return BalanceStrategy::ROUND_ROBIN;
}
