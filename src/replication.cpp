#include "miniredis/replication.hpp"

#include "miniredis/rdb.hpp"
#include "miniredis/resp.hpp"
#include "miniredis/server.hpp"

#include <algorithm>
#include <random>

namespace miniredis {

namespace {

constexpr int kHandshakeTimeoutMs = 5000;
constexpr int kMaxReconnectBackoffMs = 8000;
constexpr Millis kAckIntervalMs = 1000;

} // namespace

const char* replicaLinkStateName(ReplicaLinkState state) {
    switch (state) {
        case ReplicaLinkState::None:       return "none";
        case ReplicaLinkState::Connect:    return "connect";
        case ReplicaLinkState::Connecting: return "connecting";
        case ReplicaLinkState::Sync:       return "sync";
        case ReplicaLinkState::Connected:  return "connected";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// ReplicationBacklog
// ---------------------------------------------------------------------------

void ReplicationBacklog::resize(size_t capacity) {
    m_buffer.assign(capacity, '\0');
    m_writePos = 0;
    m_wrapped = false;
    m_startOffset = m_endOffset;
}

void ReplicationBacklog::append(const char* data, size_t length) {
    if (m_buffer.empty() || length == 0) {
        m_endOffset += static_cast<std::int64_t>(length);
        m_startOffset = m_endOffset;
        return;
    }

    // A payload larger than the whole ring can only keep its tail.
    if (length >= m_buffer.size()) {
        const size_t keep = m_buffer.size();
        std::copy(data + length - keep, data + length, m_buffer.begin());
        m_writePos = 0;
        m_wrapped = true;
        m_endOffset += static_cast<std::int64_t>(length);
        m_startOffset = m_endOffset - static_cast<std::int64_t>(keep);
        return;
    }

    size_t written = 0;
    while (written < length) {
        const size_t chunk = std::min(length - written, m_buffer.size() - m_writePos);
        std::copy(data + written, data + written + chunk, m_buffer.begin() + static_cast<long>(m_writePos));
        m_writePos += chunk;
        written += chunk;
        if (m_writePos == m_buffer.size()) {
            m_writePos = 0;
            m_wrapped = true;
        }
    }

    m_endOffset += static_cast<std::int64_t>(length);
    const std::int64_t held = m_wrapped ? static_cast<std::int64_t>(m_buffer.size())
                                        : static_cast<std::int64_t>(m_writePos);
    m_startOffset = m_endOffset - held;
}

bool ReplicationBacklog::canServe(std::int64_t fromOffset) const {
    if (m_buffer.empty()) return false;
    return fromOffset >= m_startOffset && fromOffset <= m_endOffset;
}

bool ReplicationBacklog::readFrom(std::int64_t fromOffset, std::string& out) const {
    if (!canServe(fromOffset)) return false;

    const std::int64_t wanted = m_endOffset - fromOffset;
    if (wanted <= 0) {
        out.clear();
        return true;
    }

    out.resize(static_cast<size_t>(wanted));

    // Walk backwards from the write cursor: the byte just before m_writePos is
    // the newest, so the requested range ends there.
    size_t readPos = (m_writePos + m_buffer.size() - static_cast<size_t>(wanted)) % m_buffer.size();
    for (std::int64_t i = 0; i < wanted; ++i) {
        out[static_cast<size_t>(i)] = m_buffer[readPos];
        readPos = (readPos + 1) % m_buffer.size();
    }
    return true;
}

// ---------------------------------------------------------------------------
// ReplicationManager
// ---------------------------------------------------------------------------

ReplicationManager::ReplicationManager(Server& server)
    : m_server(server), m_replicationId(generateReplicationId()) {
    m_backlog.resize(static_cast<size_t>(std::max<std::int64_t>(server.config().replBacklogSize, 0)));
}

ReplicationManager::~ReplicationManager() {
    if (m_masterLink != kInvalidSocket) Net::closeSocket(m_masterLink);
}

std::string ReplicationManager::generateReplicationId() {
    // 40 hex characters, like Redis's run id. It identifies a particular
    // stream of history: if it changes, a replica's offset is meaningless and
    // a partial resync is impossible.
    static const char* kHex = "0123456789abcdef";
    std::random_device device;
    std::mt19937_64 engine(device());

    std::string id;
    id.reserve(40);
    for (int i = 0; i < 40; ++i) {
        id.push_back(kHex[engine() % 16]);
    }
    return id;
}

void ReplicationManager::replicaOf(const std::string& host, int port) {
    m_masterHost = host;
    m_masterPort = port;
    m_linkState = ReplicaLinkState::Connect;
    m_nextReconnectAttempt = 0;
    m_reconnectBackoffMs = 500;

    if (m_masterLink != kInvalidSocket) {
        Net::closeSocket(m_masterLink);
        m_server.setReplicationLinkSocket(kInvalidSocket);
    }

    m_server.log(LogLevel::Notice, "REPLICAOF " + host + ":" + formatInt64(port) +
                                       " enabled; will synchronise with the new master");
}

void ReplicationManager::promoteToMaster() {
    const bool wasReplica = isReplica();

    m_masterHost.clear();
    m_masterPort = 0;
    m_linkState = ReplicaLinkState::None;
    if (m_masterLink != kInvalidSocket) {
        Net::closeSocket(m_masterLink);
        m_server.setReplicationLinkSocket(kInvalidSocket);
    }
    m_masterBuffer.clear();
    m_masterBufferOffset = 0;

    if (wasReplica) {
        // A promoted replica starts a new history. Its old offsets belonged to
        // the previous master's stream, so keeping the id would let a replica
        // partially resync against a timeline that never existed here.
        m_replicationId = generateReplicationId();
        m_masterOffset = m_appliedOffset;
        m_backlog.resize(m_backlog.capacity());
        m_server.log(LogLevel::Notice, "promoted to master with replication id " + m_replicationId);
    }
}

// ---------------------------------------------------------------------------
// Master side
// ---------------------------------------------------------------------------

void ReplicationManager::propagateToReplicas(const std::string& payload) {
    if (payload.empty()) return;

    // The backlog is fed even with no replicas attached, so that one which
    // connects shortly after a burst can still partially resync.
    m_backlog.append(payload.data(), payload.size());
    m_masterOffset = m_backlog.endOffset();

    for (const std::uint64_t id : m_replicaClientIds) {
        if (Client* replica = m_server.findClient(id)) {
            m_server.addReply(*replica, payload);
        }
    }
}

void ReplicationManager::handlePsync(Client& client, const std::string& requestedReplId,
                                     std::int64_t requestedOffset) {
    const bool idMatches = (requestedReplId == m_replicationId);
    const bool offsetAvailable = m_backlog.canServe(requestedOffset);

    if (idMatches && offsetAvailable && requestedOffset >= 0) {
        // Partial resync: ship only the missed bytes.
        std::string missed;
        if (m_backlog.readFrom(requestedOffset, missed)) {
            m_server.addReply(client, "+CONTINUE " + m_replicationId + "\r\n");
            if (!missed.empty()) m_server.addReply(client, missed);

            client.role = ClientRole::Replica;
            client.replicaAckOffset = requestedOffset;
            client.replicaAckTime = nowMillis();
            if (std::find(m_replicaClientIds.begin(), m_replicaClientIds.end(), client.id) ==
                m_replicaClientIds.end()) {
                m_replicaClientIds.push_back(client.id);
            }

            ++m_server.stats().syncPartialOk;
            m_server.log(LogLevel::Notice,
                         "partial resync accepted for replica " + client.peerAddress + " from offset " +
                             formatInt64(requestedOffset) + " (" +
                             formatMemorySize(static_cast<std::int64_t>(missed.size())) + " sent)");
            return;
        }
    }

    if (requestedOffset >= 0) {
        ++m_server.stats().syncPartialErr;
        m_server.log(LogLevel::Notice,
                     std::string("partial resync refused for ") + client.peerAddress + ": " +
                         (idMatches ? "requested offset is no longer in the backlog"
                                    : "replication id mismatch"));
    }

    // Full resync: announce the stream identity and offset, then send a
    // snapshot of the whole dataset as a length-prefixed bulk payload.
    ++m_server.stats().syncFull;

    const std::string snapshot = rdbSerializeDatasets(m_server);

    std::string header = "+FULLRESYNC " + m_replicationId + " " + formatInt64(m_masterOffset) + "\r\n";
    header += "$" + formatInt64(static_cast<std::int64_t>(snapshot.size())) + "\r\n";

    m_server.addReply(client, header);
    m_server.addReply(client, snapshot);

    client.role = ClientRole::Replica;
    client.replicaAckOffset = m_masterOffset;
    client.replicaAckTime = nowMillis();
    if (std::find(m_replicaClientIds.begin(), m_replicaClientIds.end(), client.id) ==
        m_replicaClientIds.end()) {
        m_replicaClientIds.push_back(client.id);
    }

    m_server.log(LogLevel::Notice,
                 "full resync started for replica " + client.peerAddress + " (" +
                     formatMemorySize(static_cast<std::int64_t>(snapshot.size())) + " snapshot, offset " +
                     formatInt64(m_masterOffset) + ")");
}

void ReplicationManager::removeReplica(Client& client) {
    const auto it = std::find(m_replicaClientIds.begin(), m_replicaClientIds.end(), client.id);
    if (it == m_replicaClientIds.end()) return;

    m_replicaClientIds.erase(it);
    m_server.log(LogLevel::Notice, "replica " + client.peerAddress + " disconnected");
}

std::vector<Client*> ReplicationManager::attachedReplicas() const {
    std::vector<Client*> replicas;
    for (const std::uint64_t id : m_replicaClientIds) {
        if (Client* replica = const_cast<Server&>(m_server).findClient(id)) {
            replicas.push_back(replica);
        }
    }
    return replicas;
}

size_t ReplicationManager::replicaCount() const {
    return attachedReplicas().size();
}

size_t ReplicationManager::replicasAcknowledging(std::int64_t offset) const {
    size_t count = 0;
    for (const Client* replica : attachedReplicas()) {
        if (replica->replicaAckOffset >= offset) ++count;
    }
    return count;
}

void ReplicationManager::requestAcksFromReplicas() {
    if (m_replicaClientIds.empty()) return;

    const std::string request = encodeCommand(Args{"REPLCONF", "GETACK", "*"});
    for (Client* replica : attachedReplicas()) {
        m_server.addReply(*replica, request);
    }

    // The request itself is part of the stream, so it advances the offset and
    // must go into the backlog too; otherwise a replica's acknowledged offset
    // would never match the master's.
    m_backlog.append(request.data(), request.size());
    m_masterOffset = m_backlog.endOffset();
}

// ---------------------------------------------------------------------------
// Replica side
// ---------------------------------------------------------------------------

void ReplicationManager::dropMasterLink(const std::string& reason) {
    if (m_masterLink != kInvalidSocket) {
        Net::closeSocket(m_masterLink);
        m_server.setReplicationLinkSocket(kInvalidSocket);
    }
    m_masterBuffer.clear();
    m_masterBufferOffset = 0;
    m_linkState = ReplicaLinkState::Connect;

    // Exponential backoff, so a master that is down does not get hammered with
    // a connection attempt every event loop iteration.
    m_nextReconnectAttempt = nowMillis() + m_reconnectBackoffMs;
    m_reconnectBackoffMs = std::min(m_reconnectBackoffMs * 2, kMaxReconnectBackoffMs);

    m_server.log(LogLevel::Warning, "master link lost: " + reason +
                                        "; reconnecting in " + formatInt64(m_reconnectBackoffMs) + "ms");
}

bool ReplicationManager::performHandshake(std::string& error) {
    // PING first, to prove the link is a working MiniRedis before sending
    // anything that would change state on the other end.
    const std::string ping = encodeCommand(Args{"PING"});
    if (!Net::writeAll(m_masterLink, ping.data(), ping.size(), kHandshakeTimeoutMs)) {
        error = "failed to send PING";
        return false;
    }

    std::string line;
    if (!Net::readLine(m_masterLink, line, kHandshakeTimeoutMs)) {
        error = "no reply to PING";
        return false;
    }
    if (line.empty() || (line[0] != '+' && line[0] != '-')) {
        error = "unexpected reply to PING: " + line;
        return false;
    }
    if (line.rfind("-NOAUTH", 0) == 0 || line.rfind("-ERR operation not permitted", 0) == 0) {
        // Fall through to AUTH below; the master wants credentials.
    }

    if (!m_server.config().masterAuth.empty()) {
        const std::string auth = encodeCommand(Args{"AUTH", m_server.config().masterAuth});
        if (!Net::writeAll(m_masterLink, auth.data(), auth.size(), kHandshakeTimeoutMs)) {
            error = "failed to send AUTH";
            return false;
        }
        if (!Net::readLine(m_masterLink, line, kHandshakeTimeoutMs) || line.empty() || line[0] != '+') {
            error = "master rejected AUTH: " + line;
            return false;
        }
    }

    // Tell the master which port we listen on, so its INFO can report a
    // reachable address for this replica rather than the ephemeral source port.
    const std::string listeningPort =
        encodeCommand(Args{"REPLCONF", "listening-port", formatInt64(m_server.config().port)});
    if (!Net::writeAll(m_masterLink, listeningPort.data(), listeningPort.size(), kHandshakeTimeoutMs)) {
        error = "failed to send REPLCONF listening-port";
        return false;
    }
    if (!Net::readLine(m_masterLink, line, kHandshakeTimeoutMs)) {
        error = "no reply to REPLCONF listening-port";
        return false;
    }

    return true;
}

bool ReplicationManager::receiveFullResync(std::string& error) {
    // Ask for a partial resync when we have history to resume from. On the
    // very first connection there is nothing to resume, so "? -1" requests a
    // full transfer explicitly.
    const bool canAttemptPartial = !m_masterReplicationId.empty() && m_appliedOffset > 0;
    const Args psync{
        "PSYNC",
        canAttemptPartial ? m_masterReplicationId : std::string("?"),
        canAttemptPartial ? formatInt64(m_appliedOffset) : std::string("-1"),
    };

    const std::string request = encodeCommand(psync);
    if (!Net::writeAll(m_masterLink, request.data(), request.size(), kHandshakeTimeoutMs)) {
        error = "failed to send PSYNC";
        return false;
    }

    std::string line;
    if (!Net::readLine(m_masterLink, line, kHandshakeTimeoutMs)) {
        error = "no reply to PSYNC";
        return false;
    }

    if (line.rfind("+CONTINUE", 0) == 0) {
        // The master kept our place; the dataset stays exactly as it is and the
        // missed bytes arrive as an ordinary command stream.
        m_linkState = ReplicaLinkState::Connected;
        m_lastMasterInteraction = nowMillis();
        m_reconnectBackoffMs = 500;
        m_server.log(LogLevel::Notice,
                     "partial resync accepted by master; resuming from offset " + formatInt64(m_appliedOffset));
        return true;
    }

    if (line.rfind("+FULLRESYNC", 0) != 0) {
        error = "unexpected PSYNC reply: " + line;
        return false;
    }

    // +FULLRESYNC <replid> <offset>
    {
        std::string replId;
        std::int64_t offset = 0;
        const size_t firstSpace = line.find(' ');
        const size_t secondSpace = line.find(' ', firstSpace + 1);
        if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
            error = "malformed FULLRESYNC reply: " + line;
            return false;
        }
        replId = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        if (!parseInt64(line.substr(secondSpace + 1), offset)) {
            error = "malformed FULLRESYNC offset: " + line;
            return false;
        }
        m_masterReplicationId = replId;
        m_appliedOffset = offset;
    }

    // The snapshot arrives as "$<length>\r\n<payload>", without a trailing
    // CRLF, so it can be streamed without buffering it twice.
    if (!Net::readLine(m_masterLink, line, kHandshakeTimeoutMs) || line.empty() || line[0] != '$') {
        error = "expected a bulk length for the snapshot, got: " + line;
        return false;
    }

    std::int64_t payloadLength = 0;
    if (!parseInt64(line.substr(1), payloadLength) || payloadLength < 0) {
        error = "invalid snapshot length: " + line;
        return false;
    }

    std::string payload;
    payload.resize(static_cast<size_t>(payloadLength));
    if (payloadLength > 0 &&
        !Net::readExactly(m_masterLink, payload.data(), payload.size(), m_server.config().replTimeoutSeconds * 1000)) {
        error = "the snapshot transfer was cut short";
        return false;
    }

    m_server.log(LogLevel::Notice,
                 "received a " + formatMemorySize(payloadLength) + " snapshot from the master");

    // Only now is the local dataset discarded. Flushing before the transfer
    // completed would leave the replica empty if the link died mid-snapshot.
    m_server.flushAllDatabases();

    std::string loadError;
    if (!rdbLoadDatasets(m_server, payload, loadError)) {
        error = "cannot load the master snapshot: " + loadError;
        return false;
    }

    m_linkState = ReplicaLinkState::Connected;
    m_lastMasterInteraction = nowMillis();
    m_reconnectBackoffMs = 500;

    std::int64_t loadedKeys = 0;
    for (int i = 0; i < m_server.databaseCount(); ++i) loadedKeys += static_cast<std::int64_t>(m_server.db(i).size());
    m_server.log(LogLevel::Notice,
                 "full resync complete: " + formatInt64(loadedKeys) + " keys at offset " +
                     formatInt64(m_appliedOffset));
    return true;
}

void ReplicationManager::driveReplicaLink(Millis now) {
    if (!isReplica()) return;

    switch (m_linkState) {
        case ReplicaLinkState::None:
            m_linkState = ReplicaLinkState::Connect;
            return;

        case ReplicaLinkState::Connect: {
            if (now < m_nextReconnectAttempt) return;

            std::string error;
            m_masterLink = Net::connectTo(m_masterHost, m_masterPort, kHandshakeTimeoutMs, error);
            if (m_masterLink == kInvalidSocket) {
                m_nextReconnectAttempt = now + m_reconnectBackoffMs;
                m_reconnectBackoffMs = std::min(m_reconnectBackoffMs * 2, kMaxReconnectBackoffMs);
                m_server.log(LogLevel::Warning,
                             "cannot reach master " + m_masterHost + ":" + formatInt64(m_masterPort) +
                                 " (" + error + "); retrying in " + formatInt64(m_reconnectBackoffMs) + "ms");
                return;
            }

            m_linkState = ReplicaLinkState::Connecting;
            m_server.log(LogLevel::Notice, "connected to master " + m_masterHost + ":" + formatInt64(m_masterPort));
            [[fallthrough]];
        }

        case ReplicaLinkState::Connecting: {
            std::string error;
            if (!performHandshake(error)) {
                dropMasterLink(error);
                return;
            }
            m_linkState = ReplicaLinkState::Sync;
            [[fallthrough]];
        }

        case ReplicaLinkState::Sync: {
            std::string error;
            if (!receiveFullResync(error)) {
                dropMasterLink(error);
                return;
            }
            // Hand the socket to the event loop, which will poll it for
            // readability alongside every client socket.
            Net::setNonBlocking(m_masterLink, true);
            m_server.setReplicationLinkSocket(m_masterLink);
            return;
        }

        case ReplicaLinkState::Connected: {
            const Millis timeoutMs = static_cast<Millis>(m_server.config().replTimeoutSeconds) * 1000;
            if (timeoutMs > 0 && now - m_lastMasterInteraction > timeoutMs) {
                dropMasterLink("no data from the master for " + formatInt64(timeoutMs) + "ms");
                return;
            }

            // Periodic acknowledgement, so the master can report replica lag
            // and so WAIT has something to count.
            if (now - m_lastAckSent >= kAckIntervalMs) {
                const std::string ack = encodeCommand(Args{"REPLCONF", "ACK", formatInt64(m_appliedOffset)});
                size_t sent = 0;
                Net::writeSome(m_masterLink, ack.data(), ack.size(), sent);
                m_lastAckSent = now;
            }
            return;
        }
    }
}

void ReplicationManager::onMasterDataAvailable() {
    if (m_masterLink == kInvalidSocket) return;

    char chunk[64 * 1024];
    while (true) {
        size_t received = 0;
        const IoResult result = Net::readSome(m_masterLink, chunk, sizeof(chunk), received);
        if (result == IoResult::Ok) {
            m_masterBuffer.append(chunk, received);
            m_lastMasterInteraction = nowMillis();
            continue;
        }
        if (result == IoResult::WouldBlock) break;
        dropMasterLink(result == IoResult::Closed ? "the master closed the connection"
                                                  : "a socket error occurred");
        return;
    }

    applyMasterStream();
}

void ReplicationManager::applyMasterStream() {
    while (m_masterBufferOffset < m_masterBuffer.size()) {
        Args args;
        std::string error;
        const size_t commandStart = m_masterBufferOffset;
        const ParseStatus status =
            RespParser::parseCommand(m_masterBuffer, m_masterBufferOffset, args, error);

        if (status == ParseStatus::Incomplete) {
            m_masterBufferOffset = commandStart;
            break;
        }
        if (status == ParseStatus::Invalid) {
            dropMasterLink("protocol error in the master stream: " + error);
            return;
        }

        // The offset counts the bytes of the stream, not the commands, because
        // that is the unit the backlog is indexed by.
        const std::int64_t consumed = static_cast<std::int64_t>(m_masterBufferOffset - commandStart);

        if (!args.empty()) {
            if (equalsIgnoreCase(args[0], "PING")) {
                // A keepalive; it advances the offset but changes nothing.
            } else if (equalsIgnoreCase(args[0], "REPLCONF") && args.size() >= 2 &&
                       equalsIgnoreCase(args[1], "GETACK")) {
                // The ACK must report the offset *including* this command, so
                // it is sent after the offset has been advanced below.
                m_appliedOffset += consumed;
                const std::string ack = encodeCommand(Args{"REPLCONF", "ACK", formatInt64(m_appliedOffset)});
                size_t sent = 0;
                Net::writeSome(m_masterLink, ack.data(), ack.size(), sent);
                m_lastAckSent = nowMillis();
                continue;
            } else if (equalsIgnoreCase(args[0], "SELECT") && args.size() == 2) {
                std::int64_t index = 0;
                if (parseInt64(args[1], index) && index >= 0 && index < m_server.databaseCount()) {
                    m_masterStreamDb = static_cast<int>(index);
                }
            } else {
                m_server.executeFromStream(m_masterStreamDb, args);
            }
        }

        m_appliedOffset += consumed;
    }

    // Compact the buffer once the consumed prefix gets large, rather than on
    // every command, so a pipelined burst is not quadratic.
    if (m_masterBufferOffset > 64 * 1024) {
        m_masterBuffer.erase(0, m_masterBufferOffset);
        m_masterBufferOffset = 0;
    }
}

std::string ReplicationManager::infoSection() const {
    std::string out;

    if (isReplica()) {
        out += "role:slave\r\n";
        out += "master_host:" + m_masterHost + "\r\n";
        out += "master_port:" + formatInt64(m_masterPort) + "\r\n";
        out += std::string("master_link_status:") + (linkIsUp() ? "up" : "down") + "\r\n";
        out += "master_repl_state:" + std::string(replicaLinkStateName(m_linkState)) + "\r\n";
        out += "slave_read_only:" + std::string(m_server.config().replicaReadOnly ? "1" : "0") + "\r\n";
        out += "slave_repl_offset:" + formatInt64(m_appliedOffset) + "\r\n";
        out += "master_replid:" + (m_masterReplicationId.empty() ? m_replicationId : m_masterReplicationId) + "\r\n";
    } else {
        out += "role:master\r\n";
        const std::vector<Client*> replicas = attachedReplicas();
        out += "connected_slaves:" + formatInt64(static_cast<std::int64_t>(replicas.size())) + "\r\n";

        const Millis now = nowMillis();
        for (size_t i = 0; i < replicas.size(); ++i) {
            const Client* replica = replicas[i];
            const std::string host = replica->peerAddress.substr(0, replica->peerAddress.find(':'));
            const std::string port = replica->replicaListeningPort.empty()
                                         ? replica->peerAddress.substr(replica->peerAddress.find(':') + 1)
                                         : replica->replicaListeningPort;
            out += "slave" + formatInt64(static_cast<std::int64_t>(i)) + ":ip=" + host +
                   ",port=" + port +
                   ",state=online,offset=" + formatInt64(replica->replicaAckOffset) +
                   ",lag=" + formatInt64((now - replica->replicaAckTime) / 1000) + "\r\n";
        }
        out += "master_replid:" + m_replicationId + "\r\n";
    }

    out += "master_repl_offset:" + formatInt64(isReplica() ? m_appliedOffset : m_masterOffset) + "\r\n";
    out += "repl_backlog_size:" + formatInt64(static_cast<std::int64_t>(m_backlog.capacity())) + "\r\n";
    out += "repl_backlog_first_byte_offset:" + formatInt64(m_backlog.firstAvailableOffset()) + "\r\n";
    out += "repl_backlog_histlen:" + formatInt64(m_backlog.histlen()) + "\r\n";
    return out;
}

} // namespace miniredis
