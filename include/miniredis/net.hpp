// Thin cross-platform TCP layer.
//
// The rest of MiniRedis never includes <winsock2.h> or <sys/socket.h>; it talks
// to sockets exclusively through this header. That keeps the Windows and POSIX
// differences (SOCKET vs int, closesocket vs close, WSAPoll vs poll, WSAStartup)
// in one place instead of smeared across the server.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <poll.h>
#include <sys/socket.h>
#endif

namespace miniredis {

#ifdef _WIN32
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// What a non-blocking read or write reported.
enum class IoResult {
    Ok,        // progress was made
    WouldBlock, // the socket is not ready; try again when poll() says so
    Closed,    // peer closed the connection
    Error,     // unrecoverable error
};

// Events requested from / reported by pollSockets().
struct PollRequest {
    SocketHandle fd = kInvalidSocket;
    bool readable = false;   // in: interested in readability, out: is readable
    bool writable = false;   // in: interested in writability, out: is writable
    bool errored = false;    // out only: hangup or error
};

class Net {
public:
    // Must be called once per process before any other member. On Windows this
    // runs WSAStartup; elsewhere it installs a SIGPIPE ignore so that writing to
    // a closed socket returns EPIPE instead of killing the process.
    static bool startup();
    static void shutdown();

    static SocketHandle listenOn(const std::string& bindAddress, int port, int backlog, std::string& error);
    static SocketHandle acceptConnection(SocketHandle listener, std::string& peerAddress);

    // Blocking connect with a timeout, used by a replica dialling its master.
    static SocketHandle connectTo(const std::string& host, int port, int timeoutMs, std::string& error);

    static bool setNonBlocking(SocketHandle fd, bool enabled);
    static bool setNoDelay(SocketHandle fd, bool enabled);
    static bool setKeepAlive(SocketHandle fd, bool enabled);

    // Non-blocking primitives. `transferred` receives the byte count on Ok.
    static IoResult readSome(SocketHandle fd, char* buffer, size_t capacity, size_t& transferred);
    static IoResult writeSome(SocketHandle fd, const char* buffer, size_t length, size_t& transferred);

    // Blocking helpers, used only on the replication handshake path where a
    // simple request/response exchange is far clearer than driving the loop.
    static bool writeAll(SocketHandle fd, const char* buffer, size_t length, int timeoutMs);
    static bool readLine(SocketHandle fd, std::string& line, int timeoutMs);
    static bool readExactly(SocketHandle fd, char* buffer, size_t length, int timeoutMs);

    static void closeSocket(SocketHandle& fd);

    // Waits up to timeoutMs for any of the requested events. Returns the number
    // of ready sockets, or -1 on error. A negative timeout blocks indefinitely.
    static int pollSockets(std::vector<PollRequest>& requests, int timeoutMs);

    static std::string lastErrorString();
    static bool lastErrorWasWouldBlock();
};

} // namespace miniredis
