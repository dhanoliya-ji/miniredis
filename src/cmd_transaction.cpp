// Transactions: MULTI, EXEC, DISCARD, WATCH, UNWATCH.
//
// A Redis transaction is not a database transaction. It is two guarantees and
// no more:
//
//   1. Isolation. Queued commands run back to back with nothing interleaved.
//      On a single-threaded server that is free -- nothing else can run.
//
//   2. All-or-nothing *dispatch*. Either every queued command is attempted or
//      none is.
//
// What it is NOT is atomic on failure. If the third of five commands fails at
// runtime -- say INCR against a key holding a list -- the first two stay
// applied and the last two still run. There is no rollback. Redis made that
// choice deliberately: a runtime type error is a bug in the client, and
// paying for undo logs on every command to clean up after bugs is a bad trade.
//
// WATCH supplies the missing piece: optimistic concurrency. Watch a key, and
// if anybody touches it before your EXEC, the EXEC aborts and you retry. That
// is compare-and-swap, and it is how you build a correct read-modify-write
// without a lock.
#include "command_helpers.hpp"

#include <algorithm>

namespace miniredis {

using namespace detail;

namespace {

void cmdMulti(CommandContext& ctx) {
    if (ctx.client.inMulti) {
        ctx.reply().error("ERR MULTI calls can not be nested");
        return;
    }
    ctx.client.inMulti = true;
    ctx.client.multiQueue.clear();
    ctx.client.multiQueueError = false;
    ctx.reply().ok();
}

void cmdDiscard(CommandContext& ctx) {
    if (!ctx.client.inMulti) {
        ctx.reply().error("ERR DISCARD without MULTI");
        return;
    }

    ctx.client.inMulti = false;
    ctx.client.multiQueue.clear();
    ctx.client.multiQueueError = false;
    // DISCARD also releases every WATCH, because the point of watching was to
    // guard the transaction being abandoned.
    ctx.client.watchedKeys.clear();
    ctx.client.watchInvalidated = false;
    ctx.reply().ok();
}

void cmdWatch(CommandContext& ctx) {
    if (ctx.client.inMulti) {
        // Watching from inside a transaction is meaningless: the check happens
        // at EXEC, which is already the next thing to run.
        ctx.reply().error("ERR WATCH inside MULTI is not allowed");
        return;
    }

    for (size_t i = 1; i < ctx.args.size(); ++i) {
        const WatchedKey entry{ctx.client.dbIndex, ctx.args[i]};
        const bool alreadyWatching = std::any_of(
            ctx.client.watchedKeys.begin(), ctx.client.watchedKeys.end(),
            [&](const WatchedKey& existing) {
                return existing.dbIndex == entry.dbIndex && existing.key == entry.key;
            });
        if (!alreadyWatching) ctx.client.watchedKeys.push_back(entry);
    }
    ctx.reply().ok();
}

void cmdUnwatch(CommandContext& ctx) {
    ctx.client.watchedKeys.clear();
    ctx.client.watchInvalidated = false;
    ctx.reply().ok();
}

void cmdExec(CommandContext& ctx) {
    if (!ctx.client.inMulti) {
        ctx.reply().error("ERR EXEC without MULTI");
        return;
    }

    // A command that failed to queue poisons the whole transaction. Running
    // the rest would silently drop one operation from a sequence the client
    // believes is complete, which is worse than refusing the lot.
    if (ctx.client.multiQueueError) {
        ctx.client.inMulti = false;
        ctx.client.multiQueue.clear();
        ctx.client.multiQueueError = false;
        ctx.client.watchedKeys.clear();
        ctx.client.watchInvalidated = false;
        ctx.reply().error("EXECABORT Transaction discarded because of previous errors.");
        return;
    }

    // The WATCH check. Note that it is a *flag*, not a value comparison: a key
    // that was written and then written back to its original value still
    // aborts the transaction. That is intentional -- it catches the ABA
    // problem, where a value looks unchanged but the world moved underneath.
    if (ctx.client.watchInvalidated) {
        ctx.client.inMulti = false;
        ctx.client.multiQueue.clear();
        ctx.client.watchedKeys.clear();
        ctx.client.watchInvalidated = false;
        ctx.reply().nullArray();
        return;
    }

    const std::vector<Args> queued = std::move(ctx.client.multiQueue);
    ctx.client.multiQueue.clear();
    ctx.client.inMulti = false;
    ctx.client.watchedKeys.clear();
    ctx.client.watchInvalidated = false;

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(queued.size()));

    for (const Args& command : queued) {
        const CommandSpec* spec = ctx.server.commands().find(command[0]);
        if (spec == nullptr) {
            writer.error("ERR unknown command '" + command[0] + "'");
            continue;
        }

        std::string reply;
        CommandContext inner{ctx.server, ctx.client, command, reply, ctx.now};

        const std::int64_t dirtyBefore = ctx.server.dirty();
        spec->handler(inner);

        // Each queued command propagates on its own. Wrapping the batch in
        // MULTI/EXEC on the replica link would be tidier, but a queued command
        // that turned out to be a no-op should still not be shipped, and
        // per-command propagation keeps that rule in one place.
        if (spec->isWrite() && !inner.propagationSuppressed && ctx.server.dirty() != dirtyBefore) {
            ctx.server.propagate(ctx.client.dbIndex,
                                 inner.hasPropagationOverride ? inner.propagationOverride : command);
        }

        writer.raw(reply);
    }
}

} // namespace

void registerTransactionCommands(CommandTable& table) {
    using namespace cmdflag;

    // All four carry kNoMulti so the dispatcher runs them instead of queueing
    // them -- queueing an EXEC inside a MULTI would be a deadlock of sorts.
    table.add({"MULTI", cmdMulti, 1, kFast | kNoMulti, 0, 0, 0});
    table.add({"EXEC", cmdExec, 1, kNoMulti, 0, 0, 0});
    table.add({"DISCARD", cmdDiscard, 1, kFast | kNoMulti, 0, 0, 0});
    table.add({"WATCH", cmdWatch, -2, kFast | kNoMulti, 1, -1, 1});
    table.add({"UNWATCH", cmdUnwatch, 1, kFast | kNoMulti, 0, 0, 0});
}

} // namespace miniredis
