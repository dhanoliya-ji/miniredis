// Sorted set commands.
//
// The sorted set is the type that makes Redis useful as more than a cache:
// leaderboards, rate limiters, priority queues and time-series indexes are all
// "keep things in score order and slice by score or rank".
#include "command_helpers.hpp"

#include <algorithm>
#include <cmath>

namespace miniredis {

using namespace detail;

namespace {

// A score bound in ZRANGEBYSCORE syntax: "5", "(5" for exclusive, and
// "-inf"/"+inf" for the open ends.
struct ScoreBound {
    double value = 0;
    bool exclusive = false;
};

bool parseScoreBound(const Bytes& text, ScoreBound& bound) {
    std::string_view view(text);
    if (!view.empty() && view.front() == '(') {
        bound.exclusive = true;
        view.remove_prefix(1);
    }
    return parseDouble(view, bound.value);
}

void cmdZAdd(CommandContext& ctx) {
    size_t cursor = 2;
    bool onlyIfAbsent = false;   // NX
    bool onlyIfPresent = false;  // XX
    bool onlyIfGreater = false;  // GT
    bool onlyIfLess = false;     // LT
    bool returnChanged = false;  // CH
    bool incrementMode = false;  // INCR

    while (cursor < ctx.args.size()) {
        const Bytes& token = ctx.args[cursor];
        if (equalsIgnoreCase(token, "NX"))      onlyIfAbsent = true;
        else if (equalsIgnoreCase(token, "XX")) onlyIfPresent = true;
        else if (equalsIgnoreCase(token, "GT")) onlyIfGreater = true;
        else if (equalsIgnoreCase(token, "LT")) onlyIfLess = true;
        else if (equalsIgnoreCase(token, "CH")) returnChanged = true;
        else if (equalsIgnoreCase(token, "INCR")) incrementMode = true;
        else break;
        ++cursor;
    }

    if (onlyIfAbsent && (onlyIfPresent || onlyIfGreater || onlyIfLess)) {
        ctx.reply().error("ERR GT, LT, and/or NX options at the same time are not compatible");
        return;
    }
    if (onlyIfGreater && onlyIfLess) {
        ctx.reply().error("ERR GT, LT, and/or NX options at the same time are not compatible");
        return;
    }

    const size_t remaining = ctx.args.size() - cursor;
    if (remaining == 0 || remaining % 2 != 0) {
        ctx.reply().error(err::kSyntax);
        return;
    }
    if (incrementMode && remaining != 2) {
        ctx.reply().error("ERR INCR option supports a single increment-element pair");
        return;
    }

    // Every score is validated before anything is written, so a malformed
    // score in the middle of a batch cannot leave the set half updated.
    std::vector<std::pair<double, const Bytes*>> pending;
    pending.reserve(remaining / 2);
    for (size_t i = cursor; i + 1 < ctx.args.size(); i += 2) {
        double score = 0;
        if (!parseDouble(ctx.args[i], score)) {
            ctx.reply().error(err::kNotFloat);
            return;
        }
        pending.emplace_back(score, &ctx.args[i + 1]);
    }

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    if (onlyIfPresent && !keyspace.exists(ctx.args[1], ctx.now)) {
        if (incrementMode) ctx.reply().nullBulkString();
        else ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    Object* value = openForWrite(ctx, ctx.args[1], ObjectType::ZSet);
    if (value == nullptr) return;

    ZSet& zset = value->zset();
    std::int64_t added = 0;
    std::int64_t changed = 0;
    double incrementResult = 0;
    bool incrementApplied = false;

    for (const auto& [score, memberPtr] : pending) {
        const Bytes& member = *memberPtr;
        double existing = 0;
        const bool present = zset.score(member, existing);

        if (onlyIfAbsent && present) continue;
        if (onlyIfPresent && !present) continue;

        double target = score;
        if (incrementMode) {
            target = present ? existing + score : score;
            if (std::isnan(target)) {
                ctx.reply().error("ERR resulting score is not a number (NaN)");
                dropIfEmpty(ctx, ctx.args[1]);
                return;
            }
        }

        if (present) {
            if (onlyIfGreater && target <= existing) continue;
            if (onlyIfLess && target >= existing) continue;
        }

        if (zset.add(member, target)) ++added;
        else if (target != existing) ++changed;

        incrementResult = target;
        incrementApplied = true;
    }

    const std::int64_t total = added + changed;
    if (total == 0) {
        ctx.suppressPropagation();
        dropIfEmpty(ctx, ctx.args[1]);
    } else {
        touchKey(ctx, ctx.args[1], total);
    }

    if (incrementMode) {
        if (!incrementApplied) ctx.reply().nullBulkString();
        else ctx.reply().bulkString(formatDouble(incrementResult));
        // The result depends on the current score, so the computed value is
        // propagated rather than the increment.
        if (incrementApplied) {
            ctx.propagateInstead(Args{"ZADD", ctx.args[1], formatDouble(incrementResult),
                                      *pending.front().second});
        }
        return;
    }

    ctx.reply().integer(returnChanged ? total : added);
}

void cmdZRem(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    std::int64_t removed = 0;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (value->zset().remove(ctx.args[i])) ++removed;
    }

    if (removed == 0) ctx.suppressPropagation();
    else touchKey(ctx, ctx.args[1], removed);

    ctx.reply().integer(removed);
    dropIfEmpty(ctx, ctx.args[1]);
}

void cmdZScore(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value);
    if (outcome == LookupOutcome::WrongType) return;

    double score = 0;
    if (outcome == LookupOutcome::Missing || !value->zset().score(ctx.args[2], score)) {
        ctx.reply().nullBulkString();
        return;
    }
    ctx.reply().bulkString(formatDouble(score));
}

void cmdZMScore(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value);
    if (outcome == LookupOutcome::WrongType) return;

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(ctx.args.size() - 2));
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        double score = 0;
        if (outcome == LookupOutcome::Missing || !value->zset().score(ctx.args[i], score)) {
            writer.nullBulkString();
        } else {
            writer.bulkString(formatDouble(score));
        }
    }
}

void cmdZIncrBy(CommandContext& ctx) {
    double delta = 0;
    if (!readDouble(ctx, ctx.args[2], delta)) return;

    Object* value = openForWrite(ctx, ctx.args[1], ObjectType::ZSet);
    if (value == nullptr) return;

    double current = 0;
    value->zset().score(ctx.args[3], current);
    const double updated = current + delta;

    if (std::isnan(updated)) {
        ctx.reply().error("ERR resulting score is not a number (NaN)");
        dropIfEmpty(ctx, ctx.args[1]);
        return;
    }

    value->zset().add(ctx.args[3], updated);
    touchKey(ctx, ctx.args[1]);

    const std::string rendered = formatDouble(updated);
    ctx.reply().bulkString(rendered);
    ctx.propagateInstead(Args{"ZADD", ctx.args[1], rendered, ctx.args[3]});
}

void cmdZCard(CommandContext& ctx) {
    Object* value = nullptr;
    switch (lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value)) {
        case LookupOutcome::Found:   ctx.reply().integer(static_cast<std::int64_t>(value->zset().size())); break;
        case LookupOutcome::Missing: ctx.reply().integer(0); break;
        case LookupOutcome::WrongType: break;
    }
}

void cmdZCount(CommandContext& ctx) {
    ScoreBound low;
    ScoreBound high;
    if (!parseScoreBound(ctx.args[2], low) || !parseScoreBound(ctx.args[3], high)) {
        ctx.reply().error("ERR min or max is not a float");
        return;
    }

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        return;
    }

    const auto matches = value->zset().rangeByScore(low.value, low.exclusive, high.value, high.exclusive);
    ctx.reply().integer(static_cast<std::int64_t>(matches.size()));
}

void emitRangeByRank(CommandContext& ctx, bool reverse, bool withScores) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!readInt(ctx, ctx.args[2], start)) return;
    if (!readInt(ctx, ctx.args[3], stop)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().arrayHeader(0);
        return;
    }

    std::vector<ZSet::Entry> entries(value->zset().ordered().begin(), value->zset().ordered().end());
    if (reverse) std::reverse(entries.begin(), entries.end());

    std::int64_t from = 0;
    std::int64_t to = 0;
    if (!normaliseRange(start, stop, static_cast<std::int64_t>(entries.size()), from, to)) {
        ctx.reply().arrayHeader(0);
        return;
    }

    const std::int64_t count = to - from + 1;
    RespWriter writer = ctx.reply();
    writer.arrayHeader(withScores ? count * 2 : count);

    for (std::int64_t i = from; i <= to; ++i) {
        const auto& entry = entries[static_cast<size_t>(i)];
        writer.bulkString(entry.second);
        if (withScores) writer.bulkString(formatDouble(entry.first));
    }
}

void emitRangeByScore(CommandContext& ctx, bool reverse) {
    // ZREVRANGEBYSCORE takes its bounds in the opposite order, which is easy
    // to get wrong, so the swap happens once here.
    const Bytes& lowText = reverse ? ctx.args[3] : ctx.args[2];
    const Bytes& highText = reverse ? ctx.args[2] : ctx.args[3];

    ScoreBound low;
    ScoreBound high;
    if (!parseScoreBound(lowText, low) || !parseScoreBound(highText, high)) {
        ctx.reply().error("ERR min or max is not a float");
        return;
    }

    bool withScores = false;
    std::int64_t offset = 0;
    std::int64_t limit = -1;

    for (size_t i = 4; i < ctx.args.size(); ++i) {
        if (equalsIgnoreCase(ctx.args[i], "WITHSCORES")) {
            withScores = true;
        } else if (equalsIgnoreCase(ctx.args[i], "LIMIT") && i + 2 < ctx.args.size()) {
            if (!readInt(ctx, ctx.args[i + 1], offset)) return;
            if (!readInt(ctx, ctx.args[i + 2], limit)) return;
            i += 2;
        } else {
            ctx.reply().error(err::kSyntax);
            return;
        }
    }

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().arrayHeader(0);
        return;
    }

    std::vector<ZSet::Entry> matches =
        value->zset().rangeByScore(low.value, low.exclusive, high.value, high.exclusive);
    if (reverse) std::reverse(matches.begin(), matches.end());

    if (offset < 0) offset = 0;
    if (offset >= static_cast<std::int64_t>(matches.size())) {
        ctx.reply().arrayHeader(0);
        return;
    }
    matches.erase(matches.begin(), matches.begin() + static_cast<long>(offset));
    if (limit >= 0 && static_cast<std::int64_t>(matches.size()) > limit) {
        matches.resize(static_cast<size_t>(limit));
    }

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(matches.size()) * (withScores ? 2 : 1));
    for (const auto& entry : matches) {
        writer.bulkString(entry.second);
        if (withScores) writer.bulkString(formatDouble(entry.first));
    }
}

void emitRank(CommandContext& ctx, bool reverse) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().nullBulkString();
        return;
    }

    const std::int64_t rank = value->zset().rank(ctx.args[2], reverse);
    if (rank < 0) ctx.reply().nullBulkString();
    else ctx.reply().integer(rank);
}

void cmdZRemRangeByScore(CommandContext& ctx) {
    ScoreBound low;
    ScoreBound high;
    if (!parseScoreBound(ctx.args[2], low) || !parseScoreBound(ctx.args[3], high)) {
        ctx.reply().error("ERR min or max is not a float");
        return;
    }

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    // The matches are collected before anything is removed, because removing
    // while iterating the ordered index would invalidate the iterator.
    const auto doomed = value->zset().rangeByScore(low.value, low.exclusive, high.value, high.exclusive);
    for (const auto& entry : doomed) value->zset().remove(entry.second);

    const std::int64_t removed = static_cast<std::int64_t>(doomed.size());
    if (removed == 0) ctx.suppressPropagation();
    else touchKey(ctx, ctx.args[1], removed);

    ctx.reply().integer(removed);
    dropIfEmpty(ctx, ctx.args[1]);
}

void cmdZRemRangeByRank(CommandContext& ctx) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!readInt(ctx, ctx.args[2], start)) return;
    if (!readInt(ctx, ctx.args[3], stop)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    std::vector<ZSet::Entry> entries(value->zset().ordered().begin(), value->zset().ordered().end());
    std::int64_t from = 0;
    std::int64_t to = 0;
    if (!normaliseRange(start, stop, static_cast<std::int64_t>(entries.size()), from, to)) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    for (std::int64_t i = from; i <= to; ++i) {
        value->zset().remove(entries[static_cast<size_t>(i)].second);
    }

    const std::int64_t removed = to - from + 1;
    touchKey(ctx, ctx.args[1], removed);
    ctx.reply().integer(removed);
    dropIfEmpty(ctx, ctx.args[1]);
}

void popExtreme(CommandContext& ctx, bool lowest) {
    std::int64_t requested = 1;
    const bool hasCount = ctx.args.size() == 3;
    if (hasCount) {
        if (!readInt(ctx, ctx.args[2], requested)) return;
        if (requested < 0) {
            ctx.reply().error(err::kOutOfRange);
            return;
        }
    }

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::ZSet, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().arrayHeader(0);
        ctx.suppressPropagation();
        return;
    }

    std::vector<ZSet::Entry> entries(value->zset().ordered().begin(), value->zset().ordered().end());
    if (!lowest) std::reverse(entries.begin(), entries.end());

    const std::int64_t taking = std::min<std::int64_t>(requested, static_cast<std::int64_t>(entries.size()));
    entries.resize(static_cast<size_t>(taking));

    for (const auto& entry : entries) value->zset().remove(entry.second);

    RespWriter writer = ctx.reply();
    writer.arrayHeader(taking * 2);
    for (const auto& entry : entries) {
        writer.bulkString(entry.second);
        writer.bulkString(formatDouble(entry.first));
    }

    if (taking == 0) {
        ctx.suppressPropagation();
    } else {
        touchKey(ctx, ctx.args[1], taking);
        // Which member is "the lowest" depends on the current contents, so the
        // members actually removed are propagated rather than the pop.
        Args rewritten{"ZREM", ctx.args[1]};
        for (const auto& entry : entries) rewritten.push_back(entry.second);
        ctx.propagateInstead(std::move(rewritten));
    }
    dropIfEmpty(ctx, ctx.args[1]);
}

} // namespace

void registerZSetCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"ZADD", cmdZAdd, -4, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"ZREM", cmdZRem, -3, kWrite | kFast, 1, 1, 1});
    table.add({"ZSCORE", cmdZScore, 3, kReadOnly | kFast, 1, 1, 1});
    table.add({"ZMSCORE", cmdZMScore, -3, kReadOnly | kFast, 1, 1, 1});
    table.add({"ZINCRBY", cmdZIncrBy, 4, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"ZCARD", cmdZCard, 2, kReadOnly | kFast, 1, 1, 1});
    table.add({"ZCOUNT", cmdZCount, 4, kReadOnly | kFast, 1, 1, 1});

    table.add({"ZRANGE", [](CommandContext& c) {
        const bool withScores = c.args.size() == 5 && equalsIgnoreCase(c.args[4], "WITHSCORES");
        if (c.args.size() > 5 || (c.args.size() == 5 && !withScores)) {
            c.reply().error(err::kSyntax);
            return;
        }
        emitRangeByRank(c, false, withScores);
    }, -4, kReadOnly, 1, 1, 1});

    table.add({"ZREVRANGE", [](CommandContext& c) {
        const bool withScores = c.args.size() == 5 && equalsIgnoreCase(c.args[4], "WITHSCORES");
        if (c.args.size() > 5 || (c.args.size() == 5 && !withScores)) {
            c.reply().error(err::kSyntax);
            return;
        }
        emitRangeByRank(c, true, withScores);
    }, -4, kReadOnly, 1, 1, 1});

    table.add({"ZRANGEBYSCORE", [](CommandContext& c) { emitRangeByScore(c, false); }, -4, kReadOnly, 1, 1, 1});
    table.add({"ZREVRANGEBYSCORE", [](CommandContext& c) { emitRangeByScore(c, true); }, -4, kReadOnly, 1, 1, 1});

    table.add({"ZRANK", [](CommandContext& c) { emitRank(c, false); }, 3, kReadOnly | kFast, 1, 1, 1});
    table.add({"ZREVRANK", [](CommandContext& c) { emitRank(c, true); }, 3, kReadOnly | kFast, 1, 1, 1});

    table.add({"ZREMRANGEBYSCORE", cmdZRemRangeByScore, 4, kWrite, 1, 1, 1});
    table.add({"ZREMRANGEBYRANK", cmdZRemRangeByRank, 4, kWrite, 1, 1, 1});

    table.add({"ZPOPMIN", [](CommandContext& c) { popExtreme(c, true); }, -2, kWrite | kFast, 1, 1, 1});
    table.add({"ZPOPMAX", [](CommandContext& c) { popExtreme(c, false); }, -2, kWrite | kFast, 1, 1, 1});
}

} // namespace miniredis
