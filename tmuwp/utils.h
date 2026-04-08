#pragma once

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <memory>

// Winsock initialization
bool InitWinsock();
void CleanupWinsock();

// Address parsing utility
bool ParseAddress(const std::string& addr, std::string& ip, int& port);

// String conversion utilities
std::string WStringToString(const std::wstring& wstr);
std::wstring StringToWString(const std::string& str);

// IOCP operation types
enum class IOOperation {
    ACCEPT,
    CONNECT,
    READ,
    WRITE,
    DISCONNECT
};

// Per-I/O operation context
struct IOContext {
    WSAOVERLAPPED overlapped;
    WSABUF wsaBuf;
    char buffer[8192];
    IOOperation operation;
    SOCKET sock;
    SOCKET remoteSock;
    DWORD bytesTransferred;
    bool inUse;

    IOContext();
    void Reset();
};

// Per-connection context
struct ConnectionContext {
    SOCKET clientSock;
    SOCKET serverSock;
    std::unique_ptr<IOContext> clientToServer;
    std::unique_ptr<IOContext> serverToClient;
    bool closing;

    ConnectionContext();
};
