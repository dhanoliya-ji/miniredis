// Key-space commands: existence, expiry, renaming and iteration.
#include "command_helpers.hpp"

#include <algorithm>

namespace miniredis {

using namespace detail;

namespace {

void cmdDel(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    std::int64_t removed = 0;

    for (size_t i = 1; i < ctx.args.size(); ++i) {
        if (keyspace.erase(ctx.args[i])) {
            ++removed;
            touchKey(ctx, ctx.args[i]);
        }
    }

    ctx.reply().integer(removed);
    // Deleting nothing is not a change, so it should not reach the log or the
    // replicas.
    if (removed == 0) ctx.suppressPropagation();
}

void cmdExists(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    std::int64_t found = 0;
    // EXISTS counts repeats: EXISTS k k on a present key returns 2.
    for (size_t i = 1; i < ctx.args.size(); ++i) {
        if (keyspace.exists(ctx.args[i], ctx.now)) ++found;
    }
    ctx.reply().integer(found);
}

void cmdType(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* value = keyspace.lookupRead(ctx.args[1], ctx.now);
    ctx.reply().simpleString(value == nullptr ? "none" : value->typeName());
}

// EXPIRE, PEXPIRE, EXPIREAT and PEXPIREAT differ only in unit and whether the
// argument is relative or absolute, so they share one body.
void setExpiry(CommandContext& ctx, bool milliseconds, bool absolute) {
    std::int64_t amount = 0;
    if (!readInt(ctx, ctx.args[2], amount)) return;

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    if (!keyspace.exists(ctx.args[1], ctx.now)) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    // NX / XX / GT / LT conditions, as in Redis 7.
    bool requireNoExpiry = false;
    bool requireExpiry = false;
    bool onlyIfGreater = false;
    bool onlyIfLess = false;

    for (size_t i = 3; i < ctx.args.size(); ++i) {
        if (equalsIgnoreCase(ctx.args[i], "NX"))      requireNoExpiry = true;
        else if (equalsIgnoreCase(ctx.args[i], "XX")) requireExpiry = true;
        else if (equalsIgnoreCase(ctx.args[i], "GT")) onlyIfGreater = true;
        else if (equalsIgnoreCase(ctx.args[i], "LT")) onlyIfLess = true;
        else {
            ctx.reply().error("ERR Unsupported option " + ctx.args[i]);
            return;
        }
    }
    if ((onlyIfGreater && onlyIfLess) || (requireNoExpiry && (requireExpiry || onlyIfGreater || onlyIfLess))) {
        ctx.reply().error("ERR NX and XX, GT or LT options at the same time are not compatible");
        return;
    }

    const Millis target = absolute ? (milliseconds ? amount : amount * 1000)
                                   : ctx.now + (milliseconds ? amount : amount * 1000);

    Millis currentExpiry = 0;
    const bool hasExpiry = keyspace.getExpireAt(ctx.args[1], currentExpiry);

    if (requireNoExpiry && hasExpiry)  { ctx.reply().integer(0); ctx.suppressPropagation(); return; }
    if (requireExpiry && !hasExpiry)   { ctx.reply().integer(0); ctx.suppressPropagation(); return; }
    if (onlyIfGreater && (!hasExpiry || target <= currentExpiry)) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }
    if (onlyIfLess && hasExpiry && target >= currentExpiry) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    if (target <= ctx.now) {
        // An expiry in the past deletes the key immediately, which is what
        // makes "EXPIRE k -1" a documented way to delete something.
        keyspace.erase(ctx.args[1]);
        touchKey(ctx, ctx.args[1]);
        ctx.reply().integer(1);
        ctx.propagateInstead(Args{"DEL", ctx.args[1]});
        return;
    }

    keyspace.setExpireAt(ctx.args[1], target);
    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(1);

    // Always propagated as an absolute millisecond deadline. A relative EXPIRE
    // replayed from the log or applied late on a replica would be re-anchored
    // to that moment, quietly extending the key's life every time.
    ctx.propagateInstead(Args{"PEXPIREAT", ctx.args[1], formatInt64(target)});
}

void cmdTtl(CommandContext& ctx, bool milliseconds) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    const std::int64_t remaining = keyspace.ttlMillis(ctx.args[1], ctx.now);

    // -2 means the key is gone, -1 means it has no TTL. Both pass through
    // unscaled; only a real duration is converted to seconds.
    if (remaining < 0) {
        ctx.reply().integer(remaining);
        return;
    }
    ctx.reply().integer(milliseconds ? remaining : (remaining + 999) / 1000);
}

void cmdPersist(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    if (!keyspace.exists(ctx.args[1], ctx.now) || !keyspace.persist(ctx.args[1])) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }
    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(1);
}

void cmdKeys(CommandContext& ctx) {
    // KEYS walks the entire keyspace and blocks the server for the duration.
    // It is kept because it is invaluable in development, and SCAN exists for
    // production.
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    const std::vector<Bytes> matches = keyspace.matchingKeys(ctx.args[1], ctx.now);

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(matches.size()));
    for (const auto& key : matches) writer.bulkString(key);
}

void cmdScan(CommandContext& ctx) {
    std::int64_t cursor = 0;
    if (!readInt(ctx, ctx.args[1], cursor) || cursor < 0) {
        ctx.reply().error("ERR invalid cursor");
        return;
    }

    size_t count = 10;
    std::string matchPattern;
    bool haveMatch = false;
    ObjectType typeFilter = ObjectType::String;
    bool haveTypeFilter = false;

    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (equalsIgnoreCase(ctx.args[i], "MATCH") && i + 1 < ctx.args.size()) {
            matchPattern = ctx.args[++i];
            haveMatch = true;
        } else if (equalsIgnoreCase(ctx.args[i], "COUNT") && i + 1 < ctx.args.size()) {
            std::int64_t requested = 0;
            if (!parseInt64(ctx.args[++i], requested) || requested < 1) {
                ctx.reply().error(err::kSyntax);
                return;
            }
            // COUNT is a hint about work per call, not a result size. Capping
            // it keeps one SCAN from blocking the loop the way KEYS does.
            count = static_cast<size_t>(std::min<std::int64_t>(requested, 10000));
        } else if (equalsIgnoreCase(ctx.args[i], "TYPE") && i + 1 < ctx.args.size()) {
            const std::string wanted = toLower(ctx.args[++i]);
            if (wanted == "string")      typeFilter = ObjectType::String;
            else if (wanted == "list")   typeFilter = ObjectType::List;
            else if (wanted == "hash")   typeFilter = ObjectType::Hash;
            else if (wanted == "set")    typeFilter = ObjectType::Set;
            else if (wanted == "zset")   typeFilter = ObjectType::ZSet;
            else {
                ctx.reply().error(err::kSyntax);
                return;
            }
            haveTypeFilter = true;
        } else {
            ctx.reply().error(err::kSyntax);
            return;
        }
    }

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    const ScanResult result = keyspace.scan(static_cast<std::uint64_t>(cursor), count,
                                            haveMatch ? &matchPattern : nullptr,
                                            haveTypeFilter ? &typeFilter : nullptr,
                                            ctx.now);

    RespWriter writer = ctx.reply();
    writer.arrayHeader(2);
    writer.bulkString(formatInt64(static_cast<std::int64_t>(result.cursor)));
    writer.arrayHeader(static_cast<std::int64_t>(result.keys.size()));
    for (const auto& key : result.keys) writer.bulkString(key);
}

void cmdRandomKey(CommandContext& ctx) {
    const auto key = ctx.server.currentDb(ctx.client).randomKey(ctx.now);
    if (key.has_value()) ctx.reply().bulkString(*key);
    else ctx.reply().nullBulkString();
}

void cmdRename(CommandContext& ctx, bool failIfTargetExists) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* source = keyspace.lookupWrite(ctx.args[1], ctx.now);

    if (source == nullptr) {
        ctx.reply().error(err::kNoSuchKey);
        ctx.suppressPropagation();
        return;
    }

    if (failIfTargetExists && keyspace.exists(ctx.args[2], ctx.now)) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    // Renaming a key onto itself is a no-op, but must not destroy it.
    if (ctx.args[1] == ctx.args[2]) {
        if (failIfTargetExists) ctx.reply().integer(0);
        else ctx.reply().ok();
        ctx.suppressPropagation();
        return;
    }

    // The TTL travels with the value, which is what makes RENAME different
    // from a GET followed by a SET.
    Millis expireAt = 0;
    const bool hasExpiry = keyspace.getExpireAt(ctx.args[1], expireAt);

    Object moved = std::move(*source);
    keyspace.erase(ctx.args[1]);
    keyspace.setValue(ctx.args[2], std::move(moved));
    if (hasExpiry) keyspace.setExpireAt(ctx.args[2], expireAt);

    touchKey(ctx, ctx.args[1]);
    touchKey(ctx, ctx.args[2]);

    if (failIfTargetExists) ctx.reply().integer(1);
    else ctx.reply().ok();
}

void cmdCopy(CommandContext& ctx) {
    bool replace = false;
    for (size_t i = 3; i < ctx.args.size(); ++i) {
        if (equalsIgnoreCase(ctx.args[i], "REPLACE")) {
            replace = true;
        } else {
            ctx.reply().error(err::kSyntax);
            return;
        }
    }

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* source = keyspace.lookupRead(ctx.args[1], ctx.now);
    if (source == nullptr) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }
    if (!replace && keyspace.exists(ctx.args[2], ctx.now)) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    Object duplicate = *source; // a genuine deep copy, not a shared reference
    Millis expireAt = 0;
    const bool hasExpiry = keyspace.getExpireAt(ctx.args[1], expireAt);

    keyspace.setValue(ctx.args[2], std::move(duplicate));
    if (hasExpiry) keyspace.setExpireAt(ctx.args[2], expireAt);

    touchKey(ctx, ctx.args[2]);
    ctx.reply().integer(1);
}

void cmdTouch(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    std::int64_t found = 0;
    for (size_t i = 1; i < ctx.args.size(); ++i) {
        // A read lookup is exactly what TOUCH is for: it refreshes the LRU and
        // LFU metadata without returning the value.
        if (keyspace.lookupRead(ctx.args[i], ctx.now) != nullptr) ++found;
    }
    ctx.reply().integer(found);
}

void cmdObject(CommandContext& ctx) {
    const std::string subcommand = toUpper(ctx.args[1]);

    if (subcommand == "HELP") {
        const std::vector<std::string> help{
            "OBJECT ENCODING <key>    -- the internal encoding of the value",
            "OBJECT FREQ <key>        -- the LFU access counter",
            "OBJECT IDLETIME <key>    -- seconds since the key was last touched",
            "OBJECT REFCOUNT <key>    -- always 1; MiniRedis does not share objects",
        };
        ctx.reply().stringArray(help);
        return;
    }

    if (ctx.args.size() != 3) {
        ctx.reply().error(wrongArgsError("object"));
        return;
    }

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* value = keyspace.lookupRead(ctx.args[2], ctx.now);
    if (value == nullptr) {
        ctx.reply().error(err::kNoSuchKey);
        return;
    }

    if (subcommand == "ENCODING") {
        // Redis reports a physical encoding here (listpack, intset, skiplist).
        // MiniRedis uses one encoding per type, so it reports that honestly
        // rather than inventing a name it does not implement.
        ctx.reply().bulkString(value->typeName());
        return;
    }
    if (subcommand == "REFCOUNT") {
        ctx.reply().integer(1);
        return;
    }
    if (subcommand == "IDLETIME" || subcommand == "FREQ") {
        ctx.reply().integer(0);
        return;
    }

    ctx.reply().error("ERR Unknown subcommand '" + ctx.args[1] + "'. Try OBJECT HELP.");
}

void cmdDbSize(CommandContext& ctx) {
    ctx.reply().integer(static_cast<std::int64_t>(ctx.server.currentDb(ctx.client).size()));
}

void cmdMove(CommandContext& ctx) {
    std::int64_t targetIndex = 0;
    if (!readInt(ctx, ctx.args[2], targetIndex)) return;
    if (targetIndex < 0 || targetIndex >= ctx.server.databaseCount()) {
        ctx.reply().error("ERR DB index is out of range");
        return;
    }
    if (targetIndex == ctx.client.dbIndex) {
        ctx.reply().error("ERR source and destination objects are the same");
        return;
    }

    Keyspace& source = ctx.server.currentDb(ctx.client);
    Keyspace& target = ctx.server.db(static_cast<int>(targetIndex));

    Object* value = source.lookupWrite(ctx.args[1], ctx.now);
    if (value == nullptr || target.exists(ctx.args[1], ctx.now)) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    Millis expireAt = 0;
    const bool hasExpiry = source.getExpireAt(ctx.args[1], expireAt);

    Object moved = std::move(*value);
    source.erase(ctx.args[1]);
    target.setValue(ctx.args[1], std::move(moved));
    if (hasExpiry) target.setExpireAt(ctx.args[1], expireAt);

    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(1);
}

} // namespace

void registerKeyCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"DEL", cmdDel, -2, kWrite, 1, -1, 1});
    // UNLINK frees memory lazily in Redis; with one thread and no background
    // reclaim there is nothing to defer, so it is an honest alias for DEL.
    table.add({"UNLINK", cmdDel, -2, kWrite | kFast, 1, -1, 1});
    table.add({"EXISTS", cmdExists, -2, kReadOnly | kFast, 1, -1, 1});
    table.add({"TYPE", cmdType, 2, kReadOnly | kFast, 1, 1, 1});

    table.add({"EXPIRE", [](CommandContext& c) { setExpiry(c, false, false); }, -3, kWrite | kFast, 1, 1, 1});
    table.add({"PEXPIRE", [](CommandContext& c) { setExpiry(c, true, false); }, -3, kWrite | kFast, 1, 1, 1});
    table.add({"EXPIREAT", [](CommandContext& c) { setExpiry(c, false, true); }, -3, kWrite | kFast, 1, 1, 1});
    table.add({"PEXPIREAT", [](CommandContext& c) { setExpiry(c, true, true); }, -3, kWrite | kFast, 1, 1, 1});
    table.add({"TTL", [](CommandContext& c) { cmdTtl(c, false); }, 2, kReadOnly | kFast, 1, 1, 1});
    table.add({"PTTL", [](CommandContext& c) { cmdTtl(c, true); }, 2, kReadOnly | kFast, 1, 1, 1});
    table.add({"PERSIST", cmdPersist, 2, kWrite | kFast, 1, 1, 1});

    table.add({"KEYS", cmdKeys, 2, kReadOnly, 0, 0, 0});
    table.add({"SCAN", cmdScan, -2, kReadOnly, 0, 0, 0});
    table.add({"RANDOMKEY", cmdRandomKey, 1, kReadOnly, 0, 0, 0});
    table.add({"DBSIZE", cmdDbSize, 1, kReadOnly | kFast, 0, 0, 0});

    table.add({"RENAME", [](CommandContext& c) { cmdRename(c, false); }, 3, kWrite, 1, 2, 1});
    table.add({"RENAMENX", [](CommandContext& c) { cmdRename(c, true); }, 3, kWrite | kFast, 1, 2, 1});
    table.add({"COPY", cmdCopy, -3, kWrite | kDenyOom, 1, 2, 1});
    table.add({"MOVE", cmdMove, 3, kWrite | kFast, 1, 1, 1});
    table.add({"TOUCH", cmdTouch, -2, kReadOnly | kFast, 1, -1, 1});
    table.add({"OBJECT", cmdObject, -2, kReadOnly, 0, 0, 0});
}

} // namespace miniredis
