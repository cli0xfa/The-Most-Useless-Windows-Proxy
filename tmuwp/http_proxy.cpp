#include "http_proxy.h"
#include <iostream>
#include <sstream>
#include <ws2tcpip.h>
#include <thread>

HttpProxy::HttpProxy()
    : iocpManager_(nullptr)
    , listenSocket_(INVALID_SOCKET)
    , running_(false)
    , connectionCount_(0) {
}

HttpProxy::~HttpProxy() {
    Stop();
}

bool HttpProxy::Initialize(const ForwardRule& rule, IOCPManager* iocpManager) {
    rule_ = rule;
    iocpManager_ = iocpManager;

    if (!CreateListener()) {
        return false;
    }

    running_ = true;

    std::thread(&HttpProxy::AcceptLoop, this).detach();

    std::cout << "HTTP proxy '" << rule_.name << "' listening on " << rule_.bind << std::endl;
    return true;
}

void HttpProxy::Stop() {
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

bool HttpProxy::CreateListener() {
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

void HttpProxy::AcceptLoop() {
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
        std::cout << "HTTP proxy: New connection from " << clientIp << ":" << ntohs(clientAddr.sin_port) << std::endl;

        auto ctx = std::make_shared<HttpConnectionContext>();
        ctx->clientSocket = clientSocket;

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_[clientSocket] = ctx;
        }

        connectionCount_++;

        auto ioContext = new IOContext();
        ioContext->sock = clientSocket;
        ioContext->remoteSock = INVALID_SOCKET;
        ioContext->operation = IOOperation::READ;

        DWORD flags = 0;
        int result = WSARecv(clientSocket, &ioContext->wsaBuf, 1, NULL, &flags,
            &ioContext->overlapped, NULL);

        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            std::cerr << "WSARecv failed: " << WSAGetLastError() << std::endl;
            delete ioContext;
            CloseConnection(clientSocket);
        }
    }
}

void HttpProxy::OnClientData(SOCKET clientSocket, const char* data, int len) {
    ProcessClientData(clientSocket, data, len);
}

void HttpProxy::OnServerData(SOCKET serverSocket, const char* data, int len) {
    ProcessServerData(serverSocket, data, len);
}

void HttpProxy::OnClientDisconnect(SOCKET clientSocket) {
    CloseConnection(clientSocket);
    connectionCount_--;
}

void HttpProxy::OnServerDisconnect(SOCKET serverSocket) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (auto& pair : connections_) {
        if (pair.second->serverSocket == serverSocket) {
            CloseConnection(pair.second->clientSocket);
            break;
        }
    }
}

void HttpProxy::ProcessClientData(SOCKET clientSocket, const char* data, int len) {
    std::shared_ptr<HttpConnectionContext> ctx;
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

    if (ctx->handshakeComplete) {
        if (ctx->serverSocket != INVALID_SOCKET) {
            send(ctx->serverSocket, data, len, 0);
        }
        return;
    }

    if (ctx->clientBufferLen + len < sizeof(ctx->clientBuffer)) {
        memcpy(ctx->clientBuffer + ctx->clientBufferLen, data, len);
        ctx->clientBufferLen += len;
    } else {
        SendErrorResponse(clientSocket, 413, "Payload Too Large");
        CloseConnection(clientSocket);
        return;
    }

    if (!ParseHttpRequest(ctx->request, ctx->clientBuffer, ctx->clientBufferLen)) {
        SendErrorResponse(clientSocket, 400, "Bad Request");
        CloseConnection(clientSocket);
        return;
    }

    if (ctx->request.state == HttpParseState::COMPLETE) {
        std::string host;
        int port;

        if (!ExtractTarget(ctx->request, host, port)) {
            SendErrorResponse(clientSocket, 400, "Bad Request: Cannot determine target");
            CloseConnection(clientSocket);
            return;
        }

        if (!rule_.targets.empty()) {
            host = rule_.targets[0];
            port = 80;
            size_t pos = host.rfind(':');
            if (pos != std::string::npos) {
                try {
                    port = std::stoi(host.substr(pos + 1));
                    host = host.substr(0, pos);
                } catch (...) {}
            }
        }

        SOCKET serverSocket = ConnectToTarget(host, port);
        if (serverSocket == INVALID_SOCKET) {
            SendErrorResponse(clientSocket, 502, "Bad Gateway: Cannot connect to target");
            CloseConnection(clientSocket);
            return;
        }

        ctx->serverSocket = serverSocket;

        if (iocpManager_) {
            iocpManager_->RegisterConnection(clientSocket, serverSocket, this);
        }

        if (ctx->request.IsConnect()) {
            ctx->isConnect = true;
            ctx->handshakeComplete = true;
            SendConnectResponse(clientSocket);
            std::cout << "HTTP proxy: CONNECT tunnel established to " << host << ":" << port << std::endl;
        } else {
            ctx->handshakeComplete = true;

            std::string requestLine;
            if (ctx->request.uri.find("http://") == 0) {
                size_t pos = ctx->request.uri.find('/', 7);
                if (pos != std::string::npos) {
                    requestLine = ctx->request.method + " " + ctx->request.uri.substr(pos) + " " + ctx->request.version + "\r\n";
                } else {
                    requestLine = ctx->request.method + " / " + ctx->request.version + "\r\n";
                }
            } else {
                requestLine = ctx->request.rawData.substr(0, ctx->request.rawData.find("\r\n") + 2);
            }

            std::string forwardData = requestLine + ctx->request.rawData.substr(ctx->request.rawData.find("\r\n") + 2);
            send(serverSocket, forwardData.c_str(), (int)forwardData.length(), 0);

            std::cout << "HTTP proxy: " << ctx->request.method << " " << ctx->request.uri << " -> " << host << ":" << port << std::endl;
        }
    }
}

void HttpProxy::ProcessServerData(SOCKET serverSocket, const char* data, int len) {
    std::shared_ptr<HttpConnectionContext> ctx;
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

bool HttpProxy::ParseHttpRequest(HttpRequest& request, const char* data, int len) {
    request.rawData.append(data, len);

    size_t pos = 0;
    while (request.state != HttpParseState::COMPLETE && request.state != HttpParseState::ERR) {
        size_t lineEnd = request.rawData.find("\r\n", pos);
        if (lineEnd == std::string::npos) {
            break;
        }

        std::string line = request.rawData.substr(pos, lineEnd - pos);
        pos = lineEnd + 2;

        if (request.state == HttpParseState::REQUEST_LINE) {
            if (!ParseRequestLine(request, line)) {
                request.state = HttpParseState::ERR;
                return false;
            }
            request.state = HttpParseState::HEADERS;
        } else if (request.state == HttpParseState::HEADERS) {
            if (line.empty()) {
                if (request.IsConnect()) {
                    request.state = HttpParseState::COMPLETE;
                } else {
                    auto it = request.headers.find("Content-Length");
                    if (it != request.headers.end()) {
                        try {
                            int contentLength = std::stoi(it->second);
                            if (contentLength > 0) {
                                request.state = HttpParseState::BODY;
                            } else {
                                request.state = HttpParseState::COMPLETE;
                            }
                        } catch (...) {
                            request.state = HttpParseState::COMPLETE;
                        }
                    } else {
                        request.state = HttpParseState::COMPLETE;
                    }
                }
            } else {
                if (!ParseHeader(request, line)) {
                    request.state = HttpParseState::ERR;
                    return false;
                }
            }
        } else if (request.state == HttpParseState::BODY) {
            request.state = HttpParseState::COMPLETE;
        }
    }

    return true;
}

bool HttpProxy::ParseRequestLine(HttpRequest& request, const std::string& line) {
    std::istringstream iss(line);
    if (!(iss >> request.method >> request.uri >> request.version)) {
        return false;
    }
    return true;
}

bool HttpProxy::ParseHeader(HttpRequest& request, const std::string& line) {
    size_t pos = line.find(':');
    if (pos == std::string::npos) {
        return false;
    }

    std::string name = line.substr(0, pos);
    std::string value = line.substr(pos + 1);

    while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
        value = value.substr(1);
    }

    request.headers[name] = value;
    return true;
}

SOCKET HttpProxy::ConnectToTarget(const std::string& host, int port) {
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

bool HttpProxy::ExtractTarget(const HttpRequest& request, std::string& host, int& port) {
    port = 80;

    if (request.IsConnect()) {
        size_t pos = request.uri.rfind(':');
        if (pos != std::string::npos) {
            host = request.uri.substr(0, pos);
            try {
                port = std::stoi(request.uri.substr(pos + 1));
            } catch (...) {
                port = 443;
            }
        } else {
            host = request.uri;
            port = 443;
        }
        return true;
    }

    host = request.GetHost();
    if (!host.empty()) {
        size_t pos = host.rfind(':');
        if (pos != std::string::npos) {
            try {
                port = std::stoi(host.substr(pos + 1));
            } catch (...) {}
            host = host.substr(0, pos);
        }
        return true;
    }

    if (request.uri.find("http://") == 0) {
        std::string url = request.uri.substr(7);
        size_t pathPos = url.find('/');
        std::string hostPort = (pathPos != std::string::npos) ? url.substr(0, pathPos) : url;

        size_t portPos = hostPort.rfind(':');
        if (portPos != std::string::npos) {
            host = hostPort.substr(0, portPos);
            try {
                port = std::stoi(hostPort.substr(portPos + 1));
            } catch (...) {}
        } else {
            host = hostPort;
        }
        return true;
    }

    return false;
}

void HttpProxy::CloseConnection(SOCKET socket) {
    std::shared_ptr<HttpConnectionContext> ctx;
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

void HttpProxy::SendConnectResponse(SOCKET clientSocket) {
    const char* response = "HTTP/1.1 200 Connection established\r\n\r\n";
    send(clientSocket, response, (int)strlen(response), 0);
}

void HttpProxy::SendErrorResponse(SOCKET clientSocket, int code, const std::string& message) {
    std::string response = "HTTP/1.1 " + std::to_string(code) + " " + message +
                           "\r\nContent-Type: text/plain\r\n"
                           "Connection: close\r\n\r\n" +
                           message + "\n";
    send(clientSocket, response.c_str(), (int)response.length(), 0);
}
