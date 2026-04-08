#include "utils.h"  // Must include first to ensure winsock2.h is included before windows.h

#include <iostream>
#include <vector>
#include <memory>
#include <csignal>
#include <thread>
#include "config.h"
#include "iocp.h"
#include "tcp_forwarder.h"
#include "http_proxy.h"
#include "socks5.h"
#include "logger.h"
#include "pool.h"


class IProxyServer {
public:
    virtual ~IProxyServer() = default;
    virtual void Stop() = 0;
};


class TcpForwarderWrapper : public IProxyServer {
public:
    TcpForwarderWrapper(std::unique_ptr<TcpForwarder> fwd) : forwarder_(std::move(fwd)) {}
    void Stop() override { if (forwarder_) forwarder_->Stop(); }
private:
    std::unique_ptr<TcpForwarder> forwarder_;
};


class HttpProxyWrapper : public IProxyServer {
public:
    HttpProxyWrapper(std::unique_ptr<HttpProxy> proxy) : proxy_(std::move(proxy)) {}
    void Stop() override { if (proxy_) proxy_->Stop(); }
private:
    std::unique_ptr<HttpProxy> proxy_;
};


class Socks5ProxyWrapper : public IProxyServer {
public:
    Socks5ProxyWrapper(std::unique_ptr<Socks5Proxy> proxy) : proxy_(std::move(proxy)) {}
    void Stop() override { if (proxy_) proxy_->Stop(); }
private:
    std::unique_ptr<Socks5Proxy> proxy_;
};

// Global variables
std::atomic<bool> g_running(true);
std::vector<std::unique_ptr<IProxyServer>> g_proxies;
std::unique_ptr<IOCPManager> g_iocpManager;


void StatsThread() {
    while (g_running) {
        Sleep(30000);  
        if (g_running) {
            LOG_INFO(StatsManager::GetInstance().GetStatsString());
        }
    }
}


void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        LOG_INFO("Shutting down...");
        g_running = false;
    }
}

// Print usage information
void PrintUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -c, --config <file>    Configuration file path (default: config.json)" << std::endl;
    std::cout << "  -h, --help             Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Proxy types: tcp, http, socks5" << std::endl;
    std::cout << std::endl;
    std::cout << "Example config.json:" << std::endl;
    std::cout << R"({
    "worker_threads": 4,
    "log_level": 1,
    "listeners": [
        {
            "name": "tcp_forward",
            "bind": "0.0.0.0:8080",
            "type": "tcp",
            "targets": ["192.168.1.100:80"],
            "balance": "round_robin"
        },
        {
            "name": "http_proxy",
            "bind": "0.0.0.0:8081",
            "type": "http"
        },
        {
            "name": "socks5_proxy",
            "bind": "0.0.0.0:1080",
            "type": "socks5"
        }
    ]
})" << std::endl;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    std::string configFile = "config.json";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    
    SetConsoleOutputCP(CP_UTF8);

    
    // Load config first
    Config config;
    if (!config.LoadFromFile(configFile)) {
        LOG_ERROR("Failed to load config: " + configFile);
        return 1;
    }

    LogLevel logLevel = static_cast<LogLevel>(config.GetLogLevel());
    Logger::GetInstance().Initialize(logLevel, "tmuwp.log");
    Logger::GetInstance().SetConsoleOutput(true);
    Logger::GetInstance().SetFileOutput(true);

    LOG_INFO("=== Windows Reverse Proxy Starting ===");

    
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    
    if (!InitWinsock()) {
        LOG_FATAL("Failed to initialize Winsock");
        return 1;
    }

    
    IOContextPool::GetInstance().Initialize(1000, 10000);
    LOG_INFO_FMT("IOContext pool initialized: initial=1000, max=10000");



    if (!config.LoadFromFile(configFile)) {
        LOG_ERROR_FMT("Failed to load configuration from: %s", configFile.c_str());
        LOG_INFO("Creating a sample config.json...");

        
        std::ofstream sampleFile("config.json");
        if (sampleFile.is_open()) {
            sampleFile << R"({
    "worker_threads": 4,
    "log_level": 1,
    "listeners": [
        {
            "name": "tcp_forward",
            "bind": "0.0.0.0:8080",
            "type": "tcp",
            "targets": ["127.0.0.1:80"],
            "balance": "round_robin"
        },
        {
            "name": "http_proxy",
            "bind": "0.0.0.0:8081",
            "type": "http"
        },
        {
            "name": "socks5_proxy",
            "bind": "0.0.0.0:1080",
            "type": "socks5"
        }
    ]
})" << std::endl;
            sampleFile.close();
            LOG_INFO("Sample config.json created. Please edit it and restart.");
        }

        CleanupWinsock();
        return 1;
    }

    LOG_INFO_FMT("Worker threads: %s",
        (config.GetWorkerThreads() > 0 ? std::to_string(config.GetWorkerThreads()).c_str() : "auto"));
    LOG_INFO_FMT("Listeners: %zu", config.GetRules().size());

    
    g_iocpManager = std::make_unique<IOCPManager>();
    if (!g_iocpManager->Initialize(config.GetWorkerThreads())) {
        LOG_FATAL("Failed to initialize IOCP manager");
        CleanupWinsock();
        return 1;
    }

    
    const auto& rules = config.GetRules();
    for (const auto& rule : rules) {
        bool initialized = false;

        switch (rule.type) {
        case ProxyType::TCP: {
            auto forwarder = std::make_unique<TcpForwarder>();
            if (forwarder->Initialize(rule, g_iocpManager.get())) {
                g_proxies.push_back(std::make_unique<TcpForwarderWrapper>(std::move(forwarder)));
                initialized = true;
            }
            break;
        }
        case ProxyType::HTTP: {
            auto proxy = std::make_unique<HttpProxy>();
            if (proxy->Initialize(rule, g_iocpManager.get())) {
                g_proxies.push_back(std::make_unique<HttpProxyWrapper>(std::move(proxy)));
                initialized = true;
            }
            break;
        }
        case ProxyType::SOCKS5: {
            auto proxy = std::make_unique<Socks5Proxy>();
            if (proxy->Initialize(rule, g_iocpManager.get())) {
                g_proxies.push_back(std::make_unique<Socks5ProxyWrapper>(std::move(proxy)));
                initialized = true;
            }
            break;
        }
        }

        if (!initialized) {
            LOG_ERROR_FMT("Failed to initialize proxy: %s", rule.name.c_str());
        }
    }

    if (g_proxies.empty()) {
        LOG_FATAL("No proxies initialized");
        g_iocpManager->Shutdown();
        CleanupWinsock();
        return 1;
    }

    LOG_INFO("Proxy server running. Press Ctrl+C to stop.");

    
    std::thread statsThread(StatsThread);

    
    while (g_running) {
        Sleep(1000);
    }

    
    if (statsThread.joinable()) {
        statsThread.join();
    }

    
    LOG_INFO("Stopping proxies...");
    for (auto& proxy : g_proxies) {
        proxy->Stop();
    }
    g_proxies.clear();

    std::cout << "Shutting down IOCP..." << std::endl;
    g_iocpManager->Shutdown();
    g_iocpManager.reset();

    LOG_INFO("Cleaning up Winsock...");
    CleanupWinsock();

    LOG_INFO("Done.");
    return 0;
}
