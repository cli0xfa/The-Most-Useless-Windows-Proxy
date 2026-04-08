#include "tcp_forwarder.h"
#include <iostream>
#include <ws2tcpip.h>

TcpForwarder::TcpForwarder()
    : iocpManager_(nullptr)
    , listenSocket_(INVALID_SOCKET)
    , connectionCount_(0)
    , running_(false) {
}

TcpForwarder::~TcpForwarder() {
    Stop();
}

bool TcpForwarder::Initialize(const ForwardRule& rule, IOCPManager* iocpManager) {
    rule_ = rule;
    iocpManager_ = iocpManager;
    bindAddress_ = rule.bind;

    if (!CreateListener()) {
        return false;
    }

    running_ = true;

    
    if (!iocpManager_->RegisterListener(listenSocket_, rule_)) {
        std::cerr << "Failed to register listener with IOCP manager" << std::endl;
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    std::cout << "TCP forwarder '" << rule_.name << "' listening on " << bindAddress_ << std::endl;
    return true;
}

void TcpForwarder::Stop() {
    running_ = false;

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }
}

bool TcpForwarder::CreateListener() {
    std::string ip;
    int port;
    if (!ParseAddress(bindAddress_, ip, port)) {
        std::cerr << "Invalid bind address: " << bindAddress_ << std::endl;
        return false;
    }

    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    
    int reuse = 1;
    if (setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse)) == SOCKET_ERROR) {
        std::cerr << "setsockopt SO_REUSEADDR failed: " << WSAGetLastError() << std::endl;
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip == "0.0.0.0" || ip.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    }

    if (bind(listenSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind to " << bindAddress_ << ": " << WSAGetLastError() << std::endl;
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Failed to listen: " << WSAGetLastError() << std::endl;
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    return true;
}

void TcpForwarder::OnClientData(SOCKET clientSocket, const char* data, int len) {
    
    connectionCount_++;
}

void TcpForwarder::OnServerData(SOCKET serverSocket, const char* data, int len) {
    
}

void TcpForwarder::OnClientDisconnect(SOCKET clientSocket) {
    connectionCount_--;
}

void TcpForwarder::OnServerDisconnect(SOCKET serverSocket) {
    
}
