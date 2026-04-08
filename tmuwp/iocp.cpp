#include "iocp.h"
#include <ws2tcpip.h>
#include <iostream>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

IOCPManager::IOCPManager()
    : iocpHandle_(INVALID_HANDLE_VALUE)
    , running_(false)
    , callback_(nullptr) {
}

IOCPManager::~IOCPManager() {
    Shutdown();
}

bool IOCPManager::Initialize(int workerThreads) {
    if (workerThreads <= 0) {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        workerThreads = sysInfo.dwNumberOfProcessors * 2;
    }

    
    iocpHandle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, workerThreads);
    if (iocpHandle_ == NULL) {
        std::cerr << "CreateIoCompletionPort failed: " << GetLastError() << std::endl;
        return false;
    }

    running_ = true;

    // 启动工作线程
    for (int i = 0; i < workerThreads; i++) {
        workerThreads_.emplace_back(&IOCPManager::WorkerThread, this);
    }

    std::cout << "IOCP initialized with " << workerThreads << " worker threads" << std::endl;
    return true;
}

void IOCPManager::Shutdown() {
    running_ = false;

    
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& pair : connections_) {
            auto& ctx = pair.second;
            if (ctx->clientSock != INVALID_SOCKET) {
                closesocket(ctx->clientSock);
            }
            if (ctx->serverSock != INVALID_SOCKET) {
                closesocket(ctx->serverSock);
            }
        }
        connections_.clear();
    }

    
    if (iocpHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(iocpHandle_);
        iocpHandle_ = INVALID_HANDLE_VALUE;
    }

    // 等待工作线程结束
    for (auto& thread : workerThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    workerThreads_.clear();

    std::cout << "IOCP shutdown complete" << std::endl;
}

bool IOCPManager::RegisterListener(SOCKET listenSocket, const ForwardRule& rule) {
    // Associate listen socket with IOCP
    if (CreateIoCompletionPort((HANDLE)listenSocket, iocpHandle_, (ULONG_PTR)listenSocket, 0) == NULL) {
        std::cerr << "Failed to associate listen socket with IOCP" << std::endl;
        return false;
    }

    listenerRules_[listenSocket] = rule;

    
    HandleAccept(listenSocket);

    return true;
}

bool IOCPManager::RegisterConnection(SOCKET clientSocket, SOCKET serverSocket, IForwarderCallback* callback) {
    // Associate with IOCP
    if (CreateIoCompletionPort((HANDLE)clientSocket, iocpHandle_, (ULONG_PTR)clientSocket, 0) == NULL) {
        return false;
    }
    if (CreateIoCompletionPort((HANDLE)serverSocket, iocpHandle_, (ULONG_PTR)serverSocket, 0) == NULL) {
        return false;
    }

    auto ctx = std::make_shared<ConnectionContext>();
    ctx->clientSock = clientSocket;
    ctx->serverSock = serverSocket;
    ctx->clientToServer->sock = clientSocket;
    ctx->clientToServer->remoteSock = serverSocket;
    ctx->serverToClient->sock = serverSocket;
    ctx->serverToClient->remoteSock = clientSocket;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_[clientSocket] = ctx;
        connections_[serverSocket] = ctx;
    }

    
    ctx->clientToServer->operation = IOOperation::READ;
    ctx->clientToServer->inUse = true;
    DWORD flags = 0;
    int result = WSARecv(clientSocket, &ctx->clientToServer->wsaBuf, 1, NULL, &flags,
        &ctx->clientToServer->overlapped, NULL);
    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        std::cerr << "WSARecv failed: " << WSAGetLastError() << std::endl;
        CloseConnection(clientSocket);
        return false;
    }

    ctx->serverToClient->operation = IOOperation::READ;
    ctx->serverToClient->inUse = true;
    result = WSARecv(serverSocket, &ctx->serverToClient->wsaBuf, 1, NULL, &flags,
        &ctx->serverToClient->overlapped, NULL);
    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        std::cerr << "WSARecv failed: " << WSAGetLastError() << std::endl;
        CloseConnection(clientSocket);
        return false;
    }

    return true;
}

void IOCPManager::CloseConnection(SOCKET socket) {
    std::shared_ptr<ConnectionContext> ctx;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = connections_.find(socket);
        if (it == connections_.end()) {
            return;
        }
        ctx = it->second;
        connections_.erase(it);
        if (ctx->clientSock != socket) {
            connections_.erase(ctx->clientSock);
        } else {
            connections_.erase(ctx->serverSock);
        }
    }

    if (ctx->closing) return;
    ctx->closing = true;

    if (ctx->clientSock != INVALID_SOCKET) {
        closesocket(ctx->clientSock);
        ctx->clientSock = INVALID_SOCKET;
    }
    if (ctx->serverSock != INVALID_SOCKET) {
        closesocket(ctx->serverSock);
        ctx->serverSock = INVALID_SOCKET;
    }
}

bool IOCPManager::PostRecv(IOContext* context) {
    context->Reset();
    context->inUse = true;
    context->operation = IOOperation::READ;

    DWORD flags = 0;
    int result = WSARecv(context->sock, &context->wsaBuf, 1, NULL, &flags,
        &context->overlapped, NULL);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        context->inUse = false;
        return false;
    }
    return true;
}

bool IOCPManager::PostSend(IOContext* context, const char* data, int len) {
    if (len > sizeof(context->buffer)) {
        len = sizeof(context->buffer);
    }

    memcpy(context->buffer, data, len);
    context->wsaBuf.buf = context->buffer;
    context->wsaBuf.len = len;
    context->inUse = true;
    context->operation = IOOperation::WRITE;

    DWORD bytesSent = 0;
    int result = WSASend(context->remoteSock, &context->wsaBuf, 1, &bytesSent, 0,
        &context->overlapped, NULL);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        context->inUse = false;
        return false;
    }
    return true;
}

std::string IOCPManager::GetNextTarget(const std::string& ruleName) {
    std::lock_guard<std::mutex> lock(balanceMutex_);

    
    const ForwardRule* rule = nullptr;
    for (const auto& pair : listenerRules_) {
        if (pair.second.name == ruleName) {
            rule = &pair.second;
            break;
        }
    }

    if (!rule || rule->targets.empty()) {
        return "";
    }

    // 简单的轮询
    size_t& index = roundRobinIndex_[ruleName];
    std::string target = rule->targets[index % rule->targets.size()];
    index++;

    return target;
}

void IOCPManager::WorkerThread() {
    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    LPOVERLAPPED lpOverlapped = NULL;

    while (running_) {
        BOOL result = GetQueuedCompletionStatus(
            iocpHandle_,
            &bytesTransferred,
            &completionKey,
            &lpOverlapped,
            INFINITE
        );

        if (!running_) break;

        if (!result) {
            DWORD error = GetLastError();
            if (lpOverlapped == NULL) {
                continue;
            }
            
            IOContext* ctx = CONTAINING_RECORD(lpOverlapped, IOContext, overlapped);
            CloseConnection(ctx->sock);
            continue;
        }

        IOContext* context = CONTAINING_RECORD(lpOverlapped, IOContext, overlapped);
        HandleIOCompletion(context, bytesTransferred, result);
    }
}

void IOCPManager::HandleIOCompletion(IOContext* context, DWORD bytesTransferred, BOOL success) {
    if (!success || bytesTransferred == 0) {
        
        CloseConnection(context->sock);
        return;
    }

    switch (context->operation) {
    case IOOperation::READ: {
        // 收到数据，转发到对端
        if (callback_) {
            if (context->sock == context->remoteSock) {
                callback_->OnServerData(context->sock, context->buffer, bytesTransferred);
            } else {
                callback_->OnClientData(context->sock, context->buffer, bytesTransferred);
            }
        }

        // 转发数据
        if (!PostSend(context, context->buffer, bytesTransferred)) {
            CloseConnection(context->sock);
            return;
        }
        break;
    }

    case IOOperation::WRITE: {
        
        context->inUse = false;
        if (!PostRecv(context)) {
            CloseConnection(context->sock);
        }
        break;
    }

    default:
        break;
    }
}

void IOCPManager::HandleAccept(SOCKET listenSocket) {
    
    std::thread([this, listenSocket]() {
        while (running_) {
            sockaddr_in clientAddr;
            int addrLen = sizeof(clientAddr);
            SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &addrLen);

            if (clientSocket == INVALID_SOCKET) {
                if (running_) {
                    std::cerr << "accept failed: " << WSAGetLastError() << std::endl;
                }
                continue;
            }

            
            auto it = listenerRules_.find(listenSocket);
            if (it == listenerRules_.end()) {
                closesocket(clientSocket);
                continue;
            }

            const ForwardRule& rule = it->second;

            
            std::string target = GetNextTarget(rule.name);
            if (target.empty()) {
                closesocket(clientSocket);
                continue;
            }

            SOCKET serverSocket = ConnectToTarget(target);
            if (serverSocket == INVALID_SOCKET) {
                closesocket(clientSocket);
                continue;
            }

            
            if (!RegisterConnection(clientSocket, serverSocket, callback_)) {
                closesocket(clientSocket);
                closesocket(serverSocket);
            }

            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
            std::cout << "New connection from " << clientIp << ":" << ntohs(clientAddr.sin_port)
                << " -> " << target << std::endl;
        }
        }).detach();
}

SOCKET IOCPManager::ConnectToTarget(const std::string& target) {
    std::string ip;
    int port;
    if (!ParseAddress(target, ip, port)) {
        return INVALID_SOCKET;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    int result = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            closesocket(sock);
            return INVALID_SOCKET;
        }

        
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);

        timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        result = select(0, NULL, &writefds, NULL, &timeout);
        if (result <= 0 || !FD_ISSET(sock, &writefds)) {
            closesocket(sock);
            return INVALID_SOCKET;
        }

        
        int so_error;
        int len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error != 0) {
            closesocket(sock);
            return INVALID_SOCKET;
        }
    }

    return sock;
}
