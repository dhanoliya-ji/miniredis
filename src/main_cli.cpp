// miniredis-cli: an interactive RESP client.
//
// Because the server speaks genuine RESP2, the stock `redis-cli` also works
// against it. This one exists so the project is usable with nothing installed,
// and so the reply decoding is visible rather than hidden in a dependency.
#include "miniredis/common.hpp"
#include "miniredis/net.hpp"
#include "miniredis/resp.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace miniredis;

constexpr int kTimeoutMs = 30000;

struct Options {
    std::string host = "127.0.0.1";
    int port = 6380;
    std::string password;
    std::vector<std::string> command; // non-interactive: run one command and exit
    int repeat = 1;
    bool raw = false;
};

void printUsage() {
    std::cout <<
        "miniredis-cli - interactive client for MiniRedis\n"
        "\n"
        "Usage: miniredis-cli [-h host] [-p port] [-a password] [-r n] [--raw] [command ...]\n"
        "\n"
        "  -h <host>      server host (default 127.0.0.1)\n"
        "  -p <port>      server port (default 6380)\n"
        "  -a <password>  authenticate on connect\n"
        "  -r <n>         repeat the command n times\n"
        "  --raw          print replies without type decoration\n"
        "\n"
        "With no command, an interactive prompt is started. Type 'help' at the\n"
        "prompt for a short tour, or 'exit' to leave.\n";
}

// Splits an input line into arguments, honouring quotes so that a value with
// spaces can be typed naturally.
bool tokenise(const std::string& line, std::vector<std::string>& out, std::string& error) {
    out.clear();
    std::string current;
    bool haveToken = false;
    char quote = 0;

    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < line.size()) {
                const char next = line[++i];
                switch (next) {
                    case 'n': current.push_back('\n'); break;
                    case 't': current.push_back('\t'); break;
                    case 'r': current.push_back('\r'); break;
                    default:  current.push_back(next); break;
                }
            } else if (c == quote) {
                quote = 0;
            } else {
                current.push_back(c);
            }
            haveToken = true;
        } else if (c == '"' || c == '\'') {
            quote = c;
            haveToken = true;
        } else if (c == ' ' || c == '\t') {
            if (haveToken) {
                out.push_back(current);
                current.clear();
                haveToken = false;
            }
        } else {
            current.push_back(c);
            haveToken = true;
        }
    }

    if (quote != 0) {
        error = "unbalanced quotes";
        return false;
    }
    if (haveToken) out.push_back(current);
    return true;
}

// Reads and renders exactly one RESP reply, recursing into arrays so that
// nested replies are indented rather than flattened.
bool readAndPrintReply(SocketHandle fd, std::string& buffer, bool raw, int depth, int index);

bool ensureBytes(SocketHandle fd, std::string& buffer, size_t needed) {
    char chunk[16 * 1024];
    while (buffer.size() < needed) {
        size_t received = 0;
        const IoResult result = Net::readSome(fd, chunk, sizeof(chunk), received);
        if (result == IoResult::Ok) {
            buffer.append(chunk, received);
            continue;
        }
        if (result == IoResult::WouldBlock) {
            std::vector<PollRequest> request(1);
            request[0].fd = fd;
            request[0].readable = true;
            if (Net::pollSockets(request, kTimeoutMs) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

// Reads one CRLF-terminated line out of the buffer, refilling from the socket.
bool readLineFrom(SocketHandle fd, std::string& buffer, std::string& line) {
    while (true) {
        const size_t end = buffer.find("\r\n");
        if (end != std::string::npos) {
            line.assign(buffer, 0, end);
            buffer.erase(0, end + 2);
            return true;
        }
        const size_t before = buffer.size();
        if (!ensureBytes(fd, buffer, before + 1)) return false;
    }
}

void printIndent(int depth) {
    for (int i = 0; i < depth; ++i) std::cout << "  ";
}

bool readAndPrintReply(SocketHandle fd, std::string& buffer, bool raw, int depth, int index) {
    std::string line;
    if (!readLineFrom(fd, buffer, line)) {
        std::cerr << "(connection lost)\n";
        return false;
    }
    if (line.empty()) return true;

    const char tag = line[0];
    const std::string payload = line.substr(1);

    printIndent(depth);
    if (index >= 0 && !raw) std::cout << index << ") ";

    switch (tag) {
        case '+':
            std::cout << (raw ? payload : payload) << "\n";
            return true;

        case '-':
            // Errors go to stderr so that piping the output of a script does
            // not silently swallow a failure.
            std::cerr << (raw ? payload : "(error) " + payload) << "\n";
            return true;

        case ':':
            std::cout << (raw ? payload : "(integer) " + payload) << "\n";
            return true;

        case '$': {
            std::int64_t length = 0;
            if (!parseInt64(payload, length)) {
                std::cerr << "(protocol error) bad bulk length: " << payload << "\n";
                return false;
            }
            if (length < 0) {
                std::cout << (raw ? "" : "(nil)") << "\n";
                return true;
            }
            if (!ensureBytes(fd, buffer, static_cast<size_t>(length) + 2)) {
                std::cerr << "(connection lost)\n";
                return false;
            }
            const std::string value(buffer, 0, static_cast<size_t>(length));
            buffer.erase(0, static_cast<size_t>(length) + 2);

            if (raw) {
                std::cout << value << "\n";
            } else if (value.find('\n') != std::string::npos) {
                // Multi-line payloads (INFO, the SQL tables) read far better
                // printed verbatim than wrapped in quotes.
                std::cout << "\n" << value;
                if (!value.empty() && value.back() != '\n') std::cout << "\n";
            } else {
                std::cout << "\"" << value << "\"\n";
            }
            return true;
        }

        case '*': {
            std::int64_t count = 0;
            if (!parseInt64(payload, count)) {
                std::cerr << "(protocol error) bad array length: " << payload << "\n";
                return false;
            }
            if (count < 0) {
                std::cout << (raw ? "" : "(nil)") << "\n";
                return true;
            }
            if (count == 0) {
                std::cout << (raw ? "" : "(empty array)") << "\n";
                return true;
            }

            std::cout << "\n";
            for (std::int64_t i = 0; i < count; ++i) {
                if (!readAndPrintReply(fd, buffer, raw, depth + 1, static_cast<int>(i + 1))) return false;
            }
            return true;
        }

        default:
            std::cerr << "(protocol error) unknown reply type '" << tag << "'\n";
            return false;
    }
}

bool sendCommand(SocketHandle fd, const std::vector<std::string>& args) {
    const std::string encoded = encodeCommand(args);
    return Net::writeAll(fd, encoded.data(), encoded.size(), kTimeoutMs);
}

void printInteractiveHelp() {
    std::cout <<
        "\nMiniRedis speaks RESP2, so the stock redis-cli works against it too.\n"
        "\n"
        "A quick tour:\n"
        "  SET user:1 alice            store a string\n"
        "  GET user:1                  read it back\n"
        "  EXPIRE user:1 60            give it a 60 second TTL\n"
        "  TTL user:1                  see what is left\n"
        "  RPUSH queue a b c           lists\n"
        "  HSET user:2 name bob        hashes\n"
        "  ZADD board 100 alice        sorted sets, for leaderboards\n"
        "  ZRANGE board 0 -1 WITHSCORES\n"
        "\n"
        "  MULTI / EXEC                queue commands and run them together\n"
        "  WATCH k                     abort the transaction if k changes\n"
        "  SUBSCRIBE news              enter pub/sub mode\n"
        "\n"
        "  SQL SELECT * FROM kv        the SQL view of the string keyspace\n"
        "  INFO                        server self-report\n"
        "  COMMAND COUNT               how many commands are implemented\n"
        "  MEMORY DOCTOR               memory advice\n"
        "\n"
        "Type 'exit' or press Ctrl-C to leave.\n\n";
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string token = argv[i];

        if (token == "--help") {
            printUsage();
            return 0;
        }
        if (token == "--raw") {
            options.raw = true;
        } else if (token == "-h" && i + 1 < argc) {
            options.host = argv[++i];
        } else if (token == "-p" && i + 1 < argc) {
            std::int64_t port = 0;
            if (!parseInt64(argv[++i], port) || port < 1 || port > 65535) {
                std::cerr << "miniredis-cli: invalid port\n";
                return 1;
            }
            options.port = static_cast<int>(port);
        } else if (token == "-a" && i + 1 < argc) {
            options.password = argv[++i];
        } else if (token == "-r" && i + 1 < argc) {
            std::int64_t repeat = 0;
            if (!parseInt64(argv[++i], repeat) || repeat < 1) {
                std::cerr << "miniredis-cli: invalid repeat count\n";
                return 1;
            }
            options.repeat = static_cast<int>(repeat);
        } else {
            // Everything from here is the command to run non-interactively.
            for (; i < argc; ++i) options.command.push_back(argv[i]);
            break;
        }
    }

    if (!Net::startup()) {
        std::cerr << "miniredis-cli: cannot initialise networking\n";
        return 1;
    }

    std::string error;
    SocketHandle fd = Net::connectTo(options.host, options.port, 5000, error);
    if (fd == kInvalidSocket) {
        std::cerr << "miniredis-cli: could not connect to " << options.host << ":" << options.port
                  << " - " << error << "\n";
        Net::shutdown();
        return 1;
    }

    std::string buffer;

    if (!options.password.empty()) {
        if (!sendCommand(fd, {"AUTH", options.password})) {
            std::cerr << "miniredis-cli: failed to send AUTH\n";
            Net::closeSocket(fd);
            Net::shutdown();
            return 1;
        }
        readAndPrintReply(fd, buffer, options.raw, 0, -1);
    }

    // Non-interactive: run the command (optionally repeated) and exit.
    if (!options.command.empty()) {
        int exitCode = 0;
        for (int i = 0; i < options.repeat; ++i) {
            if (!sendCommand(fd, options.command)) {
                std::cerr << "miniredis-cli: failed to send the command\n";
                exitCode = 1;
                break;
            }
            if (!readAndPrintReply(fd, buffer, options.raw, 0, -1)) {
                exitCode = 1;
                break;
            }
        }
        Net::closeSocket(fd);
        Net::shutdown();
        return exitCode;
    }

    std::cout << "MiniRedis 1.0.0 - connected to " << options.host << ":" << options.port << "\n"
              << "Type 'help' for a tour, 'exit' to quit.\n\n";

    std::string line;
    while (true) {
        std::cout << options.host << ":" << options.port << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        if (equalsIgnoreCase(trimmed, "exit") || equalsIgnoreCase(trimmed, "quit")) break;
        if (equalsIgnoreCase(trimmed, "help")) {
            printInteractiveHelp();
            continue;
        }

        std::vector<std::string> args;
        std::string tokeniseError;
        if (!tokenise(trimmed, args, tokeniseError)) {
            std::cerr << "(error) " << tokeniseError << "\n";
            continue;
        }
        if (args.empty()) continue;

        if (!sendCommand(fd, args)) {
            std::cerr << "(error) connection lost while sending\n";
            break;
        }
        if (!readAndPrintReply(fd, buffer, options.raw, 0, -1)) break;

        // SUBSCRIBE puts the connection into message mode, where the server
        // pushes without being asked. Keep printing until the user interrupts.
        if (equalsIgnoreCase(args[0], "SUBSCRIBE") || equalsIgnoreCase(args[0], "PSUBSCRIBE")) {
            std::cout << "(listening for messages; press Ctrl-C to stop)\n";
            while (readAndPrintReply(fd, buffer, options.raw, 0, -1)) {
                // Each delivered message prints as it arrives.
            }
            break;
        }
    }

    Net::closeSocket(fd);
    Net::shutdown();
    std::cout << "\n";
    return 0;
}
