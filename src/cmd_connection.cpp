// Connection-level commands: handshake, authentication, database selection.
#include "command_helpers.hpp"

#include "miniredis/cluster.hpp"
#include "miniredis/pubsub.hpp"
#include "miniredis/replication.hpp"

namespace miniredis {

using namespace detail;

namespace {

void cmdPing(CommandContext& ctx) {
    // Inside a subscription the reply has to be a push-shaped array, because
    // the connection is in message mode and a bare +PONG would be ambiguous
    // against a delivered message.
    if (ctx.client.isSubscribeMode()) {
        RespWriter writer = ctx.reply();
        writer.arrayHeader(2);
        writer.bulkString("pong");
        writer.bulkString(ctx.args.size() > 1 ? ctx.args[1] : "");
        return;
    }

    if (ctx.args.size() > 2) {
        ctx.reply().error(wrongArgsError(ctx.args[0]));
        return;
    }
    if (ctx.args.size() == 2) ctx.reply().bulkString(ctx.args[1]);
    else ctx.reply().simpleString("PONG");
}

void cmdEcho(CommandContext& ctx) {
    ctx.reply().bulkString(ctx.args[1]);
}

void cmdAuth(CommandContext& ctx) {
    if (ctx.server.config().requirePass.empty()) {
        ctx.reply().error("ERR Client sent AUTH, but no password is set");
        return;
    }

    // AUTH takes an optional username for compatibility; MiniRedis has a
    // single shared password, so only "default" is accepted.
    const Bytes& password = ctx.args.size() == 3 ? ctx.args[2] : ctx.args[1];
    if (ctx.args.size() == 3 && !equalsIgnoreCase(ctx.args[1], "default")) {
        ctx.reply().error("WRONGPASS invalid username-password pair");
        return;
    }

    if (password != ctx.server.config().requirePass) {
        ctx.client.authenticated = false;
        ctx.reply().error("WRONGPASS invalid username-password pair");
        return;
    }

    ctx.client.authenticated = true;
    ctx.reply().ok();
}

void cmdSelect(CommandContext& ctx) {
    std::int64_t index = 0;
    if (!readInt(ctx, ctx.args[1], index)) return;

    if (index < 0 || index >= ctx.server.databaseCount()) {
        ctx.reply().error("ERR DB index is out of range");
        return;
    }
    // Cluster mode has a single database, because a multi-database keyspace
    // has no meaning once keys are distributed by hash slot.
    if (ctx.server.cluster().isEnabled() && index != 0) {
        ctx.reply().error("ERR SELECT is not allowed in cluster mode");
        return;
    }

    ctx.client.dbIndex = static_cast<int>(index);
    ctx.reply().ok();
}

void cmdSwapDb(CommandContext& ctx) {
    std::int64_t first = 0;
    std::int64_t second = 0;
    if (!readInt(ctx, ctx.args[1], first)) return;
    if (!readInt(ctx, ctx.args[2], second)) return;

    if (first < 0 || second < 0 || first >= ctx.server.databaseCount() ||
        second >= ctx.server.databaseCount()) {
        ctx.reply().error("ERR DB index is out of range");
        return;
    }

    if (first != second) {
        // Swapping is how a rebuilt dataset is promoted atomically: build it
        // in db 1, then SWAPDB 0 1 and every client sees the new data at once.
        ctx.server.swapDatabases(static_cast<int>(first), static_cast<int>(second));
        ctx.server.markDirty();
    }
    ctx.reply().ok();
}

void cmdQuit(CommandContext& ctx) {
    ctx.reply().ok();
    ctx.client.closeAfterReply = true;
}

void cmdHello(CommandContext& ctx) {
    if (ctx.args.size() > 1) {
        std::int64_t version = 0;
        if (!parseInt64(ctx.args[1], version) || version < 2 || version > 3) {
            ctx.reply().error("NOPROTO unsupported protocol version");
            return;
        }
        // RESP3 is advertised as unsupported rather than accepted-and-ignored,
        // so a client does not enable push messages the server cannot send.
        if (version == 3) {
            ctx.reply().error("NOPROTO unsupported protocol version, MiniRedis speaks RESP2");
            return;
        }
    }

    RespWriter writer = ctx.reply();
    writer.arrayHeader(14);
    writer.bulkString("server");   writer.bulkString("miniredis");
    writer.bulkString("version");  writer.bulkString("1.0.0");
    writer.bulkString("proto");    writer.integer(2);
    writer.bulkString("id");       writer.integer(static_cast<std::int64_t>(ctx.client.id));
    writer.bulkString("mode");     writer.bulkString(ctx.server.cluster().isEnabled() ? "cluster" : "standalone");
    writer.bulkString("role");     writer.bulkString(ctx.server.replication().isReplica() ? "replica" : "master");
    writer.bulkString("modules");  writer.arrayHeader(0);
}

void cmdClient(CommandContext& ctx) {
    const std::string subcommand = toUpper(ctx.args[1]);

    if (subcommand == "SETNAME" && ctx.args.size() == 3) {
        // A name with spaces would break the single-line CLIENT LIST format.
        if (ctx.args[2].find(' ') != std::string::npos || ctx.args[2].find('\n') != std::string::npos) {
            ctx.reply().error("ERR Client names cannot contain spaces or newlines");
            return;
        }
        ctx.client.name = ctx.args[2];
        ctx.reply().ok();
        return;
    }

    if (subcommand == "GETNAME" && ctx.args.size() == 2) {
        if (ctx.client.name.empty()) ctx.reply().nullBulkString();
        else ctx.reply().bulkString(ctx.client.name);
        return;
    }

    if (subcommand == "ID" && ctx.args.size() == 2) {
        ctx.reply().integer(static_cast<std::int64_t>(ctx.client.id));
        return;
    }

    if (subcommand == "LIST" && ctx.args.size() == 2) {
        std::string listing;
        for (const auto& [id, client] : ctx.server.clients()) {
            listing += "id=" + formatInt64(static_cast<std::int64_t>(id));
            listing += " addr=" + client->peerAddress;
            listing += " name=" + client->name;
            listing += " db=" + formatInt64(client->dbIndex);
            listing += " age=" + formatInt64((ctx.now - client->createdAt) / 1000);
            listing += " idle=" + formatInt64((ctx.now - client->lastInteraction) / 1000);
            listing += " sub=" + formatInt64(static_cast<std::int64_t>(client->subscribedChannels.size()));
            listing += " psub=" + formatInt64(static_cast<std::int64_t>(client->subscribedPatterns.size()));
            listing += " multi=" + formatInt64(client->inMulti ? static_cast<std::int64_t>(client->multiQueue.size()) : -1);
            listing += " cmd=" + formatInt64(static_cast<std::int64_t>(client->commandsProcessed));
            listing += std::string(" flags=") +
                       (client->role == ClientRole::Replica ? "S"
                        : client->role == ClientRole::Monitor ? "O" : "N");
            listing += '\n';
        }
        ctx.reply().bulkString(listing);
        return;
    }

    if (subcommand == "KILL" && ctx.args.size() == 3) {
        std::int64_t killed = 0;
        for (const auto& [id, client] : ctx.server.clients()) {
            if (client->peerAddress != ctx.args[2]) continue;
            // A client may kill itself; the reply still has to be delivered
            // first, so closeAfterReply is used rather than an immediate close.
            if (client->id == ctx.client.id) ctx.client.closeAfterReply = true;
            else client->shouldClose = true;
            ++killed;
        }
        if (killed == 0) ctx.reply().error("ERR No such client address");
        else ctx.reply().ok();
        return;
    }

    if (subcommand == "HELP") {
        const std::vector<std::string> help{
            "CLIENT ID                -- this connection's id",
            "CLIENT GETNAME           -- this connection's name",
            "CLIENT SETNAME <name>    -- name this connection",
            "CLIENT LIST              -- describe every connection",
            "CLIENT KILL <addr:port>  -- disconnect a client",
        };
        ctx.reply().stringArray(help);
        return;
    }

    ctx.reply().error("ERR Unknown CLIENT subcommand '" + ctx.args[1] + "'. Try CLIENT HELP.");
}

void cmdReset(CommandContext& ctx) {
    // RESET returns a connection to its just-opened state, which is what a
    // connection pool wants before handing a socket to another user.
    ctx.server.pubsub().unsubscribeAll(ctx.client);
    ctx.client.inMulti = false;
    ctx.client.multiQueue.clear();
    ctx.client.multiQueueError = false;
    ctx.client.watchedKeys.clear();
    ctx.client.watchInvalidated = false;
    ctx.client.dbIndex = 0;
    ctx.client.name.clear();
    ctx.client.role = ClientRole::Normal;
    ctx.client.authenticated = ctx.server.config().requirePass.empty();
    ctx.reply().simpleString("RESET");
}

} // namespace

void registerConnectionCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"PING", cmdPing, -1, kReadOnly | kFast | kPubSub | kNoAuth, 0, 0, 0});
    table.add({"ECHO", cmdEcho, 2, kReadOnly | kFast, 0, 0, 0});
    table.add({"AUTH", cmdAuth, -2, kFast | kNoAuth | kNoMulti, 0, 0, 0});
    table.add({"SELECT", cmdSelect, 2, kFast, 0, 0, 0});
    table.add({"SWAPDB", cmdSwapDb, 3, kWrite | kFast, 0, 0, 0});
    table.add({"QUIT", cmdQuit, 1, kFast | kPubSub | kNoAuth | kNoMulti, 0, 0, 0});
    table.add({"HELLO", cmdHello, -1, kFast | kNoAuth, 0, 0, 0});
    table.add({"CLIENT", cmdClient, -2, kAdmin, 0, 0, 0});
    table.add({"RESET", cmdReset, 1, kFast | kNoAuth | kNoMulti | kPubSub, 0, 0, 0});
}

} // namespace miniredis
