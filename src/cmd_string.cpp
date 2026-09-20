// String commands: the Redis type that is really "a byte array with counters".
#include "command_helpers.hpp"

#include <algorithm>
#include <cmath>

namespace miniredis {

using namespace detail;

namespace {

// SET has accumulated a lot of options over the years; parsing them in one
// place keeps SET, SETEX, PSETEX and SETNX from each growing their own copy.
struct SetOptions {
    bool onlyIfAbsent = false;  // NX
    bool onlyIfPresent = false; // XX
    bool keepTtl = false;       // KEEPTTL
    bool returnOldValue = false;// GET
    bool hasExpiry = false;
    Millis expireAtMs = 0;
};

bool parseSetOptions(CommandContext& ctx, size_t firstOption, SetOptions& options) {
    for (size_t i = firstOption; i < ctx.args.size(); ++i) {
        const Bytes& token = ctx.args[i];

        if (equalsIgnoreCase(token, "NX")) {
            options.onlyIfAbsent = true;
        } else if (equalsIgnoreCase(token, "XX")) {
            options.onlyIfPresent = true;
        } else if (equalsIgnoreCase(token, "KEEPTTL")) {
            options.keepTtl = true;
        } else if (equalsIgnoreCase(token, "GET")) {
            options.returnOldValue = true;
        } else if (equalsIgnoreCase(token, "EX") || equalsIgnoreCase(token, "PX") ||
                   equalsIgnoreCase(token, "EXAT") || equalsIgnoreCase(token, "PXAT")) {
            if (i + 1 >= ctx.args.size()) {
                ctx.reply().error(err::kSyntax);
                return false;
            }
            std::int64_t amount = 0;
            if (!parseInt64(ctx.args[i + 1], amount)) {
                ctx.reply().error(err::kNotInteger);
                return false;
            }
            // A zero or negative relative TTL is rejected rather than silently
            // creating a key that is already dead.
            if ((equalsIgnoreCase(token, "EX") || equalsIgnoreCase(token, "PX")) && amount <= 0) {
                ctx.reply().error("ERR invalid expire time in 'set' command");
                return false;
            }

            if (equalsIgnoreCase(token, "EX"))        options.expireAtMs = ctx.now + amount * 1000;
            else if (equalsIgnoreCase(token, "PX"))   options.expireAtMs = ctx.now + amount;
            else if (equalsIgnoreCase(token, "EXAT")) options.expireAtMs = amount * 1000;
            else                                      options.expireAtMs = amount;

            options.hasExpiry = true;
            ++i;
        } else {
            ctx.reply().error(err::kSyntax);
            return false;
        }
    }

    if (options.onlyIfAbsent && options.onlyIfPresent) {
        ctx.reply().error(err::kSyntax);
        return false;
    }
    return true;
}

// The shared body of SET and its shorthands.
void performSet(CommandContext& ctx, const Bytes& key, const Bytes& value, const SetOptions& options) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);

    Object* existing = keyspace.lookupWrite(key, ctx.now);
    if (existing != nullptr && options.onlyIfAbsent) {
        if (options.returnOldValue && existing->type() == ObjectType::String) {
            ctx.reply().bulkString(existing->string());
        } else {
            ctx.reply().nullBulkString();
        }
        ctx.suppressPropagation();
        return;
    }
    if (existing == nullptr && options.onlyIfPresent) {
        ctx.reply().nullBulkString();
        ctx.suppressPropagation();
        return;
    }

    Bytes oldValue;
    bool haveOldValue = false;
    if (options.returnOldValue && existing != nullptr) {
        if (existing->type() != ObjectType::String) {
            ctx.reply().error(err::kWrongType);
            ctx.suppressPropagation();
            return;
        }
        oldValue = existing->string();
        haveOldValue = true;
    }

    if (options.keepTtl) {
        keyspace.setValueKeepTtl(key, Object::makeString(value));
    } else {
        // A plain SET clears any TTL the key had. This trips people up, so it
        // is worth being explicit: SET is a full replacement of the key, not
        // just of its value.
        keyspace.setValue(key, Object::makeString(value));
    }

    if (options.hasExpiry) {
        keyspace.setExpireAt(key, options.expireAtMs);
    }

    touchKey(ctx, key);

    if (options.returnOldValue) {
        if (haveOldValue) ctx.reply().bulkString(oldValue);
        else ctx.reply().nullBulkString();
    } else {
        ctx.reply().ok();
    }

    // An expiry given as a relative EX/PX is rewritten to an absolute
    // PEXPIREAT for the log and the replicas, so replay does not re-anchor it
    // to a later clock. Without this every restart would extend the key's life.
    if (options.hasExpiry) {
        Args rewritten{"SET", key, value};
        rewritten.push_back("PXAT");
        rewritten.push_back(formatInt64(options.expireAtMs));
        if (options.keepTtl) rewritten.push_back("KEEPTTL");
        ctx.propagateInstead(std::move(rewritten));
    }
}

void cmdSet(CommandContext& ctx) {
    SetOptions options;
    if (!parseSetOptions(ctx, 3, options)) return;
    performSet(ctx, ctx.args[1], ctx.args[2], options);
}

void cmdSetNx(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    if (keyspace.lookupWrite(ctx.args[1], ctx.now) != nullptr) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }
    keyspace.setValue(ctx.args[1], Object::makeString(ctx.args[2]));
    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(1);
}

void cmdSetEx(CommandContext& ctx, bool milliseconds) {
    std::int64_t ttl = 0;
    if (!readInt(ctx, ctx.args[2], ttl)) return;
    if (ttl <= 0) {
        ctx.reply().error("ERR invalid expire time in '" + toLower(ctx.args[0]) + "' command");
        return;
    }

    SetOptions options;
    options.hasExpiry = true;
    options.expireAtMs = ctx.now + (milliseconds ? ttl : ttl * 1000);
    performSet(ctx, ctx.args[1], ctx.args[3], options);
}

void cmdGet(CommandContext& ctx) {
    Object* value = nullptr;
    switch (lookupTyped(ctx, ctx.args[1], ObjectType::String, value)) {
        case LookupOutcome::Found:   ctx.reply().bulkString(value->string()); break;
        case LookupOutcome::Missing: ctx.reply().nullBulkString(); break;
        case LookupOutcome::WrongType: break;
    }
}

void cmdGetSet(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* existing = keyspace.lookupWrite(ctx.args[1], ctx.now);

    if (existing != nullptr && existing->type() != ObjectType::String) {
        ctx.reply().error(err::kWrongType);
        return;
    }

    if (existing != nullptr) ctx.reply().bulkString(existing->string());
    else ctx.reply().nullBulkString();

    keyspace.setValue(ctx.args[1], Object::makeString(ctx.args[2]));
    touchKey(ctx, ctx.args[1]);
}

void cmdGetDel(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::String, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().nullBulkString();
        ctx.suppressPropagation();
        return;
    }

    ctx.reply().bulkString(value->string());
    ctx.server.currentDb(ctx.client).erase(ctx.args[1]);
    touchKey(ctx, ctx.args[1]);

    // Replay must delete the key, not re-read it.
    ctx.propagateInstead(Args{"DEL", ctx.args[1]});
}

void cmdMGet(CommandContext& ctx) {
    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(ctx.args.size() - 1));

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    for (size_t i = 1; i < ctx.args.size(); ++i) {
        Object* value = keyspace.lookupRead(ctx.args[i], ctx.now);
        // A wrong-typed key is reported as nil rather than failing the whole
        // command, because MGET is a best-effort bulk read.
        if (value == nullptr || value->type() != ObjectType::String) {
            writer.nullBulkString();
        } else {
            writer.bulkString(value->string());
        }
    }
}

void cmdMSet(CommandContext& ctx) {
    if ((ctx.args.size() - 1) % 2 != 0) {
        ctx.reply().error(wrongArgsError(ctx.args[0]));
        return;
    }

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    for (size_t i = 1; i + 1 < ctx.args.size(); i += 2) {
        keyspace.setValue(ctx.args[i], Object::makeString(ctx.args[i + 1]));
        touchKey(ctx, ctx.args[i]);
    }
    ctx.reply().ok();
}

void cmdMSetNx(CommandContext& ctx) {
    if ((ctx.args.size() - 1) % 2 != 0) {
        ctx.reply().error(wrongArgsError(ctx.args[0]));
        return;
    }

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);

    // All or nothing: every key is checked before any is written, which is the
    // only reason to use MSETNX over a loop of SETNX.
    for (size_t i = 1; i + 1 < ctx.args.size(); i += 2) {
        if (keyspace.exists(ctx.args[i], ctx.now)) {
            ctx.reply().integer(0);
            ctx.suppressPropagation();
            return;
        }
    }

    for (size_t i = 1; i + 1 < ctx.args.size(); i += 2) {
        keyspace.setValue(ctx.args[i], Object::makeString(ctx.args[i + 1]));
        touchKey(ctx, ctx.args[i]);
    }
    ctx.reply().integer(1);
}

// INCR and friends. Redis stores counters as strings and re-parses them on
// every operation, which is why INCR on a 20-digit value is still O(1) but
// INCR on "hello" is an error rather than a coercion.
void incrementBy(CommandContext& ctx, const Bytes& key, std::int64_t delta) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* existing = keyspace.lookupWrite(key, ctx.now);

    std::int64_t current = 0;
    if (existing != nullptr) {
        if (existing->type() != ObjectType::String) {
            ctx.reply().error(err::kWrongType);
            return;
        }
        if (!parseInt64(existing->string(), current)) {
            ctx.reply().error(err::kNotInteger);
            return;
        }
    }

    // Overflow is an error, not a wrap. A counter that silently wraps to
    // negative is far worse than one that refuses to move.
    if ((delta > 0 && current > INT64_MAX - delta) ||
        (delta < 0 && current < INT64_MIN - delta)) {
        ctx.reply().error("ERR increment or decrement would overflow");
        return;
    }

    const std::int64_t updated = current + delta;
    keyspace.setValueKeepTtl(key, Object::makeString(formatInt64(updated)));
    touchKey(ctx, key);
    ctx.reply().integer(updated);
}

void cmdIncr(CommandContext& ctx)   { incrementBy(ctx, ctx.args[1], 1); }
void cmdDecr(CommandContext& ctx)   { incrementBy(ctx, ctx.args[1], -1); }

void cmdIncrBy(CommandContext& ctx) {
    std::int64_t delta = 0;
    if (!readInt(ctx, ctx.args[2], delta)) return;
    incrementBy(ctx, ctx.args[1], delta);
}

void cmdDecrBy(CommandContext& ctx) {
    std::int64_t delta = 0;
    if (!readInt(ctx, ctx.args[2], delta)) return;
    if (delta == INT64_MIN) {
        ctx.reply().error("ERR decrement would overflow");
        return;
    }
    incrementBy(ctx, ctx.args[1], -delta);
}

void cmdIncrByFloat(CommandContext& ctx) {
    double delta = 0;
    if (!readDouble(ctx, ctx.args[2], delta)) return;

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* existing = keyspace.lookupWrite(ctx.args[1], ctx.now);

    double current = 0;
    if (existing != nullptr) {
        if (existing->type() != ObjectType::String) {
            ctx.reply().error(err::kWrongType);
            return;
        }
        if (!parseDouble(existing->string(), current)) {
            ctx.reply().error(err::kNotFloat);
            return;
        }
    }

    const double updated = current + delta;
    if (std::isnan(updated) || std::isinf(updated)) {
        ctx.reply().error("ERR increment would produce NaN or Infinity");
        return;
    }

    const std::string rendered = formatDouble(updated);
    keyspace.setValueKeepTtl(ctx.args[1], Object::makeString(rendered));
    touchKey(ctx, ctx.args[1]);
    ctx.reply().bulkString(rendered);

    // Float addition is not associative and the result depends on the current
    // value, so replaying the increment on a replica could land on a different
    // last bit. Propagating the computed result keeps them identical.
    ctx.propagateInstead(Args{"SET", ctx.args[1], rendered, "KEEPTTL"});
}

void cmdAppend(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* existing = keyspace.lookupWrite(ctx.args[1], ctx.now);

    if (existing == nullptr) {
        keyspace.setValue(ctx.args[1], Object::makeString(ctx.args[2]));
        touchKey(ctx, ctx.args[1]);
        ctx.reply().integer(static_cast<std::int64_t>(ctx.args[2].size()));
        return;
    }
    if (existing->type() != ObjectType::String) {
        ctx.reply().error(err::kWrongType);
        return;
    }

    existing->string().append(ctx.args[2]);
    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(static_cast<std::int64_t>(existing->string().size()));
}

void cmdStrLen(CommandContext& ctx) {
    Object* value = nullptr;
    switch (lookupTyped(ctx, ctx.args[1], ObjectType::String, value)) {
        case LookupOutcome::Found:   ctx.reply().integer(static_cast<std::int64_t>(value->string().size())); break;
        case LookupOutcome::Missing: ctx.reply().integer(0); break;
        case LookupOutcome::WrongType: break;
    }
}

void cmdGetRange(CommandContext& ctx) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!readInt(ctx, ctx.args[2], start)) return;
    if (!readInt(ctx, ctx.args[3], stop)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::String, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().bulkString("");
        return;
    }

    std::int64_t from = 0;
    std::int64_t to = 0;
    if (!normaliseRange(start, stop, static_cast<std::int64_t>(value->string().size()), from, to)) {
        ctx.reply().bulkString("");
        return;
    }

    ctx.reply().bulkString(std::string_view(value->string()).substr(
        static_cast<size_t>(from), static_cast<size_t>(to - from + 1)));
}

void cmdSetRange(CommandContext& ctx) {
    std::int64_t offset = 0;
    if (!readInt(ctx, ctx.args[2], offset)) return;
    if (offset < 0) {
        ctx.reply().error("ERR offset is out of range");
        return;
    }

    const Bytes& patch = ctx.args[3];
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* existing = keyspace.lookupWrite(ctx.args[1], ctx.now);

    if (existing != nullptr && existing->type() != ObjectType::String) {
        ctx.reply().error(err::kWrongType);
        return;
    }

    if (patch.empty()) {
        // Writing nothing does not create the key, and does not change one.
        ctx.reply().integer(existing == nullptr ? 0 : static_cast<std::int64_t>(existing->string().size()));
        ctx.suppressPropagation();
        return;
    }

    const size_t required = static_cast<size_t>(offset) + patch.size();
    if (required > static_cast<size_t>(RespParser::kMaxBulkLength)) {
        ctx.reply().error("ERR string exceeds maximum allowed size");
        return;
    }

    if (existing == nullptr) {
        // Writing past the end of a missing key zero-pads the gap, so the
        // string has a defined value everywhere rather than uninitialised
        // bytes.
        Bytes fresh(static_cast<size_t>(offset), '\0');
        fresh.append(patch);
        keyspace.setValue(ctx.args[1], Object::makeString(std::move(fresh)));
        touchKey(ctx, ctx.args[1]);
        ctx.reply().integer(static_cast<std::int64_t>(required));
        return;
    }

    Bytes& target = existing->string();
    if (target.size() < required) target.resize(required, '\0');
    std::copy(patch.begin(), patch.end(), target.begin() + offset);

    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(static_cast<std::int64_t>(target.size()));
}

} // namespace

void registerStringCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"SET", cmdSet, -3, kWrite | kDenyOom, 1, 1, 1});
    table.add({"SETNX", cmdSetNx, 3, kWrite | kDenyOom, 1, 1, 1});
    table.add({"SETEX", [](CommandContext& c) { cmdSetEx(c, false); }, 4, kWrite | kDenyOom, 1, 1, 1});
    table.add({"PSETEX", [](CommandContext& c) { cmdSetEx(c, true); }, 4, kWrite | kDenyOom, 1, 1, 1});
    table.add({"GET", cmdGet, 2, kReadOnly | kFast, 1, 1, 1});
    table.add({"GETSET", cmdGetSet, 3, kWrite | kDenyOom, 1, 1, 1});
    table.add({"GETDEL", cmdGetDel, 2, kWrite | kFast, 1, 1, 1});
    table.add({"MGET", cmdMGet, -2, kReadOnly | kFast, 1, -1, 1});
    table.add({"MSET", cmdMSet, -3, kWrite | kDenyOom, 1, -1, 2});
    table.add({"MSETNX", cmdMSetNx, -3, kWrite | kDenyOom, 1, -1, 2});
    table.add({"INCR", cmdIncr, 2, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"DECR", cmdDecr, 2, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"INCRBY", cmdIncrBy, 3, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"DECRBY", cmdDecrBy, 3, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"INCRBYFLOAT", cmdIncrByFloat, 3, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"APPEND", cmdAppend, 3, kWrite | kDenyOom, 1, 1, 1});
    table.add({"STRLEN", cmdStrLen, 2, kReadOnly | kFast, 1, 1, 1});
    table.add({"GETRANGE", cmdGetRange, 4, kReadOnly, 1, 1, 1});
    table.add({"SETRANGE", cmdSetRange, 4, kWrite | kDenyOom, 1, 1, 1});
}

} // namespace miniredis
