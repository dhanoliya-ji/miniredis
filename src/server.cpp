#include "miniredis/server.hpp"

#include "miniredis/aof.hpp"
#include "miniredis/cluster.hpp"
#include "miniredis/pubsub.hpp"
#include "miniredis/rdb.hpp"
#include "miniredis/replication.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace miniredis {

namespace {

// How long poll() waits when there is nothing to do. Short enough that the
// cron still runs on time, long enough that an idle server uses no CPU.
constexpr int kPollTimeoutMs = 100;

// The cron tick, matching Redis's default hz of 10.
constexpr Millis kCronIntervalMs = 100;

// Active expiry: how many volatile keys to sample per database per tick, and
// the "still finding lots of dead keys" threshold that triggers another round.
constexpr size_t kActiveExpireSampleSize = 20;
constexpr double kActiveExpireContinueRatio = 0.25;
constexpr int kActiveExpireMaxRounds = 16;

// Read buffer size per readable event.
constexpr size_t kReadChunkSize = 64 * 1024;

// A client whose unsent replies exceed this is disconnected. Without a cap, a
// subscriber that never reads would make the server buffer without bound until
// it is killed by the OOM killer.
constexpr size_t kMaxClientOutputBuffer = 256 * 1024 * 1024;

// Once this many bytes at the front of the input buffer have been consumed,
// compact it. Doing it per command would make a pipelined burst quadratic.
constexpr size_t kInputCompactThreshold = 64 * 1024;

const char* logLevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:   return ".";
        case LogLevel::Verbose: return "-";
        case LogLevel::Notice:  return "*";
        case LogLevel::Warning: return "#";
    }
    return "*";
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Server::Server(ServerConfig config)
    : m_config(std::move(config)), m_commands(buildCommandTable()) {
    m_databases.reserve(static_cast<size_t>(m_config.databaseCount));
    for (int i = 0; i < m_config.databaseCount; ++i) {
        m_databases.push_back(std::make_unique<Keyspace>(i));
    }

    m_aof = std::make_unique<Aof>(*this);
    m_pubsub = std::make_unique<PubSub>(*this);
    m_replication = std::make_unique<ReplicationManager>(*this);
    m_cluster = std::make_unique<ClusterState>(*this);

    m_startTime = nowMillis();
    m_lastSaveTime = m_startTime;
    m_lruClock = static_cast<std::uint32_t>(m_startTime / 1000);

    // Every database reports key removals back to the server, so an expiry or
    // an eviction becomes an explicit DEL in the AOF and on the replica link.
    for (auto& database : m_databases) {
        database->setLruClock(m_lruClock);
        database->setKeyRemovalHook([this](int dbIndex, const Bytes& key) {
            notifyKeyRemoved(dbIndex, key);
        });
    }
}

Server::~Server() {
    if (m_aof) m_aof->disable();
    for (auto& [id, client] : m_clients) {
        if (client->fd != kInvalidSocket) Net::closeSocket(client->fd);
    }
    if (m_listener != kInvalidSocket) Net::closeSocket(m_listener);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void Server::log(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(m_config.logLevel)) return;

    const std::time_t seconds = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%d %b %Y %H:%M:%S", &parts);

    const std::string line = std::string(timestamp) + " " + logLevelTag(level) + " " + message;

    if (m_config.logFile.empty()) {
        std::cout << line << std::endl;
    } else {
        std::ofstream file(m_config.resolvePath(m_config.logFile), std::ios::app);
        if (file.is_open()) file << line << '\n';
    }
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

bool Server::start(std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(m_config.workingDir, ec);

    m_listener = Net::listenOn(m_config.bindAddress, m_config.port, m_config.tcpBacklog, error);
    if (m_listener == kInvalidSocket) return false;

    log(LogLevel::Notice, "MiniRedis " + m_config.nodeId + " listening on " +
                              m_config.bindAddress + ":" + formatInt64(m_config.port));
    log(LogLevel::Notice, std::string("command table holds ") +
                              formatInt64(static_cast<std::int64_t>(m_commands.size())) + " commands");

    if (m_config.clusterEnabled) {
        const std::string clusterPath = m_config.resolvePath(m_config.clusterConfigFile);
        std::string clusterError;
        if (!m_cluster->loadConfig(clusterPath, clusterError)) {
            // No usable config yet: become a single-node cluster owning every
            // slot, and persist that so the identity survives a restart.
            m_cluster->initialiseSingleNode(m_config.nodeId, m_config.bindAddress, m_config.port);
            std::string saveError;
            m_cluster->saveConfig(clusterPath, saveError);
            log(LogLevel::Notice, "cluster mode enabled; this node owns all " +
                                      formatInt64(kClusterSlots) + " hash slots");
        } else {
            log(LogLevel::Notice, "cluster config loaded: " +
                                      formatInt64(m_cluster->assignedSlotCount()) + " of " +
                                      formatInt64(kClusterSlots) + " slots assigned");
        }
    }

    if (m_config.loadOnStartup) {
        std::string loadError;
        if (!loadDataset(loadError)) {
            log(LogLevel::Warning, "starting with an empty dataset: " + loadError);
        }
    }

    if (m_config.appendOnly) {
        std::string aofError;
        if (!m_aof->enable(aofError)) {
            error = aofError;
            return false;
        }
        log(LogLevel::Notice, "append-only file enabled (fsync policy: " +
                                  std::string(aofFsyncPolicyName(m_config.appendFsync)) + ")");
    }

    if (!m_config.replicaOfHost.empty()) {
        m_replication->replicaOf(m_config.replicaOfHost, m_config.replicaOfPort);
    }

    m_running = true;
    m_lastCronRun = nowMillis();
    return true;
}

bool Server::loadDataset(std::string& error) {
    // The AOF wins when both exist, because it is the more recent record: the
    // snapshot is a point in time, the log continues past it.
    const std::string aofPath = m_config.resolvePath(m_config.appendFilename);
    if (m_config.appendOnly && std::filesystem::exists(aofPath)) {
        bool truncated = false;
        if (!m_aof->load(aofPath, truncated, error)) return false;

        std::int64_t keys = 0;
        for (auto& database : m_databases) keys += static_cast<std::int64_t>(database->size());
        log(LogLevel::Notice, "loaded " + formatInt64(keys) + " keys from the append-only file" +
                                  (truncated ? " (trailing partial command discarded)" : ""));
        return true;
    }

    const std::string rdbPath = m_config.resolvePath(m_config.rdbFilename);
    if (!std::filesystem::exists(rdbPath)) {
        error = "no dataset file found";
        return false;
    }

    RdbFile snapshot(*this);
    if (!snapshot.load(rdbPath, error)) return false;

    log(LogLevel::Notice, "loaded " + formatInt64(static_cast<std::int64_t>(snapshot.keysLoaded())) +
                              " keys from " + m_config.rdbFilename);
    return true;
}

bool Server::saveSnapshot(std::string& error) {
    RdbFile snapshot(*this);
    const std::string path = m_config.resolvePath(m_config.rdbFilename);
    if (!snapshot.save(path, error)) return false;

    m_dirtyAtLastSave = m_dirty;
    m_lastSaveTime = nowMillis();
    log(LogLevel::Notice, "snapshot written: " +
                              formatInt64(static_cast<std::int64_t>(snapshot.keysWritten())) + " keys");
    return true;
}

void Server::swapDatabases(int first, int second) {
    if (first == second) return;
    auto& a = m_databases[static_cast<size_t>(first)];
    auto& b = m_databases[static_cast<size_t>(second)];
    std::swap(a, b);
    a->setIndex(first);
    b->setIndex(second);
}

void Server::flushAllDatabases() {
    for (auto& database : m_databases) database->clear();
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

int Server::run() {
    std::vector<PollRequest> requests;
    std::vector<std::uint64_t> pollClientIds;

    while (m_running) {
        requests.clear();
        pollClientIds.clear();

        // The listener is always watched for readability: a readable listening
        // socket means a pending connection.
        requests.push_back(PollRequest{m_listener, true, false, false});
        pollClientIds.push_back(0);

        // The replica's link to its master is just another socket in the same
        // poll set, so replication needs no thread of its own.
        if (m_replicationLinkFd != kInvalidSocket) {
            requests.push_back(PollRequest{m_replicationLinkFd, true, false, false});
            pollClientIds.push_back(0);
        }
        const size_t replicationLinkIndex =
            (m_replicationLinkFd != kInvalidSocket) ? 1 : SIZE_MAX;

        for (const auto& [id, client] : m_clients) {
            if (client->fd == kInvalidSocket || client->shouldClose) continue;
            PollRequest request;
            request.fd = client->fd;
            request.readable = true;
            // Only ask about writability when there is something to write.
            // Asking unconditionally would make poll() return immediately
            // every iteration and spin the CPU at 100%.
            request.writable = client->pendingOutputBytes() > 0;
            requests.push_back(request);
            pollClientIds.push_back(id);
        }

        const int ready = Net::pollSockets(requests, kPollTimeoutMs);
        if (ready < 0) {
            log(LogLevel::Warning, "poll failed: " + Net::lastErrorString());
            break;
        }

        if (ready > 0) {
            if (requests[0].readable) acceptNewClients();

            if (replicationLinkIndex != SIZE_MAX && replicationLinkIndex < requests.size()) {
                const PollRequest& linkEvent = requests[replicationLinkIndex];
                if (linkEvent.readable || linkEvent.errored) {
                    m_replication->onMasterDataAvailable();
                }
            }

            for (size_t i = 0; i < requests.size(); ++i) {
                const std::uint64_t id = pollClientIds[i];
                if (id == 0) continue;

                Client* client = findClient(id);
                if (client == nullptr || client->shouldClose) continue;

                if (requests[i].errored) {
                    client->shouldClose = true;
                    continue;
                }
                if (requests[i].readable) readFromClient(*client);
                if (!client->shouldClose && requests[i].writable) writeToClient(*client);
            }
        }

        // Commands buffered during this iteration are written to the AOF once,
        // not once per command, so a pipeline costs a single write().
        m_aof->flush(nowMillis());

        const Millis now = nowMillis();
        if (now - m_lastCronRun >= kCronIntervalMs) {
            serverCron();
            m_lastCronRun = now;
        }

        m_replication->driveReplicaLink(now);
        removeClosedClients();
    }

    log(LogLevel::Notice, "shutting down");
    m_aof->disable();
    return 0;
}

void Server::acceptNewClients() {
    // Drain the accept queue rather than taking one per loop iteration, so a
    // burst of connections is not spread across many polls.
    while (true) {
        std::string peerAddress;
        SocketHandle fd = Net::acceptConnection(m_listener, peerAddress);
        if (fd == kInvalidSocket) break;

        if (m_clients.size() >= m_config.maxClients) {
            const char* message = "-ERR max number of clients reached\r\n";
            Net::writeAll(fd, message, std::strlen(message), 100);
            Net::closeSocket(fd);
            ++m_stats.rejectedConnections;
            continue;
        }

        auto client = std::make_unique<Client>();
        client->id = m_nextClientId++;
        client->fd = fd;
        client->peerAddress = peerAddress;
        client->createdAt = nowMillis();
        client->lastInteraction = client->createdAt;
        // With no password configured, every connection starts authenticated.
        client->authenticated = m_config.requirePass.empty();

        const std::uint64_t id = client->id;
        m_clients.emplace(id, std::move(client));
        ++m_stats.totalConnections;

        log(LogLevel::Verbose, "accepted connection from " + peerAddress);
    }
}

void Server::readFromClient(Client& client) {
    char chunk[kReadChunkSize];

    while (true) {
        size_t received = 0;
        const IoResult result = Net::readSome(client.fd, chunk, sizeof(chunk), received);

        if (result == IoResult::Ok) {
            client.inputBuffer.append(chunk, received);
            client.lastInteraction = nowMillis();
            // A short read means the socket is drained; going round again
            // would just return WouldBlock.
            if (received < sizeof(chunk)) break;
            continue;
        }
        if (result == IoResult::WouldBlock) break;

        client.shouldClose = true;
        return;
    }

    processInputBuffer(client);
}

void Server::processInputBuffer(Client& client) {
    while (client.inputOffset < client.inputBuffer.size() && !client.shouldClose) {
        Args args;
        std::string parseError;
        const size_t commandStart = client.inputOffset;

        const ParseStatus status =
            RespParser::parseCommand(client.inputBuffer, client.inputOffset, args, parseError);

        if (status == ParseStatus::Incomplete) {
            // Rewind and wait for more bytes. This is the whole reason the
            // parser is incremental: a command split across TCP segments must
            // not consume anything.
            client.inputOffset = commandStart;
            break;
        }

        if (status == ParseStatus::Invalid) {
            addReply(client, "-" + parseError + "\r\n");
            client.closeAfterReply = true;
            client.inputOffset = client.inputBuffer.size();
            break;
        }

        if (!args.empty()) {
            executeCommand(client, args);
        }
    }

    if (client.inputOffset >= client.inputBuffer.size()) {
        client.inputBuffer.clear();
        client.inputOffset = 0;
    } else if (client.inputOffset > kInputCompactThreshold) {
        client.inputBuffer.erase(0, client.inputOffset);
        client.inputOffset = 0;
    }

    if (client.closeAfterReply && client.pendingOutputBytes() == 0) {
        client.shouldClose = true;
    }
}

void Server::addReply(Client& client, std::string_view bytes) {
    if (bytes.empty()) return;

    client.outputBuffer.append(bytes);

    if (client.pendingOutputBytes() > kMaxClientOutputBuffer) {
        log(LogLevel::Warning, "client " + client.peerAddress +
                                   " exceeded the output buffer limit and was disconnected");
        client.shouldClose = true;
        return;
    }

    // Try to hand the bytes to the kernel immediately. Most replies fit in the
    // socket buffer and leave nothing for the event loop to do, which saves a
    // poll round trip on every command.
    writeToClient(client);
}

void Server::writeToClient(Client& client) {
    while (client.pendingOutputBytes() > 0) {
        size_t sent = 0;
        const IoResult result = Net::writeSome(client.fd,
                                               client.outputBuffer.data() + client.outputOffset,
                                               client.pendingOutputBytes(), sent);
        if (result == IoResult::Ok) {
            client.outputOffset += sent;
            continue;
        }
        if (result == IoResult::WouldBlock) {
            // The kernel buffer is full; the loop will poll for writability.
            break;
        }
        client.shouldClose = true;
        return;
    }

    if (client.outputOffset >= client.outputBuffer.size()) {
        client.outputBuffer.clear();
        client.outputOffset = 0;
        if (client.closeAfterReply) client.shouldClose = true;
    } else if (client.outputOffset > kInputCompactThreshold) {
        client.outputBuffer.erase(0, client.outputOffset);
        client.outputOffset = 0;
    }
}

void Server::disconnectClient(Client& client) {
    m_pubsub->unsubscribeAll(client);
    if (client.role == ClientRole::Replica) {
        m_replication->removeReplica(client);
    }
    if (client.fd != kInvalidSocket) {
        Net::closeSocket(client.fd);
    }
}

void Server::removeClosedClients() {
    std::vector<std::uint64_t> doomed;
    for (const auto& [id, client] : m_clients) {
        if (client->shouldClose) doomed.push_back(id);
    }

    for (const std::uint64_t id : doomed) {
        const auto it = m_clients.find(id);
        if (it == m_clients.end()) continue;
        disconnectClient(*it->second);
        m_clients.erase(it);
    }
}

Client* Server::findClient(std::uint64_t id) {
    const auto it = m_clients.find(id);
    return it == m_clients.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// Cron
// ---------------------------------------------------------------------------

void Server::serverCron() {
    const Millis now = nowMillis();

    // The coarse LRU clock only needs second resolution; reading the wall
    // clock on every key access would cost more than the eviction saves.
    m_lruClock = static_cast<std::uint32_t>(now / 1000);
    const std::uint16_t lfuMinute = static_cast<std::uint16_t>((now - m_startTime) / 60000);
    for (auto& database : m_databases) {
        database->setLruClock(m_lruClock);
        database->setLfuMinute(lfuMinute);
    }

    // A replica does not expire keys on its own: its master sends an explicit
    // DEL when a key dies. If both expired independently, a read on the
    // replica could return a different answer than the same read on the master
    // purely because their clocks differ by a few milliseconds.
    if (!m_replication->isReplica()) {
        for (auto& database : m_databases) {
            for (int round = 0; round < kActiveExpireMaxRounds; ++round) {
                double expiredRatio = 0.0;
                const size_t removed = database->activeExpireCycle(now, kActiveExpireSampleSize, expiredRatio);
                m_stats.expiredKeys += removed;
                // Keep going only while the sample is still mostly dead keys;
                // otherwise the cycle would burn CPU scanning live ones.
                if (expiredRatio < kActiveExpireContinueRatio) break;
            }
        }
    }

    if (m_config.clientTimeoutSeconds > 0) {
        const Millis idleLimit = static_cast<Millis>(m_config.clientTimeoutSeconds) * 1000;
        for (auto& [id, client] : m_clients) {
            // Replicas and subscribers are legitimately idle for long stretches.
            if (client->role != ClientRole::Normal || client->isSubscribeMode()) continue;
            if (now - client->lastInteraction > idleLimit) {
                client->shouldClose = true;
            }
        }
    }

    // Snapshot rules: "save 900 1" means save if at least one change happened
    // in the last 900 seconds.
    if (!m_config.saveRules.empty()) {
        const std::int64_t changes = m_dirty - m_dirtyAtLastSave;
        const Millis sinceLastSave = now - m_lastSaveTime;
        for (const auto& rule : m_config.saveRules) {
            if (changes >= rule.changes && sinceLastSave >= static_cast<Millis>(rule.seconds) * 1000) {
                std::string error;
                if (!saveSnapshot(error)) {
                    log(LogLevel::Warning, "scheduled snapshot failed: " + error);
                }
                break;
            }
        }
    }

    if (m_aof->shouldAutoRewrite()) {
        std::string error;
        if (!m_aof->rewrite(error)) {
            log(LogLevel::Warning, "automatic append-only rewrite failed: " + error);
        }
    }

    // Keep replica offsets fresh so INFO reports real lag and WAIT can count.
    static Millis lastAckRequest = 0;
    if (!m_replication->isReplica() && now - lastAckRequest >= 1000) {
        m_replication->requestAcksFromReplicas();
        lastAckRequest = now;
    }
}

// ---------------------------------------------------------------------------
// Memory and eviction
// ---------------------------------------------------------------------------

std::int64_t Server::usedMemory() const {
    std::int64_t total = 0;
    for (const auto& database : m_databases) {
        total += static_cast<std::int64_t>(database->memoryUsage());
    }
    for (const auto& [id, client] : m_clients) {
        total += static_cast<std::int64_t>(client->inputBuffer.capacity() + client->outputBuffer.capacity());
    }
    return total;
}

bool Server::freeMemoryIfNeeded() {
    if (m_config.maxMemoryBytes <= 0) return true;

    std::int64_t used = usedMemory();
    if (used <= m_config.maxMemoryBytes) return true;

    if (m_config.evictionPolicy == EvictionPolicy::NoEviction) {
        return false; // the caller turns this into an OOM error
    }

    // Bound the work: without a cap, a maxmemory set below the size of a
    // single large value would spin here forever.
    constexpr int kMaxEvictionRounds = 10000;
    int evicted = 0;

    for (int round = 0; round < kMaxEvictionRounds && used > m_config.maxMemoryBytes; ++round) {
        EvictionCandidate best;
        int bestDb = -1;

        // Sample each database and take the best candidate overall, so a
        // single hot database does not shield a cold one from eviction.
        for (int index = 0; index < databaseCount(); ++index) {
            EvictionCandidate candidate = m_databases[static_cast<size_t>(index)]->sampleEvictionCandidate(
                m_config.evictionPolicy, m_config.evictionSamples, m_lruClock, nowMillis());
            if (candidate.valid && (!best.valid || candidate.idleScore > best.idleScore)) {
                best = std::move(candidate);
                bestDb = index;
            }
        }

        if (!best.valid || bestDb < 0) break; // nothing left that the policy may evict

        notifyKeyRemoved(bestDb, best.key);
        m_databases[static_cast<size_t>(bestDb)]->erase(best.key);
        ++m_stats.evictedKeys;
        ++evicted;

        used = usedMemory();
    }

    if (evicted > 0) {
        log(LogLevel::Verbose, "evicted " + formatInt64(evicted) + " keys to stay under maxmemory");
    }
    return used <= m_config.maxMemoryBytes;
}

void Server::notifyKeyRemoved(int dbIndex, const Bytes& key) {
    // An expiry or eviction is a keyspace change like any other: it has to
    // reach the AOF and every replica, or they will diverge.
    signalKeyModified(dbIndex, key);

    if (m_replication->isReplica()) return; // replicas apply their master's DELs

    const Args del{"DEL", key};
    m_aof->feed(dbIndex, del);

    if (m_replication->replicaCount() > 0) {
        std::string payload;
        if (dbIndex != 0) payload += encodeCommand(Args{"SELECT", formatInt64(dbIndex)});
        payload += encodeCommand(del);
        m_replication->propagateToReplicas(payload);
    }
}

void Server::signalKeyModified(int dbIndex, const Bytes& key) {
    // Invalidate every transaction watching this key. This is the entire
    // mechanism behind WATCH: EXEC checks the flag rather than comparing
    // values, so it catches a value that changed and changed back too.
    for (auto& [id, client] : m_clients) {
        if (client->watchedKeys.empty() || client->watchInvalidated) continue;
        for (const auto& watched : client->watchedKeys) {
            if (watched.dbIndex == dbIndex && watched.key == key) {
                client->watchInvalidated = true;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Command execution
// ---------------------------------------------------------------------------

void Server::propagate(int dbIndex, const Args& args) {
    if (args.empty()) return;

    m_aof->feed(dbIndex, args);

    if (m_replication->replicaCount() > 0 || m_replication->isMaster()) {
        std::string payload;
        if (dbIndex != 0) payload += encodeCommand(Args{"SELECT", formatInt64(dbIndex)});
        payload += encodeCommand(args);
        m_replication->propagateToReplicas(payload);
    }
}

void Server::executeFromStream(int dbIndex, const Args& args) {
    const CommandSpec* spec = m_commands.find(args[0]);
    if (spec == nullptr || !spec->matchesArity(args.size())) return;

    // A synthetic client that owns no socket. Its replies go nowhere, which is
    // exactly right: nobody asked for them.
    Client streamClient;
    streamClient.id = 0;
    streamClient.dbIndex = dbIndex;
    streamClient.authenticated = true;

    std::string discardedReply;
    CommandContext context{*this, streamClient, args, discardedReply, nowMillis()};

    const bool wasExecuting = m_inExecution;
    m_inExecution = true;
    spec->handler(context);
    m_inExecution = wasExecuting;

    // A replica re-propagates what it applied so that chained replication
    // (a replica of a replica) works.
    if (spec->isWrite() && !context.propagationSuppressed) {
        m_aof->feed(dbIndex, context.hasPropagationOverride ? context.propagationOverride : args);
    }
}

void Server::executeCommand(Client& client, const Args& args) {
    const std::int64_t startMicros = monotonicMicros();
    const Millis now = nowMillis();
    client.lastInteraction = now;
    ++client.commandsProcessed;
    ++m_stats.totalCommands;

    RespWriter writer(client.outputBuffer);
    const std::string commandName = toUpper(args[0]);

    const CommandSpec* spec = m_commands.find(args[0]);
    if (spec == nullptr) {
        std::string message = "ERR unknown command '" + args[0] + "', with args beginning with: ";
        for (size_t i = 1; i < args.size() && i < 4; ++i) {
            message += "'" + args[i] + "' ";
        }
        // Inside a transaction, a command that cannot even be queued poisons
        // the whole batch. EXEC will then refuse to run any of it, rather than
        // silently dropping one operation from a sequence the client believes
        // is complete.
        if (client.inMulti) client.multiQueueError = true;
        addReply(client, "-" + message + "\r\n");
        return;
    }

    if (!spec->matchesArity(args.size())) {
        if (client.inMulti && (spec->flags & cmdflag::kNoMulti) == 0) client.multiQueueError = true;
        addReply(client, "-" + wrongArgsError(spec->name) + "\r\n");
        return;
    }

    // Authentication gates everything except the handful of commands that must
    // work before you are authenticated.
    if (!m_config.requirePass.empty() && !client.authenticated &&
        (spec->flags & cmdflag::kNoAuth) == 0) {
        addReply(client, "-NOAUTH Authentication required.\r\n");
        return;
    }

    // While subscribed, RESP2 only allows the pub/sub control commands. The
    // connection is in a push-message mode and a normal reply would be
    // indistinguishable from a delivered message.
    if (client.isSubscribeMode() && (spec->flags & cmdflag::kPubSub) == 0) {
        addReply(client, "-ERR Can't execute '" + toLower(spec->name) +
                             "': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT are allowed in this context\r\n");
        return;
    }

    // Queue rather than run, when a transaction is open.
    if (client.inMulti && (spec->flags & cmdflag::kNoMulti) == 0) {
        client.multiQueue.push_back(args);
        addReply(client, "+QUEUED\r\n");
        return;
    }

    // Cluster routing: a node serves only the slots it owns.
    if (m_cluster->isEnabled() && spec->firstKey > 0) {
        const std::vector<Bytes> keys = m_commands.extractKeys(*spec, args);
        const RoutingDecision decision = m_cluster->route(keys);
        switch (decision.result) {
            case SlotRouting::Moved:
                addReply(client, "-MOVED " + formatInt64(decision.slot) + " " + decision.targetAddress + "\r\n");
                return;
            case SlotRouting::CrossSlot:
                addReply(client, "-CROSSSLOT Keys in request don't hash to the same slot\r\n");
                return;
            case SlotRouting::Down:
                addReply(client, "-CLUSTERDOWN Hash slot not served\r\n");
                return;
            case SlotRouting::Local:
                break;
        }
    }

    const bool isWrite = spec->isWrite();

    // A read-only replica refuses writes from ordinary clients. Without this
    // the replica would diverge from its master and the next resync would
    // silently discard the local write.
    if (isWrite && m_replication->isReplica() && m_config.replicaReadOnly) {
        addReply(client, std::string("-") + err::kReadOnlyReplica + "\r\n");
        return;
    }

    // Memory pressure: evict if we can, refuse the command if we cannot.
    if ((spec->flags & cmdflag::kDenyOom) != 0) {
        if (!freeMemoryIfNeeded()) {
            addReply(client, std::string("-") + err::kOom + "\r\n");
            return;
        }
    }

    feedMonitors(client, args);

    const std::int64_t dirtyBefore = m_dirty;

    std::string reply;
    CommandContext context{*this, client, args, reply, now};
    spec->handler(context);

    addReply(client, reply);

    // Propagate only when the command actually changed something. SET on an
    // existing identical value, DEL of a missing key and SADD of a present
    // member are all writes that changed nothing, and shipping them would
    // inflate the AOF and the replication stream for no reason.
    if (isWrite && !context.propagationSuppressed && m_dirty != dirtyBefore) {
        propagate(client.dbIndex,
                  context.hasPropagationOverride ? context.propagationOverride : args);
    }

    const std::int64_t durationMicros = monotonicMicros() - startMicros;
    recordSlowLog(client, args, durationMicros);

    (void)commandName;
}

void Server::feedMonitors(Client& source, const Args& args) {
    for (auto& [id, client] : m_clients) {
        if (client->role != ClientRole::Monitor || client->id == source.id) continue;

        // MONITOR's format is "<unix.micros> [db addr] "cmd" "arg"...".
        std::string line = "+";
        line += formatInt64(nowMillis() / 1000);
        line += '.';
        line += formatInt64(nowMillis() % 1000);
        line += " [";
        line += formatInt64(source.dbIndex);
        line += ' ';
        line += source.peerAddress;
        line += "]";
        for (const auto& arg : args) {
            line += " \"";
            line += arg;
            line += '"';
        }
        line += "\r\n";
        addReply(*client, line);
    }
}

void Server::recordSlowLog(const Client& client, const Args& args, std::int64_t durationMicros) {
    if (m_config.slowLogThresholdMicros < 0) return; // logging disabled
    if (durationMicros < m_config.slowLogThresholdMicros) return;
    if (m_config.slowLogMaxLength == 0) return;

    SlowLogEntry entry;
    entry.id = m_nextSlowLogId++;
    entry.timestamp = nowMillis();
    entry.durationMicros = durationMicros;
    entry.clientAddress = client.peerAddress;
    entry.clientName = client.name;

    // Long commands are truncated, so that one MSET with a hundred thousand
    // arguments cannot pin that much memory in the slow log indefinitely.
    constexpr size_t kMaxLoggedArgs = 32;
    constexpr size_t kMaxLoggedArgLength = 128;

    for (size_t i = 0; i < args.size() && i < kMaxLoggedArgs; ++i) {
        if (args[i].size() > kMaxLoggedArgLength) {
            entry.args.push_back(args[i].substr(0, kMaxLoggedArgLength) + "... (" +
                                 formatInt64(static_cast<std::int64_t>(args[i].size())) + " bytes)");
        } else {
            entry.args.push_back(args[i]);
        }
    }
    if (args.size() > kMaxLoggedArgs) {
        entry.args.push_back("... (" + formatInt64(static_cast<std::int64_t>(args.size() - kMaxLoggedArgs)) +
                             " more arguments)");
    }

    m_slowLog.push_front(std::move(entry));
    while (m_slowLog.size() > m_config.slowLogMaxLength) {
        m_slowLog.pop_back();
    }
}

} // namespace miniredis
