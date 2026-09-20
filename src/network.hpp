#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>

// Ensure Winsock library is linked (useful for MSVC, but we will also pass -lws2_32 to g++)
#pragma comment(lib, "Ws2_32.lib")

class Network {
public:
    static bool init();
    static void cleanup();
    
    static SOCKET listenOnPort(int port);
    static SOCKET connectToNode(const std::string& ip, int port);
    
    static bool sendString(SOCKET sock, const std::string& str);
    static bool recvString(SOCKET sock, std::string& str);

    static void closeSocket(SOCKET sock);
    static void forgetSocketBuffer(SOCKET sock);
    static std::string getLastErrorStr();
};
