// The server: configuration, connected clients, and the single-threaded event
// loop that drives everything.
//
// MiniRedis follows Redis's threading model rather than the thread-per-client
// model the original project used. One thread owns the entire keyspace, so
// every command is atomic by construction: there is no lock to forget, no
// ordering between a mutation and its log record to get wrong, and no torn
// read of a data structure mid-resize. Concurrency comes from multiplexing
// thousands of sockets with poll(), not from threads.
//
// The cost is that one slow command blocks every client, which is exactly why
// KEYS is discouraged in favour of SCAN and why the slow log exists.
#pragma once

#include "miniredis/command.hpp"
#include "miniredis/common.hpp"
#include "miniredis/keyspace.hpp"
#include "miniredis/net.hpp"
#include "miniredis/object.hpp"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace miniredis {

class Aof;
class RdbFile;
class PubSub;
class ReplicationManager;
class ClusterState;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

enum class AofFsyncPolicy {
    Always,   // fsync after every write: safest, slowest
    EverySec, // fsync once a second: the Redis default, loses at most 1s
    No,       // let the OS decide: fastest, loses whatever is in page cache
};

bool parseAofFsyncPolicy(std::string_view name, AofFsyncPolicy& out);
const char* aofFsyncPolicyName(AofFsyncPolicy policy);

// "save 900 1" means: snapshot if at least 1 key changed in the last 900s.
struct SaveRule {
    int seconds = 0;
    int changes = 0;
};

enum class LogLevel { Debug, Verbose, Notice, Warning };

struct ServerConfig {
    // Networking. The default port is 6380 rather than 6379 so MiniRedis can
    // run alongside a real Redis without a collision.
    std::string bindAddress = "127.0.0.1";
    int port = 6380;
    int tcpBacklog = 511;
    int clientTimeoutSeconds = 0; // 0 disables the idle-client reaper
    size_t maxClients = 10000;

    std::string nodeId = "miniredis";
    std::string workingDir = ".";
    int databaseCount = 16;
    std::string requirePass;

    // Memory limits and what to do when they are hit.
    std::int64_t maxMemoryBytes = 0; // 0 means unlimited
    EvictionPolicy evictionPolicy = EvictionPolicy::NoEviction;
    size_t evictionSamples = 5;

    // Persistence.
    bool appendOnly = false;
    std::string appendFilename = "miniredis.aof";
    AofFsyncPolicy appendFsync = AofFsyncPolicy::EverySec;
    std::int64_t autoAofRewritePercentage = 100;
    std::int64_t autoAofRewriteMinSize = 64 * 1024 * 1024;

    std::string rdbFilename = "miniredis.rdb";
    std::vector<SaveRule> saveRules{{900, 1}, {300, 100}, {60, 10000}};
    bool loadOnStartup = true;

    // Replication.
    std::string replicaOfHost;
    int replicaOfPort = 0;
    bool replicaReadOnly = true;
    std::string masterAuth;
    int replTimeoutSeconds = 60;
    std::int64_t replBacklogSize = 1024 * 1024;

    // Cluster.
    bool clusterEnabled = false;
    std::string clusterConfigFile = "nodes.conf";
    std::string clusterAnnounceIp;
    int clusterAnnouncePort = 0;

    // Observability.
    LogLevel logLevel = LogLevel::Notice;
    std::string logFile; // empty means stdout
    std::int64_t slowLogThresholdMicros = 10000;
    size_t slowLogMaxLength = 128;

    // Loads a redis.conf-style file: one directive per line, '#' comments.
    bool loadFromFile(const std::string& path, std::string& error);

    // Applies a single "directive arg [arg...]" line. Shared by the config
    // file loader and the CONFIG SET command so the two can never drift.
    bool applyDirective(const std::string& name, const std::vector<std::string>& values, std::string& error);

    // Renders a directive's current value for CONFIG GET.
    bool readDirective(const std::string& name, std::string& valueOut) const;

    // Every directive name, for CONFIG GET with a glob pattern.
    static std::vector<std::string> directiveNames();

    std::string resolvePath(const std::string& filename) const;
};

// ---------------------------------------------------------------------------
// Clients
// ---------------------------------------------------------------------------

enum class ClientRole {
    Normal,
    Replica,  // a replica that has completed PSYNC and receives the write stream
    Monitor,  // a client running MONITOR, which receives every command
};

// A key a client is watching for a transaction.
struct WatchedKey {
    int dbIndex = 0;
    Bytes key;
};

struct Client {
    std::uint64_t id = 0;
    SocketHandle fd = kInvalidSocket;
    std::string peerAddress;
    std::string name;

    // Unparsed bytes from the socket, with the offset of the next command.
    // Consumed bytes are compacted out only when the offset grows large, so a
    // pipelined burst is parsed without shifting the buffer for each command.
    std::string inputBuffer;
    size_t inputOffset = 0;

    // Pending reply bytes and how much has already been handed to the kernel.
    std::string outputBuffer;
    size_t outputOffset = 0;

    int dbIndex = 0;
    bool authenticated = false;
    bool closeAfterReply = false;
    bool shouldClose = false;

    ClientRole role = ClientRole::Normal;

    // MULTI/EXEC state.
    bool inMulti = false;
    bool multiQueueError = false; // a queued command failed to parse
    std::vector<Args> multiQueue;
    std::vector<WatchedKey> watchedKeys;
    bool watchInvalidated = false;

    // Pub/Sub state.
    std::unordered_set<std::string> subscribedChannels;
    std::unordered_set<std::string> subscribedPatterns;

    // Replication link state, meaningful only when role == Replica.
    std::int64_t replicaAckOffset = 0;
    Millis replicaAckTime = 0;
    std::string replicaListeningPort;

    Millis createdAt = 0;
    Millis lastInteraction = 0;
    std::uint64_t commandsProcessed = 0;

    bool isSubscribeMode() const {
        return !subscribedChannels.empty() || !subscribedPatterns.empty();
    }

    size_t pendingOutputBytes() const { return outputBuffer.size() - outputOffset; }
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct SlowLogEntry {
    std::uint64_t id = 0;
    Millis timestamp = 0;
    std::int64_t durationMicros = 0;
    Args args;
    std::string clientAddress;
    std::string clientName;
};

struct ServerStats {
    std::uint64_t totalConnections = 0;
    std::uint64_t totalCommands = 0;
    std::uint64_t rejectedConnections = 0;
    std::uint64_t expiredKeys = 0;
    std::uint64_t evictedKeys = 0;
    std::uint64_t keyspaceHits = 0;
    std::uint64_t keyspaceMisses = 0;
    std::uint64_t syncFull = 0;
    std::uint64_t syncPartialOk = 0;
    std::uint64_t syncPartialErr = 0;
    std::uint64_t pubsubMessages = 0;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

class Server {
public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds the listening socket and loads any persisted dataset.
    bool start(std::string& error);

    // Runs the event loop until stop() is called. Returns the process exit code.
    int run();

    void stop() { m_running = false; }
    bool isRunning() const { return m_running; }

    // ---------------------------------------------------------------------
    // Accessors
    // ---------------------------------------------------------------------

    ServerConfig& config() { return m_config; }
    const ServerConfig& config() const { return m_config; }

    Keyspace& db(int index) { return *m_databases[static_cast<size_t>(index)]; }
    const Keyspace& db(int index) const { return *m_databases[static_cast<size_t>(index)]; }
    int databaseCount() const { return static_cast<int>(m_databases.size()); }
    Keyspace& currentDb(const Client& client) { return db(client.dbIndex); }

    const CommandTable& commands() const { return m_commands; }
    ServerStats& stats() { return m_stats; }
    const ServerStats& stats() const { return m_stats; }

    Aof& aof() { return *m_aof; }
    PubSub& pubsub() { return *m_pubsub; }
    ReplicationManager& replication() { return *m_replication; }
    ClusterState& cluster() { return *m_cluster; }

    Millis startTime() const { return m_startTime; }
    std::uint32_t lruClock() const { return m_lruClock; }

    // The dirty counter increments once per actual keyspace mutation. It is
    // how the dispatcher decides whether a write command really changed
    // anything and therefore needs propagating, and how the snapshot rules
    // decide whether enough has changed to be worth saving.
    std::int64_t dirty() const { return m_dirty; }
    void markDirty(std::int64_t amount = 1) { m_dirty += amount; }

    // ---------------------------------------------------------------------
    // Client handling
    // ---------------------------------------------------------------------

    Client* findClient(std::uint64_t id);
    const std::unordered_map<std::uint64_t, std::unique_ptr<Client>>& clients() const { return m_clients; }
    size_t clientCount() const { return m_clients.size(); }

    void closeClient(Client& client) { client.shouldClose = true; }

    // Appends to a client's output buffer and makes sure the loop will poll
    // for writability.
    void addReply(Client& client, std::string_view bytes);

    // ---------------------------------------------------------------------
    // Command execution
    // ---------------------------------------------------------------------

    // Runs one command on behalf of a client, including the MULTI queueing,
    // authentication, read-only-replica, OOM and cluster-redirect checks.
    void executeCommand(Client& client, const Args& args);

    // Runs a command that did not come from a client socket: the AOF loader
    // and the replication stream both use this.
    void executeFromStream(int dbIndex, const Args& args);

    // Sends a command to the AOF and to every attached replica. Called by the
    // dispatcher after a write command that actually changed something.
    void propagate(int dbIndex, const Args& args);

    // ---------------------------------------------------------------------
    // Keyspace maintenance
    // ---------------------------------------------------------------------

    // Current estimated memory footprint of all databases.
    std::int64_t usedMemory() const;

    // Evicts keys until usage is back under maxmemory. Returns false when the
    // limit cannot be met and the write must be rejected with OOM.
    bool freeMemoryIfNeeded();

    // Marks every client watching this key as invalidated, so their next EXEC
    // aborts. Called on every mutation.
    void signalKeyModified(int dbIndex, const Bytes& key);

    void notifyKeyRemoved(int dbIndex, const Bytes& key);

    // ---------------------------------------------------------------------
    // Persistence
    // ---------------------------------------------------------------------

    bool saveSnapshot(std::string& error);
    bool loadDataset(std::string& error);
    void flushAllDatabases();

    // ---------------------------------------------------------------------
    // Logging and the slow log
    // ---------------------------------------------------------------------

    void log(LogLevel level, const std::string& message);
    const std::deque<SlowLogEntry>& slowLog() const { return m_slowLog; }
    void resetSlowLog() { m_slowLog.clear(); }

    std::string infoReport(const std::string& section) const;

    // Registers a socket the loop should watch on behalf of a subsystem (the
    // replication link to a master). Returns false if it is already watched.
    void setReplicationLinkSocket(SocketHandle fd) { m_replicationLinkFd = fd; }

private:
    void acceptNewClients();
    void readFromClient(Client& client);
    void writeToClient(Client& client);
    void processInputBuffer(Client& client);
    void removeClosedClients();
    void disconnectClient(Client& client);

    // Runs every 100ms: active expiry, LRU clock, snapshot rules, AOF fsync,
    // replication timeouts, idle-client reaping.
    void serverCron();

    void feedMonitors(Client& client, const Args& args);
    void recordSlowLog(const Client& client, const Args& args, std::int64_t durationMicros);

    bool m_running = false;

    ServerConfig m_config;
    CommandTable m_commands;
    std::vector<std::unique_ptr<Keyspace>> m_databases;

    SocketHandle m_listener = kInvalidSocket;
    SocketHandle m_replicationLinkFd = kInvalidSocket;

    std::unordered_map<std::uint64_t, std::unique_ptr<Client>> m_clients;
    std::uint64_t m_nextClientId = 1;

    std::unique_ptr<Aof> m_aof;
    std::unique_ptr<PubSub> m_pubsub;
    std::unique_ptr<ReplicationManager> m_replication;
    std::unique_ptr<ClusterState> m_cluster;

    ServerStats m_stats;
    std::int64_t m_dirty = 0;
    std::int64_t m_dirtyAtLastSave = 0;
    Millis m_lastSaveTime = 0;
    Millis m_startTime = 0;
    Millis m_lastCronRun = 0;
    std::uint32_t m_lruClock = 0;

    std::deque<SlowLogEntry> m_slowLog;
    std::uint64_t m_nextSlowLogId = 0;

    // Reentrancy guard: propagate() must not recurse when a command executed
    // from the replication stream itself triggers an expiry.
    bool m_inExecution = false;
};

} // namespace miniredis
