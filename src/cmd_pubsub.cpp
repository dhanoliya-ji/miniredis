// Pub/Sub commands.
#include "command_helpers.hpp"

#include "miniredis/pubsub.hpp"
#include "miniredis/replication.hpp"

namespace miniredis {

using namespace detail;

namespace {

// SUBSCRIBE and friends reply once per channel named, each carrying the
// client's running subscription total. Clients rely on that count to know when
// they have left subscribe mode.
void writeSubscriptionAck(RespWriter& writer, std::string_view kind,
                          std::string_view channel, std::int64_t total) {
    writer.arrayHeader(3);
    writer.bulkString(kind);
    writer.bulkString(channel);
    writer.integer(total);
}

void cmdSubscribe(CommandContext& ctx) {
    RespWriter writer = ctx.reply();
    for (size_t i = 1; i < ctx.args.size(); ++i) {
        const size_t total = ctx.server.pubsub().subscribeChannel(ctx.client, ctx.args[i]);
        writeSubscriptionAck(writer, "subscribe", ctx.args[i], static_cast<std::int64_t>(total));
    }
}

void cmdPSubscribe(CommandContext& ctx) {
    RespWriter writer = ctx.reply();
    for (size_t i = 1; i < ctx.args.size(); ++i) {
        const size_t total = ctx.server.pubsub().subscribePattern(ctx.client, ctx.args[i]);
        writeSubscriptionAck(writer, "psubscribe", ctx.args[i], static_cast<std::int64_t>(total));
    }
}

void cmdUnsubscribe(CommandContext& ctx) {
    RespWriter writer = ctx.reply();

    if (ctx.args.size() == 1) {
        // UNSUBSCRIBE with no arguments drops every channel. A client that was
        // subscribed to nothing still gets one acknowledgement, so it always
        // has something to wait for.
        const std::vector<std::string> channels(ctx.client.subscribedChannels.begin(),
                                                ctx.client.subscribedChannels.end());
        if (channels.empty()) {
            writer.arrayHeader(3);
            writer.bulkString("unsubscribe");
            writer.nullBulkString();
            writer.integer(static_cast<std::int64_t>(ctx.client.subscribedPatterns.size()));
            return;
        }
        for (const auto& channel : channels) {
            const size_t total = ctx.server.pubsub().unsubscribeChannel(ctx.client, channel);
            writeSubscriptionAck(writer, "unsubscribe", channel, static_cast<std::int64_t>(total));
        }
        return;
    }

    for (size_t i = 1; i < ctx.args.size(); ++i) {
        const size_t total = ctx.server.pubsub().unsubscribeChannel(ctx.client, ctx.args[i]);
        writeSubscriptionAck(writer, "unsubscribe", ctx.args[i], static_cast<std::int64_t>(total));
    }
}

void cmdPUnsubscribe(CommandContext& ctx) {
    RespWriter writer = ctx.reply();

    if (ctx.args.size() == 1) {
        const std::vector<std::string> patterns(ctx.client.subscribedPatterns.begin(),
                                                ctx.client.subscribedPatterns.end());
        if (patterns.empty()) {
            writer.arrayHeader(3);
            writer.bulkString("punsubscribe");
            writer.nullBulkString();
            writer.integer(static_cast<std::int64_t>(ctx.client.subscribedChannels.size()));
            return;
        }
        for (const auto& pattern : patterns) {
            const size_t total = ctx.server.pubsub().unsubscribePattern(ctx.client, pattern);
            writeSubscriptionAck(writer, "punsubscribe", pattern, static_cast<std::int64_t>(total));
        }
        return;
    }

    for (size_t i = 1; i < ctx.args.size(); ++i) {
        const size_t total = ctx.server.pubsub().unsubscribePattern(ctx.client, ctx.args[i]);
        writeSubscriptionAck(writer, "punsubscribe", ctx.args[i], static_cast<std::int64_t>(total));
    }
}

void cmdPublish(CommandContext& ctx) {
    const std::int64_t receivers = ctx.server.pubsub().publish(ctx.args[1], ctx.args[2]);
    ctx.reply().integer(receivers);

    // PUBLISH is forwarded to replicas so that a client subscribed to a
    // replica sees messages published on the master. It is not a keyspace
    // write, so it never reaches the AOF -- there is nothing to replay.
    if (ctx.server.replication().replicaCount() > 0) {
        ctx.server.replication().propagateToReplicas(encodeCommand(ctx.args));
    }
}

void cmdPubSub(CommandContext& ctx) {
    const std::string subcommand = toUpper(ctx.args[1]);

    if (subcommand == "CHANNELS") {
        const std::string pattern = ctx.args.size() > 2 ? ctx.args[2] : std::string();
        const auto channels = ctx.server.pubsub().channelsMatching(ctx.args.size() > 2 ? &pattern : nullptr);
        ctx.reply().stringArray(channels);
        return;
    }

    if (subcommand == "NUMSUB") {
        RespWriter writer = ctx.reply();
        writer.arrayHeader(static_cast<std::int64_t>((ctx.args.size() - 2) * 2));
        for (size_t i = 2; i < ctx.args.size(); ++i) {
            writer.bulkString(ctx.args[i]);
            writer.integer(static_cast<std::int64_t>(ctx.server.pubsub().subscriberCount(ctx.args[i])));
        }
        return;
    }

    if (subcommand == "NUMPAT") {
        ctx.reply().integer(static_cast<std::int64_t>(ctx.server.pubsub().patternCount()));
        return;
    }

    if (subcommand == "HELP") {
        const std::vector<std::string> help{
            "PUBSUB CHANNELS [pattern]  -- channels with at least one subscriber",
            "PUBSUB NUMSUB [channel...] -- subscriber count per channel",
            "PUBSUB NUMPAT              -- number of distinct patterns subscribed to",
        };
        ctx.reply().stringArray(help);
        return;
    }

    ctx.reply().error("ERR Unknown PUBSUB subcommand '" + ctx.args[1] + "'. Try PUBSUB HELP.");
}

} // namespace

void registerPubSubCommands(CommandTable& table) {
    using namespace cmdflag;

    // These carry kPubSub so they remain usable once a connection has entered
    // subscribe mode and ordinary commands are refused.
    table.add({"SUBSCRIBE", cmdSubscribe, -2, kPubSub | kFast, 0, 0, 0});
    table.add({"UNSUBSCRIBE", cmdUnsubscribe, -1, kPubSub | kFast, 0, 0, 0});
    table.add({"PSUBSCRIBE", cmdPSubscribe, -2, kPubSub | kFast, 0, 0, 0});
    table.add({"PUNSUBSCRIBE", cmdPUnsubscribe, -1, kPubSub | kFast, 0, 0, 0});
    table.add({"PUBLISH", cmdPublish, 3, kPubSub | kFast, 0, 0, 0});
    table.add({"PUBSUB", cmdPubSub, -2, kPubSub, 0, 0, 0});
}

} // namespace miniredis
