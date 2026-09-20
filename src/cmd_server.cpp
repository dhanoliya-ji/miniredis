// Server administration: INFO, CONFIG, FLUSH*, SLOWLOG, MONITOR, DEBUG.
#include "command_helpers.hpp"

#include "miniredis/aof.hpp"
#include "miniredis/cluster.hpp"
#include "miniredis/replication.hpp"

#include <algorithm>

namespace miniredis {

using namespace detail;

namespace {

void cmdInfo(CommandContext& ctx) {
    const std::string section = ctx.args.size() > 1 ? ctx.args[1] : std::string("default");
    ctx.reply().bulkString(ctx.server.infoReport(section));
}

void cmdConfig(CommandContext& ctx) {
    const std::string subcommand = toUpper(ctx.args[1]);

    if (subcommand == "GET") {
        if (ctx.args.size() < 3) {
            ctx.reply().error(wrongArgsError("config|get"));
            return;
        }

        // Every requested pattern is matched against every directive name, and
        // duplicates are collapsed so "CONFIG GET max* maxmemory" does not
        // report maxmemory twice.
        std::vector<std::pair<std::string, std::string>> matches;
        for (const auto& name : ServerConfig::directiveNames()) {
            bool wanted = false;
            for (size_t i = 2; i < ctx.args.size(); ++i) {
                if (globMatch(ctx.args[i], name)) {
                    wanted = true;
                    break;
                }
            }
            if (!wanted) continue;

            std::string value;
            if (ctx.server.config().readDirective(name, value)) {
                matches.emplace_back(name, value);
            }
        }

        RespWriter writer = ctx.reply();
        writer.arrayHeader(static_cast<std::int64_t>(matches.size()) * 2);
        for (const auto& [name, value] : matches) {
            writer.bulkString(name);
            writer.bulkString(value);
        }
        return;
    }

    if (subcommand == "SET") {
        if (ctx.args.size() < 4) {
            ctx.reply().error(wrongArgsError("config|set"));
            return;
        }

        const std::vector<std::string> values(ctx.args.begin() + 3, ctx.args.end());
        std::string error;
        if (!ctx.server.config().applyDirective(ctx.args[2], values, error)) {
            ctx.reply().error("ERR CONFIG SET failed - " + error);
            return;
        }

        // A few directives need more than a field assignment to take effect.
        if (equalsIgnoreCase(ctx.args[2], "appendonly")) {
            if (ctx.server.config().appendOnly) {
                std::string aofError;
                if (!ctx.server.aof().enable(aofError)) {
                    ctx.reply().error("ERR " + aofError);
                    return;
                }
                // Starting the log mid-flight would otherwise record only
                // future writes, so the existing dataset is written first.
                if (!ctx.server.aof().rewrite(aofError)) {
                    ctx.reply().error("ERR " + aofError);
                    return;
                }
            } else {
                ctx.server.aof().disable();
            }
        }

        ctx.reply().ok();
        return;
    }

    if (subcommand == "RESETSTAT") {
        ctx.server.stats() = ServerStats{};
        ctx.reply().ok();
        return;
    }

    if (subcommand == "REWRITE") {
        ctx.reply().error("ERR CONFIG REWRITE is not supported; MiniRedis does not rewrite its config file");
        return;
    }

    if (subcommand == "HELP") {
        const std::vector<std::string> help{
            "CONFIG GET <pattern...>      -- read directives matching a glob",
            "CONFIG SET <name> <value...> -- change a directive at runtime",
            "CONFIG RESETSTAT             -- zero the INFO stats counters",
        };
        ctx.reply().stringArray(help);
        return;
    }

    ctx.reply().error("ERR Unknown CONFIG subcommand '" + ctx.args[1] + "'. Try CONFIG HELP.");
}

void cmdFlushDb(CommandContext& ctx) {
    ctx.server.currentDb(ctx.client).clear();
    ctx.server.markDirty();
    ctx.reply().ok();
}

void cmdFlushAll(CommandContext& ctx) {
    ctx.server.flushAllDatabases();
    ctx.server.markDirty();
    ctx.reply().ok();
}

void cmdCommand(CommandContext& ctx) {
    if (ctx.args.size() == 1) {
        // The full table: one entry per command, each a six-element array in
        // the shape real clients expect.
        RespWriter writer = ctx.reply();
        writer.arrayHeader(static_cast<std::int64_t>(ctx.server.commands().size()));
        for (const auto& [name, spec] : ctx.server.commands().all()) {
            writer.arrayHeader(6);
            writer.bulkString(name);
            writer.integer(spec.arity);

            std::vector<std::string> flags;
            if (spec.flags & cmdflag::kWrite)    flags.push_back("write");
            if (spec.flags & cmdflag::kReadOnly) flags.push_back("readonly");
            if (spec.flags & cmdflag::kAdmin)    flags.push_back("admin");
            if (spec.flags & cmdflag::kDenyOom)  flags.push_back("denyoom");
            if (spec.flags & cmdflag::kFast)     flags.push_back("fast");
            if (spec.flags & cmdflag::kPubSub)   flags.push_back("pubsub");
            if (spec.flags & cmdflag::kNoMulti)  flags.push_back("no-multi");
            writer.stringArray(flags);

            writer.integer(spec.firstKey);
            writer.integer(spec.lastKey);
            writer.integer(spec.keyStep);
        }
        return;
    }

    const std::string subcommand = toUpper(ctx.args[1]);

    if (subcommand == "COUNT") {
        ctx.reply().integer(static_cast<std::int64_t>(ctx.server.commands().size()));
        return;
    }

    if (subcommand == "DOCS") {
        // A real DOCS reply carries per-argument metadata. Rather than fake
        // it, MiniRedis returns an empty map, which clients treat as "no docs".
        ctx.reply().arrayHeader(0);
        return;
    }

    if (subcommand == "INFO") {
        RespWriter writer = ctx.reply();
        writer.arrayHeader(static_cast<std::int64_t>(ctx.args.size() - 2));
        for (size_t i = 2; i < ctx.args.size(); ++i) {
            const CommandSpec* spec = ctx.server.commands().find(ctx.args[i]);
            if (spec == nullptr) {
                writer.nullArray();
                continue;
            }
            writer.arrayHeader(6);
            writer.bulkString(toLower(spec->name));
            writer.integer(spec->arity);
            writer.arrayHeader(0);
            writer.integer(spec->firstKey);
            writer.integer(spec->lastKey);
            writer.integer(spec->keyStep);
        }
        return;
    }

    ctx.reply().error("ERR Unknown COMMAND subcommand '" + ctx.args[1] + "'");
}

void cmdSlowLog(CommandContext& ctx) {
    const std::string subcommand = toUpper(ctx.args[1]);

    if (subcommand == "GET") {
        std::int64_t requested = 10;
        if (ctx.args.size() > 2 && !readInt(ctx, ctx.args[2], requested)) return;

        const auto& entries = ctx.server.slowLog();
        const std::int64_t count = (requested < 0)
            ? static_cast<std::int64_t>(entries.size())
            : std::min<std::int64_t>(requested, static_cast<std::int64_t>(entries.size()));

        RespWriter writer = ctx.reply();
        writer.arrayHeader(count);

        std::int64_t emitted = 0;
        for (const auto& entry : entries) {
            if (emitted++ >= count) break;
            writer.arrayHeader(6);
            writer.integer(static_cast<std::int64_t>(entry.id));
            writer.integer(entry.timestamp / 1000);
            writer.integer(entry.durationMicros);
            writer.stringArray(entry.args);
            writer.bulkString(entry.clientAddress);
            writer.bulkString(entry.clientName);
        }
        return;
    }

    if (subcommand == "LEN") {
        ctx.reply().integer(static_cast<std::int64_t>(ctx.server.slowLog().size()));
        return;
    }

    if (subcommand == "RESET") {
        ctx.server.resetSlowLog();
        ctx.reply().ok();
        return;
    }

    if (subcommand == "HELP") {
        const std::vector<std::string> help{
            "SLOWLOG GET [count] -- the slowest recent commands, newest first",
            "SLOWLOG LEN         -- how many entries are recorded",
            "SLOWLOG RESET       -- clear the log",
        };
        ctx.reply().stringArray(help);
        return;
    }

    ctx.reply().error("ERR Unknown SLOWLOG subcommand '" + ctx.args[1] + "'. Try SLOWLOG HELP.");
}

void cmdMonitor(CommandContext& ctx) {
    // A monitoring client stops being a normal client: from here on it only
    // receives the command feed.
    ctx.client.role = ClientRole::Monitor;
    ctx.reply().ok();
}

void cmdTime(CommandContext& ctx) {
    const Millis now = ctx.now;
    RespWriter writer = ctx.reply();
    writer.arrayHeader(2);
    writer.bulkString(formatInt64(now / 1000));
    writer.bulkString(formatInt64((now % 1000) * 1000)); // microseconds
}

void cmdShutdown(CommandContext& ctx) {
    bool save = !ctx.server.config().saveRules.empty();
    for (size_t i = 1; i < ctx.args.size(); ++i) {
        if (equalsIgnoreCase(ctx.args[i], "NOSAVE")) save = false;
        else if (equalsIgnoreCase(ctx.args[i], "SAVE")) save = true;
        else {
            ctx.reply().error(err::kSyntax);
            return;
        }
    }

    if (save) {
        std::string error;
        if (!ctx.server.saveSnapshot(error)) {
            // Refusing to exit is the right call: shutting down after a failed
            // save would discard data the operator asked to keep.
            ctx.reply().error("ERR Errors trying to SHUTDOWN: " + error);
            return;
        }
    }

    ctx.server.log(LogLevel::Notice, "received SHUTDOWN, exiting");
    ctx.server.stop();
    // No reply: a successful SHUTDOWN simply closes the connection.
}

void cmdDebug(CommandContext& ctx) {
    const std::string subcommand = toUpper(ctx.args[1]);

    if (subcommand == "SLEEP" && ctx.args.size() == 3) {
        // Deliberately blocks the whole server. It exists to demonstrate the
        // cost of the single-threaded model and to test client timeouts.
        double seconds = 0;
        if (!readDouble(ctx, ctx.args[2], seconds)) return;
        const std::int64_t until = monotonicMicros() + static_cast<std::int64_t>(seconds * 1e6);
        while (monotonicMicros() < until) { /* spin */ }
        ctx.reply().ok();
        return;
    }

    if (subcommand == "JMAP" || subcommand == "OBJECT") {
        if (ctx.args.size() != 3) {
            ctx.reply().error(wrongArgsError("debug|object"));
            return;
        }
        Object* value = ctx.server.currentDb(ctx.client).lookupRead(ctx.args[2], ctx.now);
        if (value == nullptr) {
            ctx.reply().error(err::kNoSuchKey);
            return;
        }
        ctx.reply().simpleString("type:" + std::string(value->typeName()) +
                                 " elements:" + formatInt64(static_cast<std::int64_t>(value->length())) +
                                 " serialized_bytes:" + formatInt64(static_cast<std::int64_t>(value->memoryUsage())));
        return;
    }

    if (subcommand == "SET-ACTIVE-EXPIRE" && ctx.args.size() == 3) {
        ctx.reply().ok();
        return;
    }

    if (subcommand == "HELP") {
        const std::vector<std::string> help{
            "DEBUG SLEEP <seconds>  -- block the server, to show what one slow command costs",
            "DEBUG OBJECT <key>     -- type, element count and estimated size",
        };
        ctx.reply().stringArray(help);
        return;
    }

    ctx.reply().error("ERR Unknown DEBUG subcommand '" + ctx.args[1] + "'. Try DEBUG HELP.");
}

void cmdMemory(CommandContext& ctx) {
    const std::string subcommand = toUpper(ctx.args[1]);

    if (subcommand == "USAGE" && ctx.args.size() >= 3) {
        Object* value = ctx.server.currentDb(ctx.client).lookupRead(ctx.args[2], ctx.now);
        if (value == nullptr) ctx.reply().nullBulkString();
        else ctx.reply().integer(static_cast<std::int64_t>(value->memoryUsage() + ctx.args[2].size()));
        return;
    }

    if (subcommand == "DOCTOR") {
        const std::int64_t used = ctx.server.usedMemory();
        const std::int64_t limit = ctx.server.config().maxMemoryBytes;

        std::string advice = "Used memory: " + formatMemorySize(used) + ".\n";
        if (limit <= 0) {
            advice += "maxmemory is unset, so MiniRedis will never evict and will grow until the\n"
                      "machine runs out. Set maxmemory and a maxmemory-policy before relying on\n"
                      "this as a cache.\n";
        } else {
            const int percent = static_cast<int>((used * 100) / limit);
            advice += "maxmemory is " + formatMemorySize(limit) + " (" + formatInt64(percent) + "% used)";
            advice += " with policy " + std::string(evictionPolicyName(ctx.server.config().evictionPolicy)) + ".\n";
            if (ctx.server.config().evictionPolicy == EvictionPolicy::NoEviction) {
                advice += "Policy is noeviction, so writes will be refused with OOM rather than\n"
                          "making room. That is correct for a datastore and wrong for a cache.\n";
            }
            if (percent > 90) advice += "Memory use is above 90% of the limit.\n";
        }
        advice += "Evicted keys so far: " + formatInt64(static_cast<std::int64_t>(ctx.server.stats().evictedKeys)) + ".\n";
        ctx.reply().bulkString(advice);
        return;
    }

    if (subcommand == "STATS") {
        RespWriter writer = ctx.reply();
        writer.arrayHeader(8);
        writer.bulkString("used_memory");   writer.integer(ctx.server.usedMemory());
        writer.bulkString("maxmemory");     writer.integer(ctx.server.config().maxMemoryBytes);
        writer.bulkString("evicted_keys");  writer.integer(static_cast<std::int64_t>(ctx.server.stats().evictedKeys));
        writer.bulkString("expired_keys");  writer.integer(static_cast<std::int64_t>(ctx.server.stats().expiredKeys));
        return;
    }

    ctx.reply().error("ERR Unknown MEMORY subcommand '" + ctx.args[1] + "'");
}

void cmdLolwut(CommandContext& ctx) {
    (void)ctx.args;
    ctx.reply().bulkString(
        "MiniRedis 1.0.0\n"
        "A Redis-style in-memory database built from scratch in C++20.\n"
        "Every layer here is hand-written: the event loop, the RESP codec, the\n"
        "data types, expiry, eviction, the AOF, the snapshot format, replication\n"
        "and the cluster slot map.\n");
}

} // namespace

void registerServerCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"INFO", cmdInfo, -1, kReadOnly | kLoading, 0, 0, 0});
    table.add({"CONFIG", cmdConfig, -2, kAdmin | kNoMulti, 0, 0, 0});
    table.add({"DBSIZE", [](CommandContext& c) {
        c.reply().integer(static_cast<std::int64_t>(c.server.currentDb(c.client).size()));
    }, 1, kReadOnly | kFast, 0, 0, 0});
    table.add({"FLUSHDB", cmdFlushDb, -1, kWrite, 0, 0, 0});
    table.add({"FLUSHALL", cmdFlushAll, -1, kWrite, 0, 0, 0});
    table.add({"COMMAND", cmdCommand, -1, kReadOnly | kLoading, 0, 0, 0});
    table.add({"SLOWLOG", cmdSlowLog, -2, kAdmin, 0, 0, 0});
    table.add({"MONITOR", cmdMonitor, 1, kAdmin | kNoMulti, 0, 0, 0});
    table.add({"TIME", cmdTime, 1, kReadOnly | kFast, 0, 0, 0});
    table.add({"SHUTDOWN", cmdShutdown, -1, kAdmin | kNoMulti, 0, 0, 0});
    table.add({"DEBUG", cmdDebug, -2, kAdmin, 0, 0, 0});
    table.add({"MEMORY", cmdMemory, -2, kReadOnly, 0, 0, 0});
    table.add({"LOLWUT", cmdLolwut, -1, kReadOnly | kFast, 0, 0, 0});
}

} // namespace miniredis
