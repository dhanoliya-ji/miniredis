#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#include <algorithm>
#include <atomic>
#include "network.hpp"
#include "database.hpp"
#include "sql_parser.hpp"
#include "replication.hpp"

// Number of connection threads currently alive; reported on accept so an
// operator can see the server is not accumulating dead threads.
static std::atomic<int> g_activeConnections{0};

void clientHandler(SOCKET clientSock, Database& db, ReplicationManager& repl, bool isFollower) {
    struct ConnectionGuard {
        ~ConnectionGuard() { g_activeConnections.fetch_sub(1, std::memory_order_relaxed); }
    } guard;

    std::string msg;
    while (true) {
        if (!Network::recvString(clientSock, msg)) {
            break;
        }

        // Check for replica register handshake
        if (msg.rfind("REPLICA_REGISTER", 0) == 0) {
            std::stringstream ss(msg);
            std::string cmd, followerId;
            ss >> cmd >> followerId;
            std::cout << "[Server] Follower connection handshake received from node: " << followerId << std::endl;
            repl.registerFollower(clientSock);
            return; // Exit thread. ReplicationManager now owns and monitors this socket.
        }

        std::string response;
        std::string upperMsg = msg;
        // Trim
        size_t first = upperMsg.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            upperMsg = upperMsg.substr(first);
        }
        std::transform(upperMsg.begin(), upperMsg.end(), upperMsg.begin(), ::toupper);

        if (upperMsg.rfind("SELECT", 0) == 0 || 
            upperMsg.rfind("INSERT", 0) == 0 || 
            upperMsg.rfind("UPDATE", 0) == 0 || 
            upperMsg.rfind("DELETE", 0) == 0) {
            
            // SQL query execution path
            SqlCommand sqlCmd = SqlParser::parse(msg);
            if (sqlCmd.type == SqlType::INVALID) {
                response = "ERROR: " + sqlCmd.errorMessage + "\n";
            } else if (isFollower && (sqlCmd.type == SqlType::INSERT || sqlCmd.type == SqlType::UPDATE || sqlCmd.type == SqlType::DELETE_KEY)) {
                response = "ERROR: Node is a Follower. Write operations are read-only.\n";
            } else {
                response = SqlParser::execute(db, msg, isFollower);
                // If the write query was successful, replicate it
                if (response.rfind("ERROR", 0) != 0) {
                    if (sqlCmd.type == SqlType::INSERT || sqlCmd.type == SqlType::UPDATE) {
                        repl.broadcastWrite("PUT", sqlCmd.key, sqlCmd.value);
                    } else if (sqlCmd.type == SqlType::DELETE_KEY) {
                        repl.broadcastWrite("DEL", sqlCmd.key);
                    }
                }
            }
        } else {
            // Raw KV command execution path
            std::stringstream ss(msg);
            std::string op, key, val;
            ss >> op;
            std::transform(op.begin(), op.end(), op.begin(), ::toupper);

            if (op == "GET") {
                ss >> key;
                if (key.empty()) {
                    response = "ERROR: Key missing for GET.\n";
                } else {
                    std::string value;
                    if (db.get(key, value)) {
                        response = "VALUE " + value + "\n";
                    } else {
                        response = "ERROR: Key not found.\n";
                    }
                }
            } else if (op == "PUT") {
                if (isFollower) {
                    response = "ERROR: Node is a Follower. Write operations are read-only.\n";
                } else {
                    ss >> key;
                    std::getline(ss, val);
                    // Trim leading spaces from value
                    if (!val.empty() && val[0] == ' ') {
                        val = val.substr(1);
                    }
                    if (key.empty() || val.empty()) {
                        response = "ERROR: Key or Value missing for PUT.\n";
                    } else {
                        db.put(key, val);
                        repl.broadcastWrite("PUT", key, val);
                        response = "OK\n";
                    }
                }
            } else if (op == "DEL" || op == "DELETE") {
                if (isFollower) {
                    response = "ERROR: Node is a Follower. Write operations are read-only.\n";
                } else {
                    ss >> key;
                    if (key.empty()) {
                        response = "ERROR: Key missing for DELETE.\n";
                    } else {
                        if (db.del(key)) {
                            repl.broadcastWrite("DEL", key);
                            response = "OK\n";
                        } else {
                            response = "ERROR: Key not found.\n";
                        }
                    }
                }
            } else if (op == "SNAPSHOT") {
                db.createSnapshot();
                response = "OK\n";
            } else if (!op.empty()) {
                response = "ERROR: Unknown command. Supported raw commands: GET, PUT, DELETE, SNAPSHOT.\n";
            }
        }

        if (!response.empty()) {
            // Append End-of-Transmission (EOT) char so client knows when a multi-line response ends
            if (!Network::sendString(clientSock, response + "\n\x04")) {
                break;
            }
        }
    }
    Network::closeSocket(clientSock);
}

int main(int argc, char* argv[]) {
    // Default arguments
    int port = 8000;
    std::string role = "leader"; // "leader" or "follower"
    std::string leaderIp = "127.0.0.1";
    int leaderPort = 8000;
    std::string nodeId = "node0";

    // Simple CLI parser
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--role" && i + 1 < argc) {
            role = argv[++i];
            std::transform(role.begin(), role.end(), role.begin(), ::tolower);
        } else if (arg == "--leader-ip" && i + 1 < argc) {
            leaderIp = argv[++i];
        } else if (arg == "--leader-port" && i + 1 < argc) {
            leaderPort = std::stoi(argv[++i]);
        } else if (arg == "--node-id" && i + 1 < argc) {
            nodeId = argv[++i];
        }
    }

    bool isFollower = (role == "follower");

    std::cout << "==================================================" << std::endl;
    std::cout << "      Distributed Key-Value Database Server       " << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Node ID     : " << nodeId << std::endl;
    std::cout << "Role        : " << (isFollower ? "Follower (Replica)" : "Leader (Primary)") << std::endl;
    std::cout << "Listening Port: " << port << std::endl;
    if (isFollower) {
        std::cout << "Leader IP   : " << leaderIp << std::endl;
        std::cout << "Leader Port : " << leaderPort << std::endl;
    }
    std::cout << "==================================================" << std::endl;

    if (!Network::init()) {
        return 1;
    }

    Database db(nodeId);
    db.loadFromWalAndSnapshot();

    ReplicationManager repl(db, !isFollower);

    if (isFollower) {
        repl.startFollowerSync(leaderIp, leaderPort, nodeId);
    }

    SOCKET listenSock = Network::listenOnPort(port);
    if (listenSock == INVALID_SOCKET) {
        Network::cleanup();
        return 1;
    }

    std::cout << "[Server] Server listening on port " << port << "..." << std::endl;

    bool running = true;

    while (running) {
        sockaddr_in clientAddr{};
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (SOCKADDR*)&clientAddr, &clientAddrLen);
        
        if (clientSock == INVALID_SOCKET) {
            std::cerr << "[Server] Connection accept failed: " << Network::getLastErrorStr() << std::endl;
            continue;
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        std::cout << "[Server] Accepted new connection from " << ipStr << ":" << ntohs(clientAddr.sin_port)
                  << " (active connections: " << g_activeConnections.load(std::memory_order_relaxed) + 1 << ")" << std::endl;

        // Detach rather than collecting the std::thread objects in a vector: the
        // old code never joined them, so every connection ever accepted leaked a
        // thread handle and the vector grew without bound for the process
        // lifetime. A detached thread releases its handle when it returns.
        g_activeConnections.fetch_add(1, std::memory_order_relaxed);
        std::thread(clientHandler, clientSock, std::ref(db), std::ref(repl), isFollower).detach();
    }

    Network::closeSocket(listenSock);
    Network::cleanup();
    return 0;
}
