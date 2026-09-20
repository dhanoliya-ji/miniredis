// CLUSTER subcommands.
#include "command_helpers.hpp"

#include "miniredis/cluster.hpp"

namespace miniredis {

using namespace detail;

namespace {

void cmdCluster(CommandContext& ctx) {
    ClusterState& cluster = ctx.server.cluster();
    const std::string subcommand = toUpper(ctx.args[1]);

    // KEYSLOT works whether or not cluster mode is on, because it is useful
    // for reasoning about how a keyspace *would* shard before you commit to it.
    if (subcommand == "KEYSLOT" && ctx.args.size() == 3) {
        ctx.reply().integer(keyHashSlot(ctx.args[2]));
        return;
    }

    if (subcommand == "HELP") {
        const std::vector<std::string> help{
            "CLUSTER INFO                       -- cluster state and slot coverage",
            "CLUSTER MYID                       -- this node's id",
            "CLUSTER NODES                      -- one line per known node",
            "CLUSTER SLOTS                      -- slot ranges and their owners",
            "CLUSTER SHARDS                     -- slot ranges grouped by shard",
            "CLUSTER KEYSLOT <key>              -- which slot a key hashes to",
            "CLUSTER COUNTKEYSINSLOT <slot>     -- local keys in a slot",
            "CLUSTER GETKEYSINSLOT <slot> <n>   -- up to n local keys in a slot",
            "CLUSTER MEET <id> <host> <port>    -- add a node to this node's view",
            "CLUSTER ADDSLOTSRANGE <id> <a> <b> -- assign a slot range to a node",
            "CLUSTER FORGET <id>                -- drop a node from this node's view",
        };
        ctx.reply().stringArray(help);
        return;
    }

    if (!cluster.isEnabled()) {
        ctx.reply().error("ERR This instance has cluster support disabled; start it with cluster-enabled yes");
        return;
    }

    if (subcommand == "INFO") {
        ctx.reply().bulkString(cluster.describeInfo());
        return;
    }

    if (subcommand == "MYID") {
        const ClusterNode* self = cluster.self();
        ctx.reply().bulkString(self == nullptr ? "" : self->id);
        return;
    }

    if (subcommand == "NODES") {
        ctx.reply().bulkString(cluster.describeNodes());
        return;
    }

    if (subcommand == "SLOTS") {
        // Each entry is [startSlot, endSlot, [host, port, id], ...replicas].
        // This is what a cluster-aware client reads once on connect to build
        // its routing table, so it never pays for a MOVED again.
        std::vector<std::pair<std::pair<int, int>, const ClusterNode*>> ranges;
        for (const auto& node : cluster.nodes()) {
            if (!node.isMaster) continue;
            for (const auto& range : node.slotRanges) ranges.emplace_back(range, &node);
        }

        RespWriter writer = ctx.reply();
        writer.arrayHeader(static_cast<std::int64_t>(ranges.size()));
        for (const auto& [range, node] : ranges) {
            writer.arrayHeader(3);
            writer.integer(range.first);
            writer.integer(range.second);
            writer.arrayHeader(3);
            writer.bulkString(node->host);
            writer.integer(node->port);
            writer.bulkString(node->id);
        }
        return;
    }

    if (subcommand == "SHARDS") {
        RespWriter writer = ctx.reply();
        std::vector<const ClusterNode*> masters;
        for (const auto& node : cluster.nodes()) {
            if (node.isMaster) masters.push_back(&node);
        }

        writer.arrayHeader(static_cast<std::int64_t>(masters.size()));
        for (const ClusterNode* master : masters) {
            writer.arrayHeader(4);
            writer.bulkString("slots");
            writer.arrayHeader(static_cast<std::int64_t>(master->slotRanges.size()) * 2);
            for (const auto& [start, end] : master->slotRanges) {
                writer.integer(start);
                writer.integer(end);
            }
            writer.bulkString("nodes");
            writer.arrayHeader(1);
            writer.arrayHeader(6);
            writer.bulkString("id");       writer.bulkString(master->id);
            writer.bulkString("endpoint"); writer.bulkString(master->host);
            writer.bulkString("port");     writer.integer(master->port);
        }
        return;
    }

    if (subcommand == "COUNTKEYSINSLOT" && ctx.args.size() == 3) {
        std::int64_t slot = 0;
        if (!readInt(ctx, ctx.args[2], slot)) return;
        if (slot < 0 || slot >= kClusterSlots) {
            ctx.reply().error("ERR Invalid slot");
            return;
        }

        // Counting means walking the local keyspace, because keys are stored
        // by name, not grouped by slot. Redis keeps a sorted slot-to-key index
        // to make this O(log n); that index is the main thing MiniRedis's
        // cluster mode leaves out, and it only matters during resharding.
        std::int64_t count = 0;
        ctx.server.currentDb(ctx.client).forEach([&](const Bytes& key, const KeyEntry&) {
            if (keyHashSlot(key) == static_cast<int>(slot)) ++count;
        });
        ctx.reply().integer(count);
        return;
    }

    if (subcommand == "GETKEYSINSLOT" && ctx.args.size() == 4) {
        std::int64_t slot = 0;
        std::int64_t limit = 0;
        if (!readInt(ctx, ctx.args[2], slot)) return;
        if (!readInt(ctx, ctx.args[3], limit)) return;
        if (slot < 0 || slot >= kClusterSlots || limit < 0) {
            ctx.reply().error("ERR Invalid slot or count");
            return;
        }

        std::vector<std::string> keys;
        ctx.server.currentDb(ctx.client).forEach([&](const Bytes& key, const KeyEntry&) {
            if (static_cast<std::int64_t>(keys.size()) >= limit) return;
            if (keyHashSlot(key) == static_cast<int>(slot)) keys.push_back(key);
        });
        ctx.reply().stringArray(keys);
        return;
    }

    if (subcommand == "MEET" && ctx.args.size() == 5) {
        std::int64_t port = 0;
        if (!readInt(ctx, ctx.args[4], port)) return;

        ClusterNode node;
        node.id = ctx.args[2];
        node.host = ctx.args[3];
        node.port = static_cast<int>(port);
        node.isMaster = true;
        cluster.addNode(std::move(node));

        std::string error;
        cluster.saveConfig(ctx.server.config().resolvePath(ctx.server.config().clusterConfigFile), error);
        ctx.reply().ok();
        return;
    }

    if (subcommand == "ADDSLOTSRANGE" && ctx.args.size() == 5) {
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!readInt(ctx, ctx.args[3], start)) return;
        if (!readInt(ctx, ctx.args[4], end)) return;

        std::string error;
        if (!cluster.assignSlots(ctx.args[2], static_cast<int>(start), static_cast<int>(end), error)) {
            ctx.reply().error(error);
            return;
        }
        cluster.saveConfig(ctx.server.config().resolvePath(ctx.server.config().clusterConfigFile), error);
        ctx.reply().ok();
        return;
    }

    if (subcommand == "FORGET" && ctx.args.size() == 3) {
        if (!cluster.forgetNode(ctx.args[2])) {
            ctx.reply().error("ERR Unknown node, or an attempt to forget this node itself");
            return;
        }
        std::string error;
        cluster.saveConfig(ctx.server.config().resolvePath(ctx.server.config().clusterConfigFile), error);
        ctx.reply().ok();
        return;
    }

    if (subcommand == "RESET") {
        cluster.initialiseSingleNode(ctx.server.config().nodeId, ctx.server.config().bindAddress,
                                     ctx.server.config().port);
        std::string error;
        cluster.saveConfig(ctx.server.config().resolvePath(ctx.server.config().clusterConfigFile), error);
        ctx.reply().ok();
        return;
    }

    ctx.reply().error("ERR Unknown CLUSTER subcommand '" + ctx.args[1] + "'. Try CLUSTER HELP.");
}

} // namespace

void registerClusterCommands(CommandTable& table) {
    using namespace cmdflag;
    table.add({"CLUSTER", cmdCluster, -2, kAdmin, 0, 0, 0});
}

} // namespace miniredis
