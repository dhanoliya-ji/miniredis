#include "database.hpp"
#include <iostream>
#include <sstream>

Database::Database(const std::string& nodeId) 
    : m_nodeId(nodeId), 
      m_walPath(nodeId + "_wal.log"), 
      m_snapshotPath(nodeId + "_snapshot.db") {
    // Open WAL stream in append mode
    openWal();
}

Database::~Database() {
    if (m_walStream.is_open()) {
        m_walStream.close();
    }
}

void Database::openWal() {
    if (m_walStream.is_open()) {
        m_walStream.close();
    }
    m_walStream.open(m_walPath, std::ios::out | std::ios::app);
    if (!m_walStream.is_open()) {
        std::cerr << "[" << m_nodeId << "] Failed to open WAL file: " << m_walPath << std::endl;
    }
}

std::string Database::escape(const std::string& str) {
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

std::string Database::unescape(const std::string& str) {
    std::string res;
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '\\' && i + 1 < str.length()) {
            char next = str[i+1];
            if (next == 'n') res += '\n';
            else if (next == 'r') res += '\r';
            else if (next == '\\') res += '\\';
            else if (next == 's') res += ' ';
            else res += next;
            i++;
        } else {
            res += str[i];
        }
    }
    return res;
}

void Database::writeWalRecord(const std::string& operation, const std::string& key, const std::string& value) {
    if (!m_walStream.is_open()) {
        openWal();
    }
    if (m_walStream.is_open()) {
        if (operation == "PUT") {
            m_walStream << "PUT " << escape(key) << " " << escape(value) << "\n";
        } else if (operation == "DEL") {
            m_walStream << "DEL " << escape(key) << "\n";
        }
        m_walStream.flush();
    }
}

bool Database::get(const std::string& key, std::string& value) {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_store.find(key);
    if (it != m_store.end()) {
        value = it->second;
        return true;
    }
    return false;
}

void Database::put(const std::string& key, const std::string& value, bool writeToWal) {
    // The WAL append must happen under the same lock that mutates the map.
    // Appending after the lock was released let two concurrent writers to the
    // same key land in the log in the opposite order to the one they applied
    // in memory, so recovery could resurrect the older value.
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_store[key] = value;
    if (writeToWal) {
        writeWalRecord("PUT", key, value);
    }
}

bool Database::del(const std::string& key, bool writeToWal) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    const bool erased = (m_store.erase(key) > 0);
    if (erased && writeToWal) {
        writeWalRecord("DEL", key);
    }
    return erased;
}

bool Database::putIfAbsent(const std::string& key, const std::string& value) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (m_store.find(key) != m_store.end()) {
        return false;
    }
    m_store[key] = value;
    writeWalRecord("PUT", key, value);
    return true;
}

bool Database::putIfPresent(const std::string& key, const std::string& value) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_store.find(key);
    if (it == m_store.end()) {
        return false;
    }
    it->second = value;
    writeWalRecord("PUT", key, value);
    return true;
}

void Database::loadFromWalAndSnapshot() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_store.clear();

    // 1. Load from snapshot if it exists
    std::ifstream snapshotFile(m_snapshotPath);
    if (snapshotFile.is_open()) {
        std::string line;
        while (std::getline(snapshotFile, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string escapedKey, escapedVal;
            if (ss >> escapedKey >> escapedVal) {
                m_store[unescape(escapedKey)] = unescape(escapedVal);
            }
        }
        snapshotFile.close();
        std::cout << "[" << m_nodeId << "] Loaded state from snapshot: " << m_snapshotPath << " (size: " << m_store.size() << ")" << std::endl;
    }

    // 2. Replay WAL file
    std::ifstream walFile(m_walPath);
    if (walFile.is_open()) {
        std::string line;
        size_t replayedCount = 0;
        while (std::getline(walFile, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string op, escapedKey, escapedVal;
            if (ss >> op >> escapedKey) {
                if (op == "PUT") {
                    ss >> escapedVal;
                    m_store[unescape(escapedKey)] = unescape(escapedVal);
                    replayedCount++;
                } else if (op == "DEL") {
                    m_store.erase(unescape(escapedKey));
                    replayedCount++;
                }
            }
        }
        walFile.close();
        std::cout << "[" << m_nodeId << "] Replayed " << replayedCount << " operations from WAL: " << m_walPath << " (final size: " << m_store.size() << ")" << std::endl;
    }
}

void Database::createSnapshot() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    
    // Write current state to temporary snapshot file
    std::string tempSnapshotPath = m_snapshotPath + ".tmp";
    std::ofstream snapStream(tempSnapshotPath, std::ios::out | std::ios::trunc);
    if (!snapStream.is_open()) {
        std::cerr << "[" << m_nodeId << "] Failed to create temp snapshot file: " << tempSnapshotPath << std::endl;
        return;
    }

    for (const auto& pair : m_store) {
        snapStream << escape(pair.first) << " " << escape(pair.second) << "\n";
    }
    snapStream.close();

    // Close and truncate the active WAL stream, then delete the WAL file or empty it
    if (m_walStream.is_open()) {
        m_walStream.close();
    }
    
    // Atomically overwrite the snapshot file and truncate the WAL file
    std::remove(m_snapshotPath.c_str());
    if (std::rename(tempSnapshotPath.c_str(), m_snapshotPath.c_str()) != 0) {
        std::cerr << "[" << m_nodeId << "] Error replacing snapshot file." << std::endl;
        return;
    }

    // Truncate the WAL file
    std::ofstream walTruncate(m_walPath, std::ios::out | std::ios::trunc);
    walTruncate.close();
    
    // Reopen WAL in append mode
    openWal();
    
    std::cout << "[" << m_nodeId << "] Created database snapshot. Compacted WAL." << std::endl;
}

size_t Database::size() {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_store.size();
}

std::vector<std::pair<std::string, std::string>> Database::getAllEntries() {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& pair : m_store) {
        entries.push_back(pair);
    }
    return entries;
}

void Database::clear() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_store.clear();
    if (m_walStream.is_open()) {
        m_walStream.close();
    }
    std::remove(m_walPath.c_str());
    std::remove(m_snapshotPath.c_str());
    openWal();
}
