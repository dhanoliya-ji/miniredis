#pragma once

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <fstream>
#include <vector>

class Database {
public:
    Database(const std::string& nodeId = "default");
    ~Database();

    // Core KV Operations (Thread-safe)
    bool get(const std::string& key, std::string& value);
    void put(const std::string& key, const std::string& value, bool writeToWal = true);
    bool del(const std::string& key, bool writeToWal = true);

    // Atomic compare-and-set variants. These exist so callers never have to do
    // a get() followed by a put(), which is racy: another thread can slip in
    // between the two calls and the caller then reports the wrong outcome.
    bool putIfAbsent(const std::string& key, const std::string& value);
    bool putIfPresent(const std::string& key, const std::string& value);
    
    // WAL Persistence and Recovery
    void loadFromWalAndSnapshot();
    void createSnapshot();

    // Debug & Inspection
    size_t size();
    std::vector<std::pair<std::string, std::string>> getAllEntries();
    void clear();

private:
    std::string m_nodeId;
    std::string m_walPath;
    std::string m_snapshotPath;
    
    std::unordered_map<std::string, std::string> m_store;
    mutable std::shared_mutex m_mutex;

    std::ofstream m_walStream;

    void openWal();
    void writeWalRecord(const std::string& operation, const std::string& key, const std::string& value = "");
    
    // Helper to sanitize keys/values for log format (escaping newlines)
    std::string escape(const std::string& str);
    std::string unescape(const std::string& str);
};
