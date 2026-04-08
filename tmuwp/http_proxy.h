#pragma once

#include "iocp.h"
#include <string>
#include <map>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>

// HTTP parse state
enum class HttpParseState {
    REQUEST_LINE,
    HEADERS,
    BODY,
    COMPLETE,
    ERR
};

// HTTP request structure
struct HttpRequest {
    std::string method;
    std::string uri;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string rawData;
    HttpParseState state;

    HttpRequest() : state(HttpParseState::REQUEST_LINE) {}

    bool IsConnect() const { return method == "CONNECT"; }

    std::string GetHost() const {
        auto it = headers.find("Host");
        if (it != headers.end()) {
            return it->second;
        }
        return "";
    }
};

// HTTP connection context
struct HttpConnectionContext {
    SOCKET clientSocket;
    SOCKET serverSocket;
    bool handshakeComplete;
    bool isConnect;
    bool closing;
    HttpRequest request;
    char clientBuffer[8192];
    int clientBufferLen;

    HttpConnectionContext()
        : clientSocket(INVALID_SOCKET)
        , serverSocket(INVALID_SOCKET)
        , handshakeComplete(false)
        , isConnect(false)
        , closing(false)
        , clientBufferLen(0) {
        memset(clientBuffer, 0, sizeof(clientBuffer));
    }
};

// HTTP Proxy class
class HttpProxy : public IForwarderCallback {
public:
    HttpProxy();
    ~HttpProxy();

    bool Initialize(const ForwardRule& rule, IOCPManager* iocpManager);
    void Stop();

    // IForwarderCallback interface
    void OnClientData(SOCKET clientSocket, const char* data, int len) override;
    void OnServerData(SOCKET serverSocket, const char* data, int len) override;
    void OnClientDisconnect(SOCKET clientSocket) override;
    void OnServerDisconnect(SOCKET serverSocket) override;

private:
    bool CreateListener();
    void AcceptLoop();
    void ProcessClientData(SOCKET clientSocket, const char* data, int len);
    void ProcessServerData(SOCKET serverSocket, const char* data, int len);
    bool ParseHttpRequest(HttpRequest& request, const char* data, int len);
    bool ParseRequestLine(HttpRequest& request, const std::string& line);
    bool ParseHeader(HttpRequest& request, const std::string& line);
    SOCKET ConnectToTarget(const std::string& host, int port);
    bool ExtractTarget(const HttpRequest& request, std::string& host, int& port);
    void CloseConnection(SOCKET socket);
    void SendConnectResponse(SOCKET clientSocket);
    void SendErrorResponse(SOCKET clientSocket, int code, const std::string& message);

    ForwardRule rule_;
    IOCPManager* iocpManager_;
    SOCKET listenSocket_;
    std::atomic<bool> running_;
    std::atomic<int> connectionCount_;
    std::mutex connectionsMutex_;
    std::unordered_map<SOCKET, std::shared_ptr<HttpConnectionContext>> connections_;
};
