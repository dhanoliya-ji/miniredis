#include "network.hpp"
#include <iostream>
#include <vector>
#include <mutex>
#include <unordered_map>

bool Network::init() {
    WSADATA wsaData;
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res != 0) {
        std::cerr << "WSAStartup failed: " << res << std::endl;
        return false;
    }
    return true;
}

void Network::cleanup() {
    WSACleanup();
}

SOCKET Network::listenOnPort(int port) {
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed: " << getLastErrorStr() << std::endl;
        return INVALID_SOCKET;
    }

    // SO_REUSEADDR takes a BOOL/int-sized option on Winsock. Passing a single
    // `char` made setsockopt read 4 bytes from a 1-byte object, so the option
    // was set from three bytes of stack garbage.
    int optval = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&optval), sizeof(optval));

    sockaddr_in service{};
    service.sin_family = AF_INET;
    service.sin_addr.s_addr = INADDR_ANY;
    service.sin_port = htons(port);

    if (bind(listenSock, (SOCKADDR*)&service, sizeof(service)) == SOCKET_ERROR) {
        std::cerr << "Bind failed: " << getLastErrorStr() << std::endl;
        closesocket(listenSock);
        return INVALID_SOCKET;
    }

    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed: " << getLastErrorStr() << std::endl;
        closesocket(listenSock);
        return INVALID_SOCKET;
    }

    return listenSock;
}

SOCKET Network::connectToNode(const std::string& ip, int port) {
    SOCKET connectSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connectSock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed: " << getLastErrorStr() << std::endl;
        return INVALID_SOCKET;
    }

    sockaddr_in clientService{};
    clientService.sin_family = AF_INET;
    clientService.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &clientService.sin_addr);

    if (connect(connectSock, (SOCKADDR*)&clientService, sizeof(clientService)) == SOCKET_ERROR) {
        // Don't print error here, as we might try reconnection in loop
        closesocket(connectSock);
        return INVALID_SOCKET;
    }

    return connectSock;
}

bool Network::sendString(SOCKET sock, const std::string& str) {
    std::string packet = str;
    if (packet.empty() || packet.back() != '\n') {
        packet += '\n';
    }

    size_t totalSent = 0;
    size_t len = packet.length();
    const char* buf = packet.c_str();

    while (totalSent < len) {
        int sent = send(sock, buf + totalSent, static_cast<int>(len - totalSent), 0);
        if (sent == SOCKET_ERROR) {
            return false;
        }
        totalSent += sent;
    }
    return true;
}

namespace {

// Reading a line one byte at a time issues a syscall per character. We instead
// keep a small carry-over buffer per socket and refill it in 4 KiB chunks,
// which cuts recv() calls on a typical command by two orders of magnitude.
constexpr size_t kRecvChunk = 4096;

std::mutex g_bufferMutex;
std::unordered_map<SOCKET, std::string> g_recvBuffers;

} // namespace

bool Network::recvString(SOCKET sock, std::string& str) {
    str.clear();

    std::lock_guard<std::mutex> lock(g_bufferMutex);
    std::string& buffer = g_recvBuffers[sock];

    char chunk[kRecvChunk];
    while (true) {
        size_t newlinePos = buffer.find('
');
        if (newlinePos != std::string::npos) {
            str.assign(buffer, 0, newlinePos);
            buffer.erase(0, newlinePos + 1);
            return true;
        }

        int received = recv(sock, chunk, static_cast<int>(kRecvChunk), 0);
        if (received <= 0) {
            return false; // Connection closed or error
        }
        buffer.append(chunk, static_cast<size_t>(received));
    }
}

void Network::forgetSocketBuffer(SOCKET sock) {
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    g_recvBuffers.erase(sock);
}

void Network::closeSocket(SOCKET sock) {
    if (sock != INVALID_SOCKET) {
        forgetSocketBuffer(sock);
        closesocket(sock);
    }
}

std::string Network::getLastErrorStr() {
    int err = WSAGetLastError();
    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, nullptr
    );
    std::string message(messageBuffer, size);
    LocalFree(messageBuffer);
    
    // Trim trailing whitespace
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
        message.pop_back();
    }
    return message + " (Code: " + std::to_string(err) + ")";
}
