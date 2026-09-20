// Replication commands: REPLICAOF, PSYNC, REPLCONF, WAIT, FAILOVER.
#include "command_helpers.hpp"

#include "miniredis/replication.hpp"

namespace miniredis {

using namespace detail;

namespace {

void cmdReplicaOf(CommandContext& ctx) {
    if (equalsIgnoreCase(ctx.args[1], "NO") && equalsIgnoreCase(ctx.args[2], "ONE")) {
        if (ctx.server.replication().isMaster()) {
            ctx.reply().ok();
            return;
        }
        // Promotion keeps the dataset as it stands. The replica's current
        // contents become the new timeline's starting point.
        ctx.server.replication().promoteToMaster();
        ctx.server.config().replicaOfHost.clear();
        ctx.server.config().replicaOfPort = 0;
        ctx.reply().ok();
        return;
    }

    std::int64_t port = 0;
    if (!parseInt64(ctx.args[2], port) || port < 1 || port > 65535) {
        ctx.reply().error("ERR Invalid master port");
        return;
    }

    // Refusing to replicate from yourself is worth checking: the resulting
    // loop is confusing to diagnose from the logs.
    if ((ctx.args[1] == "127.0.0.1" || ctx.args[1] == "localhost" ||
         ctx.args[1] == ctx.server.config().bindAddress) &&
        static_cast<int>(port) == ctx.server.config().port) {
        ctx.reply().error("ERR Can't replicate from this server's own address");
        return;
    }

    ctx.server.config().replicaOfHost = ctx.args[1];
    ctx.server.config().replicaOfPort = static_cast<int>(port);
    ctx.server.replication().replicaOf(ctx.args[1], static_cast<int>(port));
    ctx.reply().ok();
}

void cmdPsync(CommandContext& ctx) {
    // PSYNC <replid> <offset>. "? -1" means "I have no history, send me
    // everything".
    std::int64_t requestedOffset = -1;
    if (ctx.args.size() >= 3) {
        parseInt64(ctx.args[2], requestedOffset);
    }
    const std::string requestedId = ctx.args.size() >= 2 ? ctx.args[1] : "?";

    ctx.server.replication().handlePsync(ctx.client, requestedId, requestedOffset);
}

void cmdReplConf(CommandContext& ctx) {
    for (size_t i = 1; i + 1 < ctx.args.size(); i += 2) {
        const Bytes& option = ctx.args[i];

        if (equalsIgnoreCase(option, "listening-port")) {
            // Remembering the replica's own listening port lets INFO report an
            // address another node could actually connect to, rather than the
            // ephemeral source port of this connection.
            ctx.client.replicaListeningPort = ctx.args[i + 1];
        } else if (equalsIgnoreCase(option, "ACK")) {
            std::int64_t offset = 0;
            if (parseInt64(ctx.args[i + 1], offset)) {
                ctx.client.replicaAckOffset = offset;
                ctx.client.replicaAckTime = ctx.now;
            }
            // An ACK is a report, not a request; replying would put an
            // unexpected message into the replication stream.
            return;
        } else if (equalsIgnoreCase(option, "GETACK")) {
            // Only a master sends this; a master receiving it does nothing.
            return;
        }
        // Unknown options are accepted silently, so a newer replica speaking a
        // slightly richer handshake still connects.
    }

    ctx.reply().ok();
}

void cmdWait(CommandContext& ctx) {
    std::int64_t requiredReplicas = 0;
    std::int64_t timeoutMs = 0;
    if (!readInt(ctx, ctx.args[1], requiredReplicas)) return;
    if (!readInt(ctx, ctx.args[2], timeoutMs)) return;

    if (ctx.server.replication().isReplica()) {
        ctx.reply().error("ERR WAIT cannot be used on a replica");
        return;
    }

    // Replication is asynchronous, so a client that needs to know its write
    // reached N replicas has to ask. WAIT is that question.
    //
    // It is answered without blocking: blocking the single server thread until
    // replicas acknowledge would stall every other client, which is a far
    // worse outcome than an immediate, possibly conservative, answer. The
    // client can call WAIT again.
    const std::int64_t targetOffset = ctx.server.replication().masterOffset();
    ctx.server.replication().requestAcksFromReplicas();

    const size_t acknowledging = ctx.server.replication().replicasAcknowledging(targetOffset);
    (void)requiredReplicas;
    (void)timeoutMs;
    ctx.reply().integer(static_cast<std::int64_t>(acknowledging));
}

void cmdFailover(CommandContext& ctx) {
    // Coordinated failover needs a consensus protocol -- Redis Sentinel, or
    // the cluster bus's voting. Neither is implemented here, and a failover
    // without agreement produces two masters and a split brain, so this says
    // so rather than doing something unsafe.
    if (ctx.args.size() >= 2 && equalsIgnoreCase(ctx.args[1], "ABORT")) {
        ctx.reply().error("ERR No failover in progress");
        return;
    }
    ctx.reply().error(
        "ERR FAILOVER requires a coordinator to avoid split brain, which MiniRedis does not "
        "implement. Promote a replica explicitly with REPLICAOF NO ONE after confirming the "
        "old master is down.");
}

} // namespace

void registerReplicationCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"REPLICAOF", cmdReplicaOf, 3, kAdmin | kNoMulti, 0, 0, 0});
    table.add({"SLAVEOF", cmdReplicaOf, 3, kAdmin | kNoMulti, 0, 0, 0});
    table.add({"PSYNC", cmdPsync, -1, kAdmin | kNoMulti | kNoAuth, 0, 0, 0});
    table.add({"SYNC", cmdPsync, -1, kAdmin | kNoMulti | kNoAuth, 0, 0, 0});
    table.add({"REPLCONF", cmdReplConf, -1, kAdmin | kNoMulti | kNoAuth | kLoading, 0, 0, 0});
    table.add({"WAIT", cmdWait, 3, kNoMulti, 0, 0, 0});
    table.add({"FAILOVER", cmdFailover, -1, kAdmin | kNoMulti, 0, 0, 0});
}

} // namespace miniredis
