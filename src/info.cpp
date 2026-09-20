// INFO: the server's self-report.
//
// This is the first thing anyone looks at when a Redis deployment misbehaves,
// so the fields worth having are the ones that answer a real question:
// is memory about to run out, is the replica keeping up, is the AOF actually
// being written, and is the cache hit rate what we think it is.
#include "miniredis/aof.hpp"
#include "miniredis/cluster.hpp"
#include "miniredis/pubsub.hpp"
#include "miniredis/replication.hpp"
#include "miniredis/server.hpp"

#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace miniredis {

namespace {

void appendField(std::string& out, std::string_view name, std::string_view value) {
    out.append(name);
    out.push_back(':');
    out.append(value);
    out.append("\r\n");
}

void appendField(std::string& out, std::string_view name, std::int64_t value) {
    appendField(out, name, formatInt64(value));
}

bool wants(const std::string& requested, const char* section) {
    return requested == "all" || requested == "everything" || requested == "default" ||
           equalsIgnoreCase(requested, section);
}

// Hit rate is what actually tells you whether a cache is doing its job, and
// it is irritating to compute by hand from two counters, so INFO reports it.
std::string hitRatePercent(std::uint64_t hits, std::uint64_t misses) {
    const std::uint64_t total = hits + misses;
    if (total == 0) return "n/a";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", (static_cast<double>(hits) * 100.0) / static_cast<double>(total));
    return buffer;
}

} // namespace

std::string Server::infoReport(const std::string& section) const {
    const std::string requested = section.empty() ? "default" : toLower(section);
    const Millis now = nowMillis();
    std::string out;

    if (wants(requested, "server")) {
        out += "# Server\r\n";
        appendField(out, "miniredis_version", "1.0.0");
        appendField(out, "process_id", static_cast<std::int64_t>(
#ifdef _WIN32
            GetCurrentProcessId()
#else
            ::getpid()
#endif
            ));
        appendField(out, "node_id", m_config.nodeId);
        appendField(out, "tcp_port", m_config.port);
        appendField(out, "uptime_in_seconds", (now - m_startTime) / 1000);
        appendField(out, "uptime_in_days", (now - m_startTime) / 86400000);
        appendField(out, "config_file", m_config.workingDir);
        appendField(out, "executable_mode", m_config.clusterEnabled ? "cluster" : "standalone");
        out += "\r\n";
    }

    if (wants(requested, "clients")) {
        out += "# Clients\r\n";
        appendField(out, "connected_clients", static_cast<std::int64_t>(m_clients.size()));
        appendField(out, "maxclients", static_cast<std::int64_t>(m_config.maxClients));

        std::int64_t biggestInput = 0;
        std::int64_t biggestOutput = 0;
        std::int64_t blockedOnPubsub = 0;
        for (const auto& [id, client] : m_clients) {
            biggestInput = std::max(biggestInput, static_cast<std::int64_t>(client->inputBuffer.size()));
            biggestOutput = std::max(biggestOutput, static_cast<std::int64_t>(client->pendingOutputBytes()));
            if (client->isSubscribeMode()) ++blockedOnPubsub;
        }
        appendField(out, "client_recent_max_input_buffer", biggestInput);
        appendField(out, "client_recent_max_output_buffer", biggestOutput);
        appendField(out, "pubsub_clients", blockedOnPubsub);
        out += "\r\n";
    }

    if (wants(requested, "memory")) {
        const std::int64_t used = usedMemory();
        out += "# Memory\r\n";
        appendField(out, "used_memory", used);
        appendField(out, "used_memory_human", formatMemorySize(used));
        appendField(out, "maxmemory", m_config.maxMemoryBytes);
        appendField(out, "maxmemory_human",
                    m_config.maxMemoryBytes > 0 ? formatMemorySize(m_config.maxMemoryBytes) : "unlimited");
        appendField(out, "maxmemory_policy", evictionPolicyName(m_config.evictionPolicy));
        appendField(out, "maxmemory_samples", static_cast<std::int64_t>(m_config.evictionSamples));

        // How close to the limit we are, which is the number that actually
        // predicts whether the next write gets an OOM error.
        if (m_config.maxMemoryBytes > 0) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.2f",
                          (static_cast<double>(used) * 100.0) / static_cast<double>(m_config.maxMemoryBytes));
            appendField(out, "used_memory_percent_of_max", buffer);
        }
        out += "\r\n";
    }

    if (wants(requested, "persistence")) {
        out += "# Persistence\r\n";
        appendField(out, "loading", 0);
        appendField(out, "rdb_changes_since_last_save", m_dirty - m_dirtyAtLastSave);
        appendField(out, "rdb_last_save_time", m_lastSaveTime / 1000);
        appendField(out, "rdb_filename", m_config.rdbFilename);

        std::string saveRules;
        for (const auto& rule : m_config.saveRules) {
            if (!saveRules.empty()) saveRules += ' ';
            saveRules += formatInt64(rule.seconds) + ":" + formatInt64(rule.changes);
        }
        appendField(out, "rdb_save_rules", saveRules.empty() ? "disabled" : saveRules);

        appendField(out, "aof_enabled", m_aof->isEnabled() ? 1 : 0);
        appendField(out, "aof_filename", m_config.appendFilename);
        appendField(out, "aof_fsync_policy", aofFsyncPolicyName(m_config.appendFsync));
        appendField(out, "aof_current_size", m_aof->currentSizeBytes());
        appendField(out, "aof_current_size_human", formatMemorySize(m_aof->currentSizeBytes()));
        appendField(out, "aof_base_size", m_aof->sizeAtLastRewrite());
        appendField(out, "aof_rewrite_in_progress", m_aof->rewriteInProgress() ? 1 : 0);
        appendField(out, "aof_rewrite_count", static_cast<std::int64_t>(m_aof->rewriteCount()));
        appendField(out, "aof_last_write_status", m_aof->lastWriteFailed() ? "err" : "ok");
        out += "\r\n";
    }

    if (wants(requested, "stats")) {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        for (const auto& database : m_databases) {
            hits += database->hits();
            misses += database->misses();
        }

        out += "# Stats\r\n";
        appendField(out, "total_connections_received", static_cast<std::int64_t>(m_stats.totalConnections));
        appendField(out, "total_commands_processed", static_cast<std::int64_t>(m_stats.totalCommands));
        appendField(out, "rejected_connections", static_cast<std::int64_t>(m_stats.rejectedConnections));
        appendField(out, "expired_keys", static_cast<std::int64_t>(m_stats.expiredKeys));
        appendField(out, "evicted_keys", static_cast<std::int64_t>(m_stats.evictedKeys));
        appendField(out, "keyspace_hits", static_cast<std::int64_t>(hits));
        appendField(out, "keyspace_misses", static_cast<std::int64_t>(misses));
        appendField(out, "keyspace_hit_rate_percent", hitRatePercent(hits, misses));
        appendField(out, "pubsub_channels", static_cast<std::int64_t>(m_pubsub->channelsMatching(nullptr).size()));
        appendField(out, "pubsub_patterns", static_cast<std::int64_t>(m_pubsub->patternCount()));
        appendField(out, "pubsub_messages_delivered", static_cast<std::int64_t>(m_stats.pubsubMessages));
        appendField(out, "sync_full", static_cast<std::int64_t>(m_stats.syncFull));
        appendField(out, "sync_partial_ok", static_cast<std::int64_t>(m_stats.syncPartialOk));
        appendField(out, "sync_partial_err", static_cast<std::int64_t>(m_stats.syncPartialErr));
        appendField(out, "slowlog_len", static_cast<std::int64_t>(m_slowLog.size()));
        out += "\r\n";
    }

    if (wants(requested, "replication")) {
        out += "# Replication\r\n";
        out += m_replication->infoSection();
        out += "\r\n";
    }

    if (m_config.clusterEnabled && wants(requested, "cluster")) {
        out += "# Cluster\r\n";
        out += m_cluster->describeInfo();
        out += "\r\n";
    }

    if (wants(requested, "keyspace")) {
        out += "# Keyspace\r\n";
        for (const auto& database : m_databases) {
            if (database->empty()) continue;
            appendField(out, "db" + formatInt64(database->index()),
                        "keys=" + formatInt64(static_cast<std::int64_t>(database->size())) +
                            ",expires=" + formatInt64(static_cast<std::int64_t>(database->volatileCount())) +
                            ",avg_ttl=0");
        }
        out += "\r\n";
    }

    return out;
}

} // namespace miniredis
