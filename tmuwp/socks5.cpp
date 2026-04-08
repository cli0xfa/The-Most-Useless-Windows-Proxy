#include "socks5.h"
#include <iostream>
#include <ws2tcpip.h>
#include <thread>

Socks5Proxy::Socks5Proxy()
    : iocpManager_(nullptr)
    , listenSocket_(INVALID_SOCKET)
    , running_(false)
    , connectionCount_(0) {
}

Socks5Proxy::~Socks5Proxy() {
    Stop();
}

bool Socks5Proxy::Initialize(const ForwardRule& rule, IOCPManager* iocpManager) {
    rule_ = rule;
    iocpManager_ = iocpManager;

    if (!CreateListener()) {
        return false;
    }

    running_ = true;
    std::thread(&Socks5Proxy::AcceptLoop, this).detach();

    std::cout << "SOCKS5 proxy '" << rule_.name << "' listening on " << rule_.bind << std::endl;
    return true;
}

void Socks5Proxy::Stop() {
    running_ = false;

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (auto& pair : connections_) {
        auto& ctx = pair.second;
        if (ctx->clientSocket != INVALID_SOCKET) {
            closesocket(ctx->clientSocket);
        }
        if (ctx->serverSocket != INVALID_SOCKET) {
            closesocket(ctx->serverSocket);
        }
    }
    connections_.clear();
}

bool Socks5Proxy::CreateListener() {
    std::string ip;
    int port;
    if (!ParseAddress(rule_.bind, ip, port)) {
        std::cerr << "Invalid bind address: " << rule_.bind << std::endl;
        return false;
    }

    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    int reuse = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (ip == "0.0.0.0" || ip.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    }

    if (bind(listenSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind: " << WSAGetLastError() << std::endl;
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

void Socks5Proxy::AcceptLoop() {
    while (running_) {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket_, (sockaddr*)&clientAddr, &addrLen);

        if (clientSocket == INVALID_SOCKET) {
            if (running_) {
                std::cerr << "accept failed: " << WSAGetLastError() << std::endl;
            }
            continue;
        }

        u_long mode = 1;
        ioctlsocket(clientSocket, FIONBIO, &mode);

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        std::cout << "SOCKS5: New connection from " << clientIp << ":" << ntohs(clientAddr.sin_port) << std::endl;

        auto ctx = std::make_shared<Socks5ConnectionContext>();
        ctx->clientSocket = clientSocket;

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_[clientSocket] = ctx;
        }

        connectionCount_++;

        
        auto ioContext = new IOContext();
        ioContext->sock = clientSocket;
        ioContext->operation = IOOperation::READ;
        ioContext->remoteSock = INVALID_SOCKET;

        DWORD flags = 0;
        int result = WSARecv(clientSocket, &ioContext->wsaBuf, 1, NULL, &flags,
            &ioContext->overlapped, NULL);

        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            delete ioContext;
            CloseConnection(clientSocket);
        }
    }
}

void Socks5Proxy::OnClientData(SOCKET clientSocket, const char* data, int len) {
    HandleHandshake(clientSocket, data, len);
}

void Socks5Proxy::OnServerData(SOCKET serverSocket, const char* data, int len) {
    std::shared_ptr<Socks5ConnectionContext> ctx;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& pair : connections_) {
            if (pair.second->serverSocket == serverSocket) {
                ctx = pair.second;
                break;
            }
        }
    }

    if (ctx && !ctx->closing && ctx->clientSocket != INVALID_SOCKET) {
        send(ctx->clientSocket, data, len, 0);
    }
}

void Socks5Proxy::OnClientDisconnect(SOCKET clientSocket) {
    CloseConnection(clientSocket);
    connectionCount_--;
}

void Socks5Proxy::OnServerDisconnect(SOCKET serverSocket) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (auto& pair : connections_) {
        if (pair.second->serverSocket == serverSocket) {
            CloseConnection(pair.second->clientSocket);
            break;
        }
    }
}

void Socks5Proxy::HandleHandshake(SOCKET clientSocket, const char* data, int len) {
    std::shared_ptr<Socks5ConnectionContext> ctx;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(clientSocket);
        if (it == connections_.end()) {
            return;
        }
        ctx = it->second;
    }

    if (ctx->closing) {
        return;
    }

    // Accumulate data
    if (ctx->bufferLen + len < sizeof(ctx->buffer)) {
        memcpy(ctx->buffer + ctx->bufferLen, data, len);
        ctx->bufferLen += len;
    } else {
        CloseConnection(clientSocket);
        return;
    }

    int consumed = 0;
    bool success = false;

    switch (ctx->state) {
    case Socks5::HandshakeState::METHOD_SELECTION:
        success = HandleMethodSelection(clientSocket, ctx->buffer, ctx->bufferLen, consumed);
        break;

    case Socks5::HandshakeState::REQUEST:
        success = HandleConnectRequest(clientSocket, ctx->buffer, ctx->bufferLen, consumed);
        break;

    case Socks5::HandshakeState::RELAYING:
        // Should not receive callback in relay mode
        break;

    default:
        CloseConnection(clientSocket);
        return;
    }

    if (consumed > 0 && consumed < ctx->bufferLen) {
        memmove(ctx->buffer, ctx->buffer + consumed, ctx->bufferLen - consumed);
        ctx->bufferLen -= consumed;
    } else if (consumed >= ctx->bufferLen) {
        ctx->bufferLen = 0;
    }

    if (!success) {
        CloseConnection(clientSocket);
    }
}

bool Socks5Proxy::HandleMethodSelection(SOCKET clientSocket, const char* data, int len, int& consumed) {
    consumed = 0;

    if (len < 2) return true;  // Wait for more data

    uint8_t ver = data[0];
    if (ver != Socks5::VERSION) {
        std::cerr << "SOCKS5: Invalid version: " << (int)ver << std::endl;
        return false;
    }

    uint8_t nmethods = data[1];
    if (len < 2 + nmethods) return true;  // Wait for more data

    // Find supported auth method
    uint8_t selectedMethod = Socks5::AUTH_NO_ACCEPTABLE;
    for (int i = 0; i < nmethods; i++) {
        uint8_t method = data[2 + i];
        if (method == Socks5::AUTH_NONE) {
            selectedMethod = Socks5::AUTH_NONE;
            break;
        }
    }

    if (selectedMethod == Socks5::AUTH_NO_ACCEPTABLE) {
        std::cerr << "SOCKS5: No acceptable authentication method" << std::endl;
        SendMethodSelection(clientSocket, Socks5::AUTH_NO_ACCEPTABLE);
        return false;
    }

    SendMethodSelection(clientSocket, selectedMethod);
    consumed = 2 + nmethods;

    std::shared_ptr<Socks5ConnectionContext> ctx;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        ctx = connections_[clientSocket];
    }
    ctx->authMethod = selectedMethod;
    ctx->state = Socks5::HandshakeState::REQUEST;

    return true;
}

bool Socks5Proxy::HandleConnectRequest(SOCKET clientSocket, const char* data, int len, int& consumed) {
    consumed = 0;

    if (len < 10) return true;  // Minimum length check

    uint8_t ver = data[0];
    uint8_t cmd = data[1];
    uint8_t rsv = data[2];  // Reserved field
    uint8_t atyp = data[3];

    if (ver != Socks5::VERSION) {
        return false;
    }

    // Only support CONNECT command
    if (cmd != Socks5::CMD_CONNECT) {
        SendConnectResponse(clientSocket, Socks5::REP_COMMAND_NOT_SUPPORTED, "0.0.0.0", 0);
        return false;
    }

    std::string targetHost;
    int targetPort = 0;
    int headerLen = 0;

    switch (atyp) {
    case Socks5::ATYP_IPV4: {
        if (len < 10) return true;
        char ipStr[INET_ADDRSTRLEN];
        sprintf_s(ipStr, "%d.%d.%d.%d",
            (uint8_t)data[4], (uint8_t)data[5], (uint8_t)data[6], (uint8_t)data[7]);
        targetHost = ipStr;
        targetPort = ((uint8_t)data[8] << 8) | (uint8_t)data[9];
        headerLen = 10;
        break;
    }

    case Socks5::ATYP_DOMAIN: {
        uint8_t domainLen = data[4];
        if (len < 5 + domainLen + 2) return true;
        targetHost = std::string(data + 5, domainLen);
        targetPort = ((uint8_t)data[5 + domainLen] << 8) | (uint8_t)data[5 + domainLen + 1];
        headerLen = 5 + domainLen + 2;
        break;
    }

    case Socks5::ATYP_IPV6: {
        SendConnectResponse(clientSocket, Socks5::REP_ADDRESS_TYPE_NOT_SUPPORTED, "0.0.0.0", 0);
        return false;
    }

    default:
        SendConnectResponse(clientSocket, Socks5::REP_ADDRESS_TYPE_NOT_SUPPORTED, "0.0.0.0", 0);
        return false;
    }

    std::cout << "SOCKS5: CONNECT request to " << targetHost << ":" << targetPort << std::endl;

    
    if (!rule_.targets.empty()) {
        
        std::string target = rule_.targets[0];
        size_t pos = target.rfind(':');
        if (pos != std::string::npos) {
            try {
                targetPort = std::stoi(target.substr(pos + 1));
            } catch (...) {}
            targetHost = target.substr(0, pos);
        } else {
            targetHost = target;
        }
    }

    
    SOCKET serverSocket = ConnectToTarget(targetHost, targetPort);
    if (serverSocket == INVALID_SOCKET) {
        SendConnectResponse(clientSocket, Socks5::REP_HOST_UNREACHABLE, "0.0.0.0", 0);
        return false;
    }

    // Send success response
    SendConnectResponse(clientSocket, Socks5::REP_SUCCESS, "0.0.0.0", 0);

    consumed = headerLen;

    // Enter relay mode
    StartRelay(clientSocket, serverSocket);

    return true;
}

SOCKET Socks5Proxy::ConnectToTarget(const std::string& host, int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        freeaddrinfo(result);
        closesocket(sock);
        return INVALID_SOCKET;
    }

    freeaddrinfo(result);

    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    return sock;
}

void Socks5Proxy::SendMethodSelection(SOCKET clientSocket, uint8_t method) {
    char response[2] = { Socks5::VERSION, (char)method };
    send(clientSocket, response, 2, 0);
}

void Socks5Proxy::SendConnectResponse(SOCKET clientSocket, uint8_t reply, const std::string& host, int port) {
    char response[10];
    response[0] = Socks5::VERSION;
    response[1] = reply;
    response[2] = 0x00;  // RSV
    response[3] = Socks5::ATYP_IPV4;
    response[4] = 0;
    response[5] = 0;
    response[6] = 0;
    response[7] = 0;
    response[8] = (port >> 8) & 0xFF;
    response[9] = port & 0xFF;
    send(clientSocket, response, 10, 0);
}

void Socks5Proxy::CloseConnection(SOCKET socket) {
    std::shared_ptr<Socks5ConnectionContext> ctx;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(socket);
        if (it == connections_.end()) {
            return;
        }
        ctx = it->second;
        connections_.erase(it);
    }

    if (ctx->closing) return;
    ctx->closing = true;

    if (ctx->clientSocket != INVALID_SOCKET) {
        closesocket(ctx->clientSocket);
        ctx->clientSocket = INVALID_SOCKET;
    }
    if (ctx->serverSocket != INVALID_SOCKET) {
        closesocket(ctx->serverSocket);
        ctx->serverSocket = INVALID_SOCKET;
    }
}

void Socks5Proxy::StartRelay(SOCKET clientSocket, SOCKET serverSocket) {
    std::shared_ptr<Socks5ConnectionContext> ctx;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        ctx = connections_[clientSocket];
        if (!ctx) return;
        ctx->serverSocket = serverSocket;
        ctx->state = Socks5::HandshakeState::RELAYING;
    }

    std::cout << "SOCKS5: Starting relay mode" << std::endl;

    
    if (iocpManager_) {
        iocpManager_->RegisterConnection(clientSocket, serverSocket, this);
    }
}
