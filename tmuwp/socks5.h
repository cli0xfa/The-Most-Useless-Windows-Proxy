#pragma once

#include "iocp.h"
#include <cstdint>
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>

// SOCKS5 protocol constants
namespace Socks5 {
    constexpr uint8_t VERSION = 0x05;
    constexpr uint8_t AUTH_NONE = 0x00;
    constexpr uint8_t AUTH_GSSAPI = 0x01;
    constexpr uint8_t AUTH_PASSWORD = 0x02;
    constexpr uint8_t AUTH_NO_ACCEPTABLE = 0xFF;

    constexpr uint8_t CMD_CONNECT = 0x01;
    constexpr uint8_t CMD_BIND = 0x02;
    constexpr uint8_t CMD_UDP_ASSOCIATE = 0x03;

    constexpr uint8_t ATYP_IPV4 = 0x01;
    constexpr uint8_t ATYP_DOMAIN = 0x03;
    constexpr uint8_t ATYP_IPV6 = 0x04;

    constexpr uint8_t REP_SUCCESS = 0x00;
    constexpr uint8_t REP_GENERAL_FAILURE = 0x01;
    constexpr uint8_t REP_CONNECTION_NOT_ALLOWED = 0x02;
    constexpr uint8_t REP_NETWORK_UNREACHABLE = 0x03;
    constexpr uint8_t REP_HOST_UNREACHABLE = 0x04;
    constexpr uint8_t REP_CONNECTION_REFUSED = 0x05;
    constexpr uint8_t REP_TTL_EXPIRED = 0x06;
    constexpr uint8_t REP_COMMAND_NOT_SUPPORTED = 0x07;
    constexpr uint8_t REP_ADDRESS_TYPE_NOT_SUPPORTED = 0x08;

    enum class HandshakeState {
        METHOD_SELECTION,
        AUTHENTICATION,
        REQUEST,
        CONNECTING,
        RELAYING,
        HS_ERROR
    };
}

// SOCKS5 connection context
struct Socks5ConnectionContext {
    SOCKET clientSocket;
    SOCKET serverSocket;
    bool closing;
    Socks5::HandshakeState state;
    uint8_t authMethod;
    char buffer[8192];
    int bufferLen;

    Socks5ConnectionContext()
        : clientSocket(INVALID_SOCKET)
        , serverSocket(INVALID_SOCKET)
        , closing(false)
        , state(Socks5::HandshakeState::METHOD_SELECTION)
        , authMethod(0)
        , bufferLen(0) {
        memset(buffer, 0, sizeof(buffer));
    }
};

// SOCKS5 Proxy class
class Socks5Proxy : public IForwarderCallback {
public:
    Socks5Proxy();
    ~Socks5Proxy();

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
    void HandleHandshake(SOCKET clientSocket, const char* data, int len);
    bool HandleMethodSelection(SOCKET clientSocket, const char* data, int len, int& consumed);
    bool HandleConnectRequest(SOCKET clientSocket, const char* data, int len, int& consumed);
    SOCKET ConnectToTarget(const std::string& host, int port);
    void SendMethodSelection(SOCKET clientSocket, uint8_t method);
    void SendConnectResponse(SOCKET clientSocket, uint8_t reply, const std::string& host, int port);
    void CloseConnection(SOCKET socket);
    void StartRelay(SOCKET clientSocket, SOCKET serverSocket);

    ForwardRule rule_;
    IOCPManager* iocpManager_;
    SOCKET listenSocket_;
    std::atomic<bool> running_;
    std::atomic<int> connectionCount_;
    std::mutex connectionsMutex_;
    std::unordered_map<SOCKET, std::shared_ptr<Socks5ConnectionContext>> connections_;
};
