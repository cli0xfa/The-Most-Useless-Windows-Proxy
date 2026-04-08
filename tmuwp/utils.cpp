#include "utils.h"
#include <ws2tcpip.h>
#include <iostream>


IOContext::IOContext() {
    ZeroMemory(&overlapped, sizeof(overlapped));
    ZeroMemory(buffer, sizeof(buffer));
    wsaBuf.buf = buffer;
    wsaBuf.len = sizeof(buffer);
    operation = IOOperation::READ;
    sock = INVALID_SOCKET;
    remoteSock = INVALID_SOCKET;
    bytesTransferred = 0;
    inUse = false;
}

void IOContext::Reset() {
    ZeroMemory(&overlapped, sizeof(overlapped));
    ZeroMemory(buffer, sizeof(buffer));
    wsaBuf.buf = buffer;
    wsaBuf.len = sizeof(buffer);
    operation = IOOperation::READ;
    bytesTransferred = 0;
    inUse = false;
}


ConnectionContext::ConnectionContext()
    : clientSock(INVALID_SOCKET)
    , serverSock(INVALID_SOCKET)
    , closing(false) {
    clientToServer = std::make_unique<IOContext>();
    serverToClient = std::make_unique<IOContext>();
}

bool InitWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed: " << result << std::endl;
        return false;
    }
    return true;
}

void CleanupWinsock() {
    WSACleanup();
}

bool ParseAddress(const std::string& addr, std::string& ip, int& port) {
    size_t pos = addr.rfind(':');
    if (pos == std::string::npos) {
        return false;
    }

    // Handle IPv6 addresses wrapped in brackets [IPv6]:port
    if (addr[0] == '[') {
        size_t bracket_end = addr.find(']');
        if (bracket_end == std::string::npos || bracket_end >= pos) {
            return false;
        }
        ip = addr.substr(1, bracket_end - 1);
    } else {
        ip = addr.substr(0, pos);
    }

    try {
        port = std::stoi(addr.substr(pos + 1));
    } catch (...) {
        return false;
    }

    return port > 0 && port <= 65535;
}

std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, nullptr, nullptr);
    return str;
}

std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();

    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
    return wstr;
}
