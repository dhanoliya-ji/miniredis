// Configuration: one directive table shared by the config file and CONFIG SET.
//
// Both paths funnel through applyDirective(), which is deliberate. When a
// config file parser and a CONFIG SET handler are written separately they
// drift, and you end up with a directive that can be set at startup but not at
// runtime, or one that accepts "512mb" in the file and only raw bytes at
// runtime.
#include "miniredis/server.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace miniredis {

bool parseAofFsyncPolicy(std::string_view name, AofFsyncPolicy& out) {
    if (equalsIgnoreCase(name, "always"))   { out = AofFsyncPolicy::Always;   return true; }
    if (equalsIgnoreCase(name, "everysec")) { out = AofFsyncPolicy::EverySec; return true; }
    if (equalsIgnoreCase(name, "no"))       { out = AofFsyncPolicy::No;       return true; }
    return false;
}

const char* aofFsyncPolicyName(AofFsyncPolicy policy) {
    switch (policy) {
        case AofFsyncPolicy::Always:   return "always";
        case AofFsyncPolicy::EverySec: return "everysec";
        case AofFsyncPolicy::No:       return "no";
    }
    return "everysec";
}

namespace {

bool parseBoolean(std::string_view text, bool& out) {
    if (equalsIgnoreCase(text, "yes") || equalsIgnoreCase(text, "true") || text == "1") {
        out = true;
        return true;
    }
    if (equalsIgnoreCase(text, "no") || equalsIgnoreCase(text, "false") || text == "0") {
        out = false;
        return true;
    }
    return false;
}

const char* booleanText(bool value) { return value ? "yes" : "no"; }

bool parseLogLevel(std::string_view text, LogLevel& out) {
    if (equalsIgnoreCase(text, "debug"))   { out = LogLevel::Debug;   return true; }
    if (equalsIgnoreCase(text, "verbose")) { out = LogLevel::Verbose; return true; }
    if (equalsIgnoreCase(text, "notice"))  { out = LogLevel::Notice;  return true; }
    if (equalsIgnoreCase(text, "warning")) { out = LogLevel::Warning; return true; }
    return false;
}

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:   return "debug";
        case LogLevel::Verbose: return "verbose";
        case LogLevel::Notice:  return "notice";
        case LogLevel::Warning: return "warning";
    }
    return "notice";
}

// Splits a config line into tokens, honouring quotes so that a password or a
// path containing spaces survives.
std::vector<std::string> tokenise(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool haveToken = false;
    char quote = 0;

    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < line.size()) {
                current.push_back(line[++i]);
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
                tokens.push_back(current);
                current.clear();
                haveToken = false;
            }
        } else if (c == '#' && !haveToken) {
            break; // a comment, but only when it starts a token
        } else {
            current.push_back(c);
            haveToken = true;
        }
    }
    if (haveToken) tokens.push_back(current);
    return tokens;
}

} // namespace

std::string ServerConfig::resolvePath(const std::string& filename) const {
    std::filesystem::path path(filename);
    if (path.is_absolute()) return path.string();
    return (std::filesystem::path(workingDir) / path).string();
}

std::vector<std::string> ServerConfig::directiveNames() {
    return {
        "bind", "port", "tcp-backlog", "timeout", "maxclients",
        "nodeid", "dir", "databases", "requirepass",
        "maxmemory", "maxmemory-policy", "maxmemory-samples",
        "appendonly", "appendfilename", "appendfsync",
        "auto-aof-rewrite-percentage", "auto-aof-rewrite-min-size",
        "dbfilename", "save", "load-on-startup",
        "replicaof", "replica-read-only", "masterauth",
        "repl-timeout", "repl-backlog-size",
        "cluster-enabled", "cluster-config-file", "cluster-announce-ip", "cluster-announce-port",
        "loglevel", "logfile", "slowlog-log-slower-than", "slowlog-max-len",
    };
}

bool ServerConfig::applyDirective(const std::string& name, const std::vector<std::string>& values,
                                  std::string& error) {
    const std::string key = toLower(name);

    auto requireOne = [&]() -> bool {
        if (values.size() != 1) {
            error = "directive '" + key + "' expects exactly one value";
            return false;
        }
        return true;
    };

    auto readInt = [&](std::int64_t& target, std::int64_t min, std::int64_t max) -> bool {
        if (!requireOne()) return false;
        std::int64_t parsed = 0;
        if (!parseInt64(values[0], parsed)) {
            error = "directive '" + key + "' expects an integer, got '" + values[0] + "'";
            return false;
        }
        if (parsed < min || parsed > max) {
            error = "directive '" + key + "' must be between " + formatInt64(min) +
                    " and " + formatInt64(max);
            return false;
        }
        target = parsed;
        return true;
    };

    std::int64_t scratch = 0;

    if (key == "bind")        { if (!requireOne()) return false; bindAddress = values[0]; return true; }
    if (key == "port")        { if (!readInt(scratch, 1, 65535)) return false; port = static_cast<int>(scratch); return true; }
    if (key == "tcp-backlog") { if (!readInt(scratch, 1, 65535)) return false; tcpBacklog = static_cast<int>(scratch); return true; }
    if (key == "timeout")     { if (!readInt(scratch, 0, 86400)) return false; clientTimeoutSeconds = static_cast<int>(scratch); return true; }
    if (key == "maxclients")  { if (!readInt(scratch, 1, 1000000)) return false; maxClients = static_cast<size_t>(scratch); return true; }
    if (key == "nodeid")      { if (!requireOne()) return false; nodeId = values[0]; return true; }
    if (key == "dir")         { if (!requireOne()) return false; workingDir = values[0]; return true; }
    if (key == "databases")   { if (!readInt(scratch, 1, 1024)) return false; databaseCount = static_cast<int>(scratch); return true; }

    if (key == "requirepass") {
        if (values.empty()) { requirePass.clear(); return true; }
        if (!requireOne()) return false;
        requirePass = values[0];
        return true;
    }

    if (key == "maxmemory") {
        if (!requireOne()) return false;
        std::int64_t bytes = 0;
        if (!parseMemorySize(values[0], bytes) || bytes < 0) {
            error = "maxmemory expects a size such as 0, 536870912 or '512mb'";
            return false;
        }
        maxMemoryBytes = bytes;
        return true;
    }

    if (key == "maxmemory-policy") {
        if (!requireOne()) return false;
        if (!parseEvictionPolicy(values[0], evictionPolicy)) {
            error = "unknown maxmemory-policy '" + values[0] + "'";
            return false;
        }
        return true;
    }

    if (key == "maxmemory-samples") {
        if (!readInt(scratch, 1, 64)) return false;
        evictionSamples = static_cast<size_t>(scratch);
        return true;
    }

    if (key == "appendonly") {
        if (!requireOne()) return false;
        if (!parseBoolean(values[0], appendOnly)) {
            error = "appendonly expects yes or no";
            return false;
        }
        return true;
    }

    if (key == "appendfilename") { if (!requireOne()) return false; appendFilename = values[0]; return true; }

    if (key == "appendfsync") {
        if (!requireOne()) return false;
        if (!parseAofFsyncPolicy(values[0], appendFsync)) {
            error = "appendfsync expects always, everysec or no";
            return false;
        }
        return true;
    }

    if (key == "auto-aof-rewrite-percentage") {
        if (!readInt(scratch, 0, 10000)) return false;
        autoAofRewritePercentage = scratch;
        return true;
    }

    if (key == "auto-aof-rewrite-min-size") {
        if (!requireOne()) return false;
        std::int64_t bytes = 0;
        if (!parseMemorySize(values[0], bytes) || bytes < 0) {
            error = "auto-aof-rewrite-min-size expects a size such as '64mb'";
            return false;
        }
        autoAofRewriteMinSize = bytes;
        return true;
    }

    if (key == "dbfilename") { if (!requireOne()) return false; rdbFilename = values[0]; return true; }

    if (key == "save") {
        // "save" with no arguments disables snapshotting entirely, which is
        // how a deployment that relies solely on the AOF turns it off.
        if (values.empty() || (values.size() == 1 && values[0].empty())) {
            saveRules.clear();
            return true;
        }
        if (values.size() % 2 != 0) {
            error = "save expects pairs of <seconds> <changes>";
            return false;
        }
        std::vector<SaveRule> rules;
        for (size_t i = 0; i + 1 < values.size(); i += 2) {
            std::int64_t seconds = 0;
            std::int64_t changes = 0;
            if (!parseInt64(values[i], seconds) || !parseInt64(values[i + 1], changes) ||
                seconds <= 0 || changes < 0) {
                error = "save expects positive <seconds> and non-negative <changes>";
                return false;
            }
            rules.push_back(SaveRule{static_cast<int>(seconds), static_cast<int>(changes)});
        }
        saveRules = std::move(rules);
        return true;
    }

    if (key == "load-on-startup") {
        if (!requireOne()) return false;
        if (!parseBoolean(values[0], loadOnStartup)) {
            error = "load-on-startup expects yes or no";
            return false;
        }
        return true;
    }

    if (key == "replicaof" || key == "slaveof") {
        if (values.size() == 2 && equalsIgnoreCase(values[0], "no") && equalsIgnoreCase(values[1], "one")) {
            replicaOfHost.clear();
            replicaOfPort = 0;
            return true;
        }
        if (values.size() != 2) {
            error = "replicaof expects <host> <port>, or NO ONE";
            return false;
        }
        std::int64_t parsedPort = 0;
        if (!parseInt64(values[1], parsedPort) || parsedPort < 1 || parsedPort > 65535) {
            error = "replicaof expects a valid port";
            return false;
        }
        replicaOfHost = values[0];
        replicaOfPort = static_cast<int>(parsedPort);
        return true;
    }

    if (key == "replica-read-only" || key == "slave-read-only") {
        if (!requireOne()) return false;
        if (!parseBoolean(values[0], replicaReadOnly)) {
            error = "replica-read-only expects yes or no";
            return false;
        }
        return true;
    }

    if (key == "masterauth") {
        if (values.empty()) { masterAuth.clear(); return true; }
        if (!requireOne()) return false;
        masterAuth = values[0];
        return true;
    }

    if (key == "repl-timeout") {
        if (!readInt(scratch, 1, 86400)) return false;
        replTimeoutSeconds = static_cast<int>(scratch);
        return true;
    }

    if (key == "repl-backlog-size") {
        if (!requireOne()) return false;
        std::int64_t bytes = 0;
        if (!parseMemorySize(values[0], bytes) || bytes < 0) {
            error = "repl-backlog-size expects a size such as '1mb'";
            return false;
        }
        replBacklogSize = bytes;
        return true;
    }

    if (key == "cluster-enabled") {
        if (!requireOne()) return false;
        if (!parseBoolean(values[0], clusterEnabled)) {
            error = "cluster-enabled expects yes or no";
            return false;
        }
        return true;
    }

    if (key == "cluster-config-file")   { if (!requireOne()) return false; clusterConfigFile = values[0]; return true; }
    if (key == "cluster-announce-ip")   { if (!requireOne()) return false; clusterAnnounceIp = values[0]; return true; }
    if (key == "cluster-announce-port") {
        if (!readInt(scratch, 0, 65535)) return false;
        clusterAnnouncePort = static_cast<int>(scratch);
        return true;
    }

    if (key == "loglevel") {
        if (!requireOne()) return false;
        if (!parseLogLevel(values[0], logLevel)) {
            error = "loglevel expects debug, verbose, notice or warning";
            return false;
        }
        return true;
    }

    if (key == "logfile") {
        if (values.empty()) { logFile.clear(); return true; }
        if (!requireOne()) return false;
        logFile = values[0];
        return true;
    }

    if (key == "slowlog-log-slower-than") {
        if (!readInt(scratch, -1, 1000000000)) return false;
        slowLogThresholdMicros = scratch;
        return true;
    }

    if (key == "slowlog-max-len") {
        if (!readInt(scratch, 0, 1000000)) return false;
        slowLogMaxLength = static_cast<size_t>(scratch);
        return true;
    }

    error = "unknown directive '" + key + "'";
    return false;
}

bool ServerConfig::readDirective(const std::string& name, std::string& valueOut) const {
    const std::string key = toLower(name);

    if (key == "bind")        { valueOut = bindAddress; return true; }
    if (key == "port")        { valueOut = formatInt64(port); return true; }
    if (key == "tcp-backlog") { valueOut = formatInt64(tcpBacklog); return true; }
    if (key == "timeout")     { valueOut = formatInt64(clientTimeoutSeconds); return true; }
    if (key == "maxclients")  { valueOut = formatInt64(static_cast<std::int64_t>(maxClients)); return true; }
    if (key == "nodeid")      { valueOut = nodeId; return true; }
    if (key == "dir")         { valueOut = workingDir; return true; }
    if (key == "databases")   { valueOut = formatInt64(databaseCount); return true; }

    // The password is never echoed back; CONFIG GET requirepass on a real
    // Redis returns the value, but printing a secret into a client's terminal
    // and the slow log is a poor default.
    if (key == "requirepass") { valueOut = requirePass.empty() ? "" : "<hidden>"; return true; }

    if (key == "maxmemory")         { valueOut = formatInt64(maxMemoryBytes); return true; }
    if (key == "maxmemory-policy")  { valueOut = evictionPolicyName(evictionPolicy); return true; }
    if (key == "maxmemory-samples") { valueOut = formatInt64(static_cast<std::int64_t>(evictionSamples)); return true; }

    if (key == "appendonly")     { valueOut = booleanText(appendOnly); return true; }
    if (key == "appendfilename") { valueOut = appendFilename; return true; }
    if (key == "appendfsync")    { valueOut = aofFsyncPolicyName(appendFsync); return true; }
    if (key == "auto-aof-rewrite-percentage") { valueOut = formatInt64(autoAofRewritePercentage); return true; }
    if (key == "auto-aof-rewrite-min-size")   { valueOut = formatInt64(autoAofRewriteMinSize); return true; }

    if (key == "dbfilename")      { valueOut = rdbFilename; return true; }
    if (key == "load-on-startup") { valueOut = booleanText(loadOnStartup); return true; }

    if (key == "save") {
        std::string rendered;
        for (const auto& rule : saveRules) {
            if (!rendered.empty()) rendered += ' ';
            rendered += formatInt64(rule.seconds) + " " + formatInt64(rule.changes);
        }
        valueOut = rendered;
        return true;
    }

    if (key == "replicaof" || key == "slaveof") {
        valueOut = replicaOfHost.empty() ? "" : replicaOfHost + " " + formatInt64(replicaOfPort);
        return true;
    }
    if (key == "replica-read-only" || key == "slave-read-only") { valueOut = booleanText(replicaReadOnly); return true; }
    if (key == "masterauth")       { valueOut = masterAuth.empty() ? "" : "<hidden>"; return true; }
    if (key == "repl-timeout")     { valueOut = formatInt64(replTimeoutSeconds); return true; }
    if (key == "repl-backlog-size"){ valueOut = formatInt64(replBacklogSize); return true; }

    if (key == "cluster-enabled")      { valueOut = booleanText(clusterEnabled); return true; }
    if (key == "cluster-config-file")  { valueOut = clusterConfigFile; return true; }
    if (key == "cluster-announce-ip")  { valueOut = clusterAnnounceIp; return true; }
    if (key == "cluster-announce-port"){ valueOut = formatInt64(clusterAnnouncePort); return true; }

    if (key == "loglevel") { valueOut = logLevelName(logLevel); return true; }
    if (key == "logfile")  { valueOut = logFile; return true; }
    if (key == "slowlog-log-slower-than") { valueOut = formatInt64(slowLogThresholdMicros); return true; }
    if (key == "slowlog-max-len")         { valueOut = formatInt64(static_cast<std::int64_t>(slowLogMaxLength)); return true; }

    return false;
}

bool ServerConfig::loadFromFile(const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open config file: " + path;
        return false;
    }

    std::string line;
    int lineNumber = 0;
    std::vector<SaveRule> collectedSaveRules;
    bool sawSaveDirective = false;

    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        std::vector<std::string> tokens = tokenise(trimmed);
        if (tokens.empty()) continue;

        const std::string directive = tokens.front();
        const std::vector<std::string> values(tokens.begin() + 1, tokens.end());

        // Redis accumulates repeated `save` lines rather than letting the last
        // one win, so that a config file can list several rules.
        if (equalsIgnoreCase(directive, "save")) {
            sawSaveDirective = true;
            if (values.empty()) {
                collectedSaveRules.clear();
                continue;
            }
            if (values.size() != 2) {
                error = path + ":" + std::to_string(lineNumber) + ": save expects <seconds> <changes>";
                return false;
            }
            std::int64_t seconds = 0;
            std::int64_t changes = 0;
            if (!parseInt64(values[0], seconds) || !parseInt64(values[1], changes) ||
                seconds <= 0 || changes < 0) {
                error = path + ":" + std::to_string(lineNumber) +
                        ": save expects a positive interval and a non-negative change count";
                return false;
            }
            collectedSaveRules.push_back(SaveRule{static_cast<int>(seconds), static_cast<int>(changes)});
            continue;
        }

        std::string directiveError;
        if (!applyDirective(directive, values, directiveError)) {
            error = path + ":" + std::to_string(lineNumber) + ": " + directiveError;
            return false;
        }
    }

    if (sawSaveDirective) saveRules = std::move(collectedSaveRules);
    return true;
}

} // namespace miniredis
