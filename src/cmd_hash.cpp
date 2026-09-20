// Hash commands: a dictionary stored inside a single key.
//
// A hash is what you reach for when a value has fields that are read and
// written independently -- a user record where only last_login changes. The
// alternative, serialising a struct into a string, forces a read-modify-write
// of the whole value for every field update and makes concurrent updates to
// different fields clobber each other.
#include "command_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace miniredis {

using namespace detail;

namespace {

void cmdHSet(CommandContext& ctx) {
    if ((ctx.args.size() - 2) % 2 != 0) {
        ctx.reply().error(wrongArgsError(ctx.args[0]));
        return;
    }

    Object* value = openForWrite(ctx, ctx.args[1], ObjectType::Hash);
    if (value == nullptr) return;

    HashValue& hash = value->hash();
    std::int64_t added = 0;

    for (size_t i = 2; i + 1 < ctx.args.size(); i += 2) {
        // The return value counts fields *created*, not fields written, so an
        // overwrite contributes nothing.
        if (hash.insert_or_assign(ctx.args[i], ctx.args[i + 1]).second) ++added;
    }

    touchKey(ctx, ctx.args[1], static_cast<std::int64_t>((ctx.args.size() - 2) / 2));
    ctx.reply().integer(added);
}

void cmdHSetNx(CommandContext& ctx) {
    Object* value = openForWrite(ctx, ctx.args[1], ObjectType::Hash);
    if (value == nullptr) return;

    HashValue& hash = value->hash();
    if (hash.find(ctx.args[2]) != hash.end()) {
        ctx.reply().integer(0);
        // The key may have just been created empty by openForWrite; drop it
        // again so a failed HSETNX does not leave a phantom key behind.
        dropIfEmpty(ctx, ctx.args[1]);
        ctx.suppressPropagation();
        return;
    }

    hash.emplace(ctx.args[2], ctx.args[3]);
    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(1);
}

void cmdHGet(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Hash, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().nullBulkString();
        return;
    }

    const auto it = value->hash().find(ctx.args[2]);
    if (it == value->hash().end()) ctx.reply().nullBulkString();
    else ctx.reply().bulkString(it->second);
}

void cmdHMGet(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Hash, value);
    if (outcome == LookupOutcome::WrongType) return;

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(ctx.args.size() - 2));

    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (outcome == LookupOutcome::Missing) {
            writer.nullBulkString();
            continue;
        }
        const auto it = value->hash().find(ctx.args[i]);
        if (it == value->hash().end()) writer.nullBulkString();
        else writer.bulkString(it->second);
    }
}

void cmdHDel(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Hash, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    std::int64_t removed = 0;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        removed += static_cast<std::int64_t>(value->hash().erase(ctx.args[i]));
    }

    if (removed == 0) ctx.suppressPropagation();
    else touchKey(ctx, ctx.args[1], removed);

    ctx.reply().integer(removed);
    // A hash that has lost every field is deleted, so HLEN and EXISTS agree.
    dropIfEmpty(ctx, ctx.args[1]);
}

void cmdHExists(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Hash, value);
    if (outcome == LookupOutcome::WrongType) return;

    const bool present = (outcome == LookupOutcome::Found) &&
                         value->hash().find(ctx.args[2]) != value->hash().end();
    ctx.reply().integer(present ? 1 : 0);
}

void cmdHLen(CommandContext& ctx) {
    Object* value = nullptr;
    switch (lookupTyped(ctx, ctx.args[1], ObjectType::Hash, value)) {
        case LookupOutcome::Found:   ctx.reply().integer(static_cast<std::int64_t>(value->hash().size())); break;
        case LookupOutcome::Missing: ctx.reply().integer(0); break;
        case LookupOutcome::WrongType: break;
    }
}

void cmdHStrLen(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Hash, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        return;
    }

    const auto it = value->hash().find(ctx.args[2]);
    ctx.reply().integer(it == value->hash().end() ? 0 : static_cast<std::int64_t>(it->second.size()));
}

enum class HashProjection { Keys, Values, Both };

void emitHash(CommandContext& ctx, HashProjection projection) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Hash, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().arrayHeader(0);
        return;
    }

    const HashValue& hash = value->hash();
    const std::int64_t elements = static_cast<std::int64_t>(hash.size()) *
                                  (projection == HashProjection::Both ? 2 : 1);

    RespWriter writer = ctx.reply();
    writer.arrayHeader(elements);
    for (const auto& [field, fieldValue] : hash) {
        if (projection != HashProjection::Values) writer.bulkString(field);
        if (projection != HashProjection::Keys) writer.bulkString(fieldValue);
    }
}

void cmdHIncrBy(CommandContext& ctx) {
    std::int64_t delta = 0;
    if (!readInt(ctx, ctx.args[3], delta)) return;

    Object* value = openForWrite(ctx, ctx.args[1], ObjectType::Hash);
    if (value == nullptr) return;

    HashValue& hash = value->hash();
    std::int64_t current = 0;

    const auto it = hash.find(ctx.args[2]);
    if (it != hash.end() && !parseInt64(it->second, current)) {
        ctx.reply().error("ERR hash value is not an integer");
        dropIfEmpty(ctx, ctx.args[1]);
        return;
    }

    if ((delta > 0 && current > INT64_MAX - delta) || (delta < 0 && current < INT64_MIN - delta)) {
        ctx.reply().error("ERR increment or decrement would overflow");
        dropIfEmpty(ctx, ctx.args[1]);
        return;
    }

    const std::int64_t updated = current + delta;
    hash.insert_or_assign(ctx.args[2], formatInt64(updated));
    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(updated);
}

void cmdHIncrByFloat(CommandContext& ctx) {
    double delta = 0;
    if (!readDouble(ctx, ctx.args[3], delta)) return;

    Object* value = openForWrite(ctx, ctx.args[1], ObjectType::Hash);
    if (value == nullptr) return;

    HashValue& hash = value->hash();
    double current = 0;

    const auto it = hash.find(ctx.args[2]);
    if (it != hash.end() && !parseDouble(it->second, current)) {
        ctx.reply().error("ERR hash value is not a float");
        dropIfEmpty(ctx, ctx.args[1]);
        return;
    }

    const double updated = current + delta;
    if (std::isnan(updated) || std::isinf(updated)) {
        ctx.reply().error("ERR increment would produce NaN or Infinity");
        dropIfEmpty(ctx, ctx.args[1]);
        return;
    }

    const std::string rendered = formatDouble(updated);
    hash.insert_or_assign(ctx.args[2], rendered);
    touchKey(ctx, ctx.args[1]);
    ctx.reply().bulkString(rendered);

    // As with INCRBYFLOAT, the computed value is propagated rather than the
    // increment, so a replica's last bit cannot differ.
    ctx.propagateInstead(Args{"HSET", ctx.args[1], ctx.args[2], rendered});
}

void cmdHRandField(CommandContext& ctx) {
    std::int64_t requested = 1;
    const bool hasCount = ctx.args.size() >= 3;
    bool withValues = false;

    if (hasCount) {
        if (!readInt(ctx, ctx.args[2], requested)) return;
        if (ctx.args.size() == 4) {
            if (!equalsIgnoreCase(ctx.args[3], "WITHVALUES")) {
                ctx.reply().error(err::kSyntax);
                return;
            }
            withValues = true;
        } else if (ctx.args.size() > 4) {
            ctx.reply().error(err::kSyntax);
            return;
        }
    }

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Hash, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        if (hasCount) ctx.reply().arrayHeader(0);
        else ctx.reply().nullBulkString();
        return;
    }

    std::vector<const std::pair<const Bytes, Bytes>*> fields;
    fields.reserve(value->hash().size());
    for (const auto& entry : value->hash()) fields.push_back(&entry);

    static std::mt19937_64 engine{std::random_device{}()};

    if (!hasCount) {
        const auto* chosen = fields[engine() % fields.size()];
        ctx.reply().bulkString(chosen->first);
        return;
    }

    // A negative count allows repeats and always returns exactly |count|
    // entries; a positive one returns distinct entries, capped at the size.
    std::vector<const std::pair<const Bytes, Bytes>*> chosen;
    if (requested < 0) {
        const size_t wanted = static_cast<size_t>(-requested);
        chosen.reserve(wanted);
        for (size_t i = 0; i < wanted; ++i) {
            chosen.push_back(fields[engine() % fields.size()]);
        }
    } else {
        std::shuffle(fields.begin(), fields.end(), engine);
        const size_t wanted = std::min(static_cast<size_t>(requested), fields.size());
        chosen.assign(fields.begin(), fields.begin() + static_cast<long>(wanted));
    }

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(chosen.size()) * (withValues ? 2 : 1));
    for (const auto* entry : chosen) {
        writer.bulkString(entry->first);
        if (withValues) writer.bulkString(entry->second);
    }
}

} // namespace

void registerHashCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"HSET", cmdHSet, -4, kWrite | kDenyOom, 1, 1, 1});
    // HMSET is HSET's deprecated twin; it differs only in replying +OK.
    table.add({"HMSET", [](CommandContext& c) {
        if ((c.args.size() - 2) % 2 != 0) {
            c.reply().error(wrongArgsError(c.args[0]));
            return;
        }
        Object* value = openForWrite(c, c.args[1], ObjectType::Hash);
        if (value == nullptr) return;
        for (size_t i = 2; i + 1 < c.args.size(); i += 2) {
            value->hash().insert_or_assign(c.args[i], c.args[i + 1]);
        }
        touchKey(c, c.args[1], static_cast<std::int64_t>((c.args.size() - 2) / 2));
        c.reply().ok();
    }, -4, kWrite | kDenyOom, 1, 1, 1});

    table.add({"HSETNX", cmdHSetNx, 4, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"HGET", cmdHGet, 3, kReadOnly | kFast, 1, 1, 1});
    table.add({"HMGET", cmdHMGet, -3, kReadOnly | kFast, 1, 1, 1});
    table.add({"HDEL", cmdHDel, -3, kWrite | kFast, 1, 1, 1});
    table.add({"HEXISTS", cmdHExists, 3, kReadOnly | kFast, 1, 1, 1});
    table.add({"HLEN", cmdHLen, 2, kReadOnly | kFast, 1, 1, 1});
    table.add({"HSTRLEN", cmdHStrLen, 3, kReadOnly | kFast, 1, 1, 1});
    table.add({"HKEYS", [](CommandContext& c) { emitHash(c, HashProjection::Keys); }, 2, kReadOnly, 1, 1, 1});
    table.add({"HVALS", [](CommandContext& c) { emitHash(c, HashProjection::Values); }, 2, kReadOnly, 1, 1, 1});
    table.add({"HGETALL", [](CommandContext& c) { emitHash(c, HashProjection::Both); }, 2, kReadOnly, 1, 1, 1});
    table.add({"HINCRBY", cmdHIncrBy, 4, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"HINCRBYFLOAT", cmdHIncrByFloat, 4, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"HRANDFIELD", cmdHRandField, -2, kReadOnly, 1, 1, 1});
}

} // namespace miniredis
