#include "replication.hpp"
#include <iostream>
#include <sstream>

static std::string escape(const std::string& str) {
    // The log format is whitespace separated, so an empty string would vanish
    // and shift every later field left when the record is read back. Encode it
    // as an explicit \e marker instead.
    if (str.empty()) return "\\e";

    std::string res;
    for (char c : str) {
        if (c == '\n') res += "\\n";
        else if (c == '\r') res += "\\r";
        else if (c == '\\') res += "\\\\";
        else if (c == ' ') res += "\\s";
        else res += c;
    }
    return res;
}

static std::string unescape(const std::string& str) {
    std::string res;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '\\' && i + 1 < str.length()) {
            char next = str[i+1];
            if (next == 'n') res += '\n';
            else if (next == 'r') res += '\r';
            else if (next == '\\') res += '\\';
            else if (next == 's') res += ' ';
            else if (next == 'e') { /* explicit empty-string marker */ }
            else res += next;
            i++;
        } else {
            res += str[i];
        }
    }
    return res;
}

ReplicationManager::ReplicationManager(Database& db, bool isLeader)
    : m_db(db), m_isLeader(isLeader), m_runFollower(false), m_synced(false), m_leaderSocket(INVALID_SOCKET) {}

ReplicationManager::~ReplicationManager() {
    stopFollowerSync();
    
    // Close all follower sockets if leader
    std::lock_guard<std::mutex> lock(m_followersMutex);
    for (SOCKET sock : m_followers) {
        Network::closeSocket(sock);
    }
    m_followers.clear();
}

void ReplicationManager::registerFollower(SOCKET sock) {
    if (!m_isLeader) return;

    std::lock_guard<std::mutex> lock(m_followersMutex);
    std::cout << "[Leader] Registering new follower node socket." << std::endl;

    // Send synchronization start
    if (!Network::sendString(sock, "REPLICATE_START_SYNC")) {
        std::cerr << "[Leader] Failed to initiate sync handshake with follower." << std::endl;
        Network::closeSocket(sock);
        return;
    }

    // Sync all existing entries
    auto entries = m_db.getAllEntries();
    for (const auto& entry : entries) {
        std::string syncPacket = "REPLICATE PUT " + escape(entry.first) + " " + escape(entry.second);
        if (!Network::sendString(sock, syncPacket)) {
            std::cerr << "[Leader] Failed to send sync payload to follower." << std::endl;
            Network::closeSocket(sock);
            return;
        }
    }

    // Send synchronization complete
    if (!Network::sendString(sock, "REPLICATE_SYNC_COMPLETE")) {
        std::cerr << "[Leader] Failed to finalize sync with follower." << std::endl;
        Network::closeSocket(sock);
        return;
    }

    m_followers.push_back(sock);
    std::cout << "[Leader] Follower sync complete. Replica registered successfully." << std::endl;
}

void ReplicationManager::broadcastWrite(const std::string& op, const std::string& key, const std::string& value) {
    if (!m_isLeader) return;

    std::lock_guard<std::mutex> lock(m_followersMutex);
    if (m_followers.empty()) return;

    std::string packet;
    if (op == "PUT") {
        packet = "REPLICATE PUT " + escape(key) + " " + escape(value);
    } else if (op == "DEL") {
        packet = "REPLICATE DEL " + escape(key);
    } else {
        return;
    }

    auto it = m_followers.begin();
    while (it != m_followers.end()) {
        if (!Network::sendString(*it, packet)) {
            std::cout << "[Leader] Follower disconnected. Removing replica." << std::endl;
            Network::closeSocket(*it);
            it = m_followers.erase(it);
        } else {
            ++it;
        }
    }
}

void ReplicationManager::startFollowerSync(const std::string& leaderIp, int leaderPort, const std::string& nodeId) {
    if (m_isLeader) return;
    
    m_runFollower = true;
    m_followerThread = std::thread(&ReplicationManager::followerLoop, this, leaderIp, leaderPort, nodeId);
}

void ReplicationManager::stopFollowerSync() {
    m_runFollower = false;
    if (m_leaderSocket != INVALID_SOCKET) {
        Network::closeSocket(m_leaderSocket);
        m_leaderSocket = INVALID_SOCKET;
    }
    if (m_followerThread.joinable()) {
        m_followerThread.join();
    }
    m_synced = false;
}

void ReplicationManager::followerLoop(std::string leaderIp, int leaderPort, std::string nodeId) {
    while (m_runFollower) {
        std::cout << "[Follower] Connecting to Leader at " << leaderIp << ":" << leaderPort << "..." << std::endl;
        m_leaderSocket = Network::connectToNode(leaderIp, leaderPort);
        
        if (m_leaderSocket == INVALID_SOCKET) {
            std::cerr << "[Follower] Connection failed. Retrying in 2 seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        std::cout << "[Follower] Connected to Leader. Registering replica..." << std::endl;
        
        // Register handshake
        if (!Network::sendString(m_leaderSocket, "REPLICA_REGISTER " + nodeId)) {
            std::cerr << "[Follower] Failed to register. Retrying connection..." << std::endl;
            Network::closeSocket(m_leaderSocket);
            m_leaderSocket = INVALID_SOCKET;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        std::string msg;
        while (m_runFollower) {
            if (!Network::recvString(m_leaderSocket, msg)) {
                std::cerr << "[Follower] Lost connection to Leader." << std::endl;
                m_synced = false;
                break;
            }

            if (msg == "REPLICATE_START_SYNC") {
                std::cout << "[Follower] Synchronization started. Clearing local state..." << std::endl;
                m_synced = false;
                m_db.clear();
            } else if (msg == "REPLICATE_SYNC_COMPLETE") {
                std::cout << "[Follower] Synchronization completed. Replica is fully synced." << std::endl;
                m_synced = true;
                // Compact replica local WAL into snapshot
                m_db.createSnapshot();
            } else {
                std::stringstream ss(msg);
                std::string token, op, escKey, escVal;
                ss >> token;
                if (token == "REPLICATE") {
                    ss >> op >> escKey;
                    if (op == "PUT") {
                        ss >> escVal;
                        m_db.put(unescape(escKey), unescape(escVal), true); // true = Write to Follower's local WAL
                    } else if (op == "DEL") {
                        m_db.del(unescape(escKey), true); // true = Write to Follower's local WAL
                    }
                }
            }
        }

        Network::closeSocket(m_leaderSocket);
        m_leaderSocket = INVALID_SOCKET;
        m_synced = false;
        
        if (m_runFollower) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}
