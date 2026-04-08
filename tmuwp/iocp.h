#pragma once

#include "utils.h"
#include "config.h"
#include <windows.h>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <memory>

// Forward declaration
class IOCPManager;

// Callback interface for forwarder events
class IForwarderCallback {
public:
    virtual ~IForwarderCallback() = default;
    virtual void OnClientData(SOCKET clientSocket, const char* data, int len) = 0;
    virtual void OnServerData(SOCKET serverSocket, const char* data, int len) = 0;
    virtual void OnClientDisconnect(SOCKET clientSocket) = 0;
    virtual void OnServerDisconnect(SOCKET serverSocket) = 0;
};

// IOCP Manager class for handling async I/O
class IOCPManager {
public:
    IOCPManager();
    ~IOCPManager();

    bool Initialize(int workerThreads = 0);
    void Shutdown();

    bool RegisterListener(SOCKET listenSocket, const ForwardRule& rule);
    bool RegisterConnection(SOCKET clientSocket, SOCKET serverSocket, IForwarderCallback* callback);
    void CloseConnection(SOCKET socket);

    bool PostRecv(IOContext* context);
    bool PostSend(IOContext* context, const char* data, int len);

    std::string GetNextTarget(const std::string& ruleName);

    void SetCallback(IForwarderCallback* callback) { callback_ = callback; }

private:
    void WorkerThread();
    void HandleIOCompletion(IOContext* context, DWORD bytesTransferred, BOOL success);
    void HandleAccept(SOCKET listenSocket);
    SOCKET ConnectToTarget(const std::string& target);

    HANDLE iocpHandle_;
    std::atomic<bool> running_;
    std::vector<std::thread> workerThreads_;
    IForwarderCallback* callback_;

    std::unordered_map<SOCKET, std::shared_ptr<ConnectionContext>> connections_;
    std::unordered_map<SOCKET, ForwardRule> listenerRules_;
    std::unordered_map<std::string, size_t> roundRobinIndex_;
    std::mutex connectionsMutex_;
    std::mutex balanceMutex_;
};
