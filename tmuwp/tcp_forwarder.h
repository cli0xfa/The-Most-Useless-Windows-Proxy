#pragma once

#include "iocp.h"
#include <string>
#include <atomic>


class TcpForwarder : public IForwarderCallback {
public:
    TcpForwarder();
    ~TcpForwarder();

    
    bool Initialize(const ForwardRule& rule, IOCPManager* iocpManager);

    
    void Stop();

    
    std::string GetBindAddress() const { return bindAddress_; }

    
    int GetConnectionCount() const { return connectionCount_.load(); }

    // IForwarderCallback 接口实现
    void OnClientData(SOCKET clientSocket, const char* data, int len) override;
    void OnServerData(SOCKET serverSocket, const char* data, int len) override;
    void OnClientDisconnect(SOCKET clientSocket) override;
    void OnServerDisconnect(SOCKET serverSocket) override;

private:
    ForwardRule rule_;
    IOCPManager* iocpManager_;
    SOCKET listenSocket_;
    std::string bindAddress_;
    std::atomic<int> connectionCount_;
    bool running_;

    
    bool CreateListener();
};
