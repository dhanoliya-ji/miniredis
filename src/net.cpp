#include "miniredis/net.hpp"

#include <cstring>
#include <string>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace miniredis {

namespace {

#ifdef _WIN32
int lastError() { return WSAGetLastError(); }
constexpr int kEWouldBlock = WSAEWOULDBLOCK;
constexpr int kEInProgress = WSAEWOULDBLOCK;
constexpr int kEIntr = WSAEINTR;
#else
int lastError() { return errno; }
constexpr int kEWouldBlock = EAGAIN;
constexpr int kEInProgress = EINPROGRESS;
constexpr int kEIntr = EINTR;
#endif

bool errorIsWouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// Waits for a single socket to become readable or writable. Used by the
// blocking helpers so that they honour a timeout instead of hanging forever.
bool waitReady(SocketHandle fd, bool forWrite, int timeoutMs) {
    std::vector<PollRequest> req(1);
    req[0].fd = fd;
    req[0].readable = !forWrite;
    req[0].writable = forWrite;
    const int ready = Net::pollSockets(req, timeoutMs);
    if (ready <= 0) return false;
    return forWrite ? req[0].writable : req[0].readable;
}

} // namespace

bool Net::startup() {
#ifdef _WIN32
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    // Without this, a write to a socket the peer already closed raises SIGPIPE
    // and terminates the process; we want the EPIPE return value instead.
    ::signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

void Net::shutdown() {
#ifdef _WIN32
    WSACleanup();
#endif
}

SocketHandle Net::listenOn(const std::string& bindAddress, int port, int backlog, std::string& error) {
    SocketHandle fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) {
        error = "socket(): " + lastErrorString();
        return kInvalidSocket;
    }

    // SO_REUSEADDR is an int-sized option on every platform. Passing a smaller
    // object makes the kernel read past it, which is how the original code
    // ended up setting the flag from uninitialised stack bytes.
    int enable = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable), sizeof(enable));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (bindAddress.empty() || bindAddress == "*" || bindAddress == "0.0.0.0") {
        address.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
        error = "invalid bind address: " + bindAddress;
        closeSocket(fd);
        return kInvalidSocket;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = "bind(): " + lastErrorString();
        closeSocket(fd);
        return kInvalidSocket;
    }
    if (::listen(fd, backlog) != 0) {
        error = "listen(): " + lastErrorString();
        closeSocket(fd);
        return kInvalidSocket;
    }

    setNonBlocking(fd, true);
    return fd;
}

SocketHandle Net::acceptConnection(SocketHandle listener, std::string& peerAddress) {
    sockaddr_in address{};
#ifdef _WIN32
    int addressLength = sizeof(address);
#else
    socklen_t addressLength = sizeof(address);
#endif

    SocketHandle fd = ::accept(listener, reinterpret_cast<sockaddr*>(&address), &addressLength);
    if (fd == kInvalidSocket) return kInvalidSocket;

    char host[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
    peerAddress = std::string(host) + ":" + std::to_string(ntohs(address.sin_port));

    setNonBlocking(fd, true);
    setNoDelay(fd, true);
    return fd;
}

SocketHandle Net::connectTo(const std::string& host, int port, int timeoutMs, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || results == nullptr) {
        error = "cannot resolve " + host;
        return kInvalidSocket;
    }

    SocketHandle fd = ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
    if (fd == kInvalidSocket) {
        error = "socket(): " + lastErrorString();
        ::freeaddrinfo(results);
        return kInvalidSocket;
    }

    // Connect non-blocking so the timeout is ours to enforce rather than the
    // kernel's (which can be well over a minute).
    setNonBlocking(fd, true);
    const int rc = ::connect(fd, results->ai_addr, static_cast<int>(results->ai_addrlen));
    ::freeaddrinfo(results);

    if (rc != 0) {
        const int err = lastError();
        if (!errorIsWouldBlock(err) && err != kEInProgress) {
            error = "connect(): " + lastErrorString();
            closeSocket(fd);
            return kInvalidSocket;
        }
        if (!waitReady(fd, /*forWrite=*/true, timeoutMs)) {
            error = "connect timed out after " + std::to_string(timeoutMs) + "ms";
            closeSocket(fd);
            return kInvalidSocket;
        }
        // A writable socket does not by itself mean success; SO_ERROR carries
        // the real outcome of an asynchronous connect.
        int soError = 0;
#ifdef _WIN32
        int length = sizeof(soError);
#else
        socklen_t length = sizeof(soError);
#endif
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &length);
        if (soError != 0) {
            error = "connect failed (code " + std::to_string(soError) + ")";
            closeSocket(fd);
            return kInvalidSocket;
        }
    }

    setNoDelay(fd, true);
    return fd;
}

bool Net::setNonBlocking(SocketHandle fd, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags) == 0;
#endif
}

bool Net::setNoDelay(SocketHandle fd, bool enabled) {
    int value = enabled ? 1 : 0;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

bool Net::setKeepAlive(SocketHandle fd, bool enabled) {
    int value = enabled ? 1 : 0;
    return ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

IoResult Net::readSome(SocketHandle fd, char* buffer, size_t capacity, size_t& transferred) {
    transferred = 0;
    const int n = ::recv(fd, buffer, static_cast<int>(capacity), 0);
    if (n > 0) {
        transferred = static_cast<size_t>(n);
        return IoResult::Ok;
    }
    if (n == 0) return IoResult::Closed;

    const int err = lastError();
    if (errorIsWouldBlock(err) || err == kEIntr) return IoResult::WouldBlock;
    return IoResult::Error;
}

IoResult Net::writeSome(SocketHandle fd, const char* buffer, size_t length, size_t& transferred) {
    transferred = 0;
    if (length == 0) return IoResult::Ok;

    const int n = ::send(fd, buffer, static_cast<int>(length), 0);
    if (n > 0) {
        transferred = static_cast<size_t>(n);
        return IoResult::Ok;
    }

    const int err = lastError();
    if (errorIsWouldBlock(err) || err == kEIntr) return IoResult::WouldBlock;
    return IoResult::Error;
}

bool Net::writeAll(SocketHandle fd, const char* buffer, size_t length, int timeoutMs) {
    size_t sent = 0;
    while (sent < length) {
        size_t chunk = 0;
        const IoResult result = writeSome(fd, buffer + sent, length - sent, chunk);
        if (result == IoResult::Ok) {
            sent += chunk;
        } else if (result == IoResult::WouldBlock) {
            if (!waitReady(fd, /*forWrite=*/true, timeoutMs)) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool Net::readExactly(SocketHandle fd, char* buffer, size_t length, int timeoutMs) {
    size_t received = 0;
    while (received < length) {
        size_t chunk = 0;
        const IoResult result = readSome(fd, buffer + received, length - received, chunk);
        if (result == IoResult::Ok) {
            received += chunk;
        } else if (result == IoResult::WouldBlock) {
            if (!waitReady(fd, /*forWrite=*/false, timeoutMs)) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool Net::readLine(SocketHandle fd, std::string& line, int timeoutMs) {
    line.clear();
    // The handshake lines are short, so reading a byte at a time here is fine
    // and avoids having to hand a leftover buffer back to the caller. The hot
    // client path uses the buffered reader in the server instead.
    while (line.size() < 64 * 1024) {
        char c = 0;
        size_t chunk = 0;
        const IoResult result = readSome(fd, &c, 1, chunk);
        if (result == IoResult::Ok) {
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            line.push_back(c);
        } else if (result == IoResult::WouldBlock) {
            if (!waitReady(fd, /*forWrite=*/false, timeoutMs)) return false;
        } else {
            return false;
        }
    }
    return false;
}

void Net::closeSocket(SocketHandle& fd) {
    if (fd == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
    fd = kInvalidSocket;
}

int Net::pollSockets(std::vector<PollRequest>& requests, int timeoutMs) {
    if (requests.empty()) return 0;

#ifdef _WIN32
    std::vector<WSAPOLLFD> fds(requests.size());
#else
    std::vector<struct pollfd> fds(requests.size());
#endif

    for (size_t i = 0; i < requests.size(); ++i) {
        fds[i].fd = requests[i].fd;
        fds[i].events = 0;
        if (requests[i].readable) fds[i].events |= POLLIN;
        if (requests[i].writable) fds[i].events |= POLLOUT;
        fds[i].revents = 0;
    }

#ifdef _WIN32
    const int ready = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs);
#else
    const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
#endif

    if (ready < 0) {
        if (lastError() == kEIntr) return 0; // a signal interrupted the wait
        return -1;
    }

    for (size_t i = 0; i < requests.size(); ++i) {
        const short revents = fds[i].revents;
        requests[i].readable = (revents & POLLIN) != 0;
        requests[i].writable = (revents & POLLOUT) != 0;
        requests[i].errored = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
    }
    return ready;
}

bool Net::lastErrorWasWouldBlock() {
    return errorIsWouldBlock(lastError());
}

std::string Net::lastErrorString() {
    const int err = lastError();
#ifdef _WIN32
    char* buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(err), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

    std::string message = (size > 0 && buffer != nullptr) ? std::string(buffer, size) : std::string("unknown error");
    if (buffer != nullptr) LocalFree(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
        message.pop_back();
    }
    return message + " (code " + std::to_string(err) + ")";
#else
    return std::string(std::strerror(err)) + " (code " + std::to_string(err) + ")";
#endif
}

// Silence "defined but not used" on the platform where a constant is unused.
static_assert(kEWouldBlock == kEWouldBlock, "");

} // namespace miniredis
