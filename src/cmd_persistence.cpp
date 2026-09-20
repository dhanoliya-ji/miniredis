// Persistence commands: SAVE, BGSAVE, BGREWRITEAOF, LASTSAVE.
#include "command_helpers.hpp"

#include "miniredis/aof.hpp"

namespace miniredis {

using namespace detail;

namespace {

void cmdSave(CommandContext& ctx) {
    // SAVE writes the snapshot on the server thread, so the whole server is
    // unavailable until it finishes. On a large dataset that is seconds of
    // downtime, which is why BGSAVE exists and why SAVE is a maintenance tool.
    std::string error;
    if (!ctx.server.saveSnapshot(error)) {
        ctx.reply().error("ERR " + error);
        return;
    }
    ctx.reply().ok();
}

void cmdBgSave(CommandContext& ctx) {
    // Real Redis forks here: the child gets a copy-on-write view of memory and
    // writes the snapshot while the parent keeps serving. MiniRedis does not
    // fork -- fork() does not exist on Windows, and a background thread would
    // need the keyspace frozen for the whole save, which is what we were
    // trying to avoid.
    //
    // So this is honest about being synchronous rather than pretending to be
    // asynchronous, and the reply says so.
    std::string error;
    if (!ctx.server.saveSnapshot(error)) {
        ctx.reply().error("ERR " + error);
        return;
    }
    ctx.reply().simpleString("Background saving started (performed synchronously by MiniRedis)");
}

void cmdBgRewriteAof(CommandContext& ctx) {
    if (!ctx.server.aof().isEnabled()) {
        ctx.reply().error("ERR The append-only file is disabled; enable it with CONFIG SET appendonly yes");
        return;
    }

    std::string error;
    if (!ctx.server.aof().rewrite(error)) {
        ctx.reply().error("ERR " + error);
        return;
    }
    ctx.reply().simpleString("Background append only file rewriting started (performed synchronously)");
}

void cmdLastSave(CommandContext& ctx) {
    // The INFO report carries the same timestamp; this command exists so a
    // backup script can poll for "has a save completed since I asked".
    std::string report = ctx.server.infoReport("persistence");
    const std::string field = "rdb_last_save_time:";
    const size_t at = report.find(field);
    if (at == std::string::npos) {
        ctx.reply().integer(0);
        return;
    }
    const size_t valueStart = at + field.size();
    const size_t valueEnd = report.find('\r', valueStart);
    std::int64_t timestamp = 0;
    parseInt64(report.substr(valueStart, valueEnd - valueStart), timestamp);
    ctx.reply().integer(timestamp);
}

} // namespace

void registerPersistenceCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"SAVE", cmdSave, 1, kAdmin | kNoMulti, 0, 0, 0});
    table.add({"BGSAVE", cmdBgSave, -1, kAdmin | kNoMulti, 0, 0, 0});
    table.add({"BGREWRITEAOF", cmdBgRewriteAof, 1, kAdmin | kNoMulti, 0, 0, 0});
    table.add({"LASTSAVE", cmdLastSave, 1, kReadOnly | kFast, 0, 0, 0});
}

} // namespace miniredis
