// Leader/follower replication.
//
// The original project replicated by re-sending each write as a text line and
// having the follower apply it. That works right up until the link drops: on
// reconnect the follower had no idea what it had missed, so it threw its whole
// dataset away and resynchronised from scratch. On a large dataset a two
// second network blip therefore cost a full transfer.
//
// The fix is the one Redis uses: give the write stream a byte offset.
//
//   * The master maintains a monotonically increasing replication offset and a
//     fixed-size circular backlog of the most recently propagated bytes.
//   * A replica remembers the offset it has applied.
//   * On reconnect it asks PSYNC <replid> <offset+1>. If that offset is still
//     inside the backlog the master replies +CONTINUE and ships only the bytes
//     the replica missed -- a partial resync. Otherwise it replies +FULLRESYNC
//     and sends a fresh snapshot.
//
// So the backlog size directly buys reconnect resilience: a 1 MB backlog on a
// stream doing 100 KB/s covers a ten second outage without a full transfer.
//
// Replication here is asynchronous, exactly as in Redis. A master acknowledges
// a write to its client before any replica has confirmed it, so a master that
// dies immediately after replying can lose that write. WAIT lets a client ask
// for stronger guarantees on demand.
#pragma once

#include "miniredis/common.hpp"
#include "miniredis/net.hpp"

#include <string>
#include <vector>

namespace miniredis {

class Server;
struct Client;

enum class ReplicaLinkState {
    None,        // this server is a master
    Connect,     // needs to dial the master
    Connecting,  // handshake in progress
    Sync,        // receiving the full-resync payload
    Connected,   // streaming commands
};

const char* replicaLinkStateName(ReplicaLinkState state);

// A ring buffer of recently propagated bytes, indexed by stream offset.
//
// Sized once at startup. It is the difference between a reconnect costing a
// few kilobytes and costing the whole dataset.
class ReplicationBacklog {
public:
    void resize(size_t capacity);

    void append(const char* data, size_t length);

    // The oldest offset still recoverable. Anything below this is gone and the
    // replica must do a full resync.
    std::int64_t firstAvailableOffset() const { return m_startOffset; }
    std::int64_t endOffset() const { return m_endOffset; }

    bool canServe(std::int64_t fromOffset) const;

    // Copies everything from `fromOffset` to the end of the stream.
    bool readFrom(std::int64_t fromOffset, std::string& out) const;

    size_t capacity() const { return m_buffer.size(); }
    std::int64_t histlen() const { return m_endOffset - m_startOffset; }

private:
    std::string m_buffer;
    size_t m_writePos = 0;
    bool m_wrapped = false;
    std::int64_t m_startOffset = 0;
    std::int64_t m_endOffset = 0;
};

class ReplicationManager {
public:
    explicit ReplicationManager(Server& server);
    ~ReplicationManager();

    // ---------------------------------------------------------------------
    // Role
    // ---------------------------------------------------------------------

    bool isReplica() const { return !m_masterHost.empty(); }
    bool isMaster() const { return m_masterHost.empty(); }
    const std::string& masterHost() const { return m_masterHost; }
    int masterPort() const { return m_masterPort; }
    ReplicaLinkState linkState() const { return m_linkState; }

    // REPLICAOF host port, or REPLICAOF NO ONE to be promoted to a master.
    void replicaOf(const std::string& host, int port);
    void promoteToMaster();

    const std::string& replicationId() const { return m_replicationId; }
    std::int64_t masterOffset() const { return m_masterOffset; }
    std::int64_t appliedOffset() const { return m_appliedOffset; }

    // ---------------------------------------------------------------------
    // Master side
    // ---------------------------------------------------------------------

    // Writes `payload` to the backlog and to every attached replica.
    void propagateToReplicas(const std::string& payload);

    // Handles PSYNC from a client, turning it into a replica.
    void handlePsync(Client& client, const std::string& requestedReplId, std::int64_t requestedOffset);

    void removeReplica(Client& client);
    std::vector<Client*> attachedReplicas() const;
    size_t replicaCount() const;

    // Replicas with an acknowledged offset at or beyond `offset`.
    size_t replicasAcknowledging(std::int64_t offset) const;

    // Asks every replica to report its offset. Called by WAIT and by the cron.
    void requestAcksFromReplicas();

    // ---------------------------------------------------------------------
    // Replica side
    // ---------------------------------------------------------------------

    // Drives the link to the master: dialling, handshaking, loading the
    // snapshot, and consuming the command stream. Called from the event loop.
    void driveReplicaLink(Millis now);

    // Feeds bytes that arrived on the master link into the command applier.
    void onMasterDataAvailable();

    SocketHandle masterLinkSocket() const { return m_masterLink; }
    Millis lastMasterInteraction() const { return m_lastMasterInteraction; }
    bool linkIsUp() const { return m_linkState == ReplicaLinkState::Connected; }

    std::string infoSection() const;

private:
    bool performHandshake(std::string& error);
    bool receiveFullResync(std::string& error);
    void dropMasterLink(const std::string& reason);
    void applyMasterStream();
    static std::string generateReplicationId();

    Server& m_server;

    // Master state.
    std::string m_replicationId;
    std::int64_t m_masterOffset = 0;
    ReplicationBacklog m_backlog;
    std::vector<std::uint64_t> m_replicaClientIds;

    // Replica state.
    std::string m_masterHost;
    int m_masterPort = 0;
    SocketHandle m_masterLink = kInvalidSocket;
    ReplicaLinkState m_linkState = ReplicaLinkState::None;
    std::int64_t m_appliedOffset = 0;
    std::string m_masterReplicationId;
    std::string m_masterBuffer;
    size_t m_masterBufferOffset = 0;
    int m_masterStreamDb = 0; // database the master's stream is currently in
    Millis m_lastMasterInteraction = 0;
    Millis m_nextReconnectAttempt = 0;
    int m_reconnectBackoffMs = 500;
    Millis m_lastAckSent = 0;
};

} // namespace miniredis
