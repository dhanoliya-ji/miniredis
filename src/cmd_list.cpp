// List commands.
//
// The list is a std::deque, so push and pop at either end are O(1) and index
// access is O(1) too. Redis uses a quicklist -- a linked list of compressed
// array nodes -- which trades that O(1) index for much lower memory on long
// lists. The deque is the right call here: it keeps LINDEX honest and the
// memory difference only matters at a scale this project does not target.
#include "command_helpers.hpp"

#include <algorithm>

namespace miniredis {

using namespace detail;

namespace {

void pushElements(CommandContext& ctx, bool atHead, bool requireExisting) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);

    if (requireExisting) {
        // LPUSHX / RPUSHX only extend a list that already exists; they never
        // create one.
        Object* existing = keyspace.lookupWrite(ctx.args[1], ctx.now);
        if (existing == nullptr) {
            ctx.reply().integer(0);
            ctx.suppressPropagation();
            return;
        }
        if (existing->type() != ObjectType::List) {
            ctx.reply().error(err::kWrongType);
            return;
        }
    }

    Object* value = openForWrite(ctx, ctx.args[1], ObjectType::List);
    if (value == nullptr) return;

    ListValue& list = value->list();
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (atHead) list.push_front(ctx.args[i]);
        else list.push_back(ctx.args[i]);
    }

    touchKey(ctx, ctx.args[1], static_cast<std::int64_t>(ctx.args.size() - 2));
    ctx.reply().integer(static_cast<std::int64_t>(list.size()));
}

void popElements(CommandContext& ctx, bool fromHead) {
    // The optional count argument turns LPOP into a bulk operation, and
    // changes the reply shape from a bulk string to an array.
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
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::List, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        if (hasCount) ctx.reply().nullArray();
        else ctx.reply().nullBulkString();
        ctx.suppressPropagation();
        return;
    }

    ListValue& list = value->list();
    const std::int64_t taking = std::min<std::int64_t>(requested, static_cast<std::int64_t>(list.size()));

    if (taking == 0) {
        if (hasCount) ctx.reply().arrayHeader(0);
        else ctx.reply().nullBulkString();
        ctx.suppressPropagation();
        return;
    }

    std::vector<Bytes> popped;
    popped.reserve(static_cast<size_t>(taking));
    for (std::int64_t i = 0; i < taking; ++i) {
        if (fromHead) {
            popped.push_back(std::move(list.front()));
            list.pop_front();
        } else {
            popped.push_back(std::move(list.back()));
            list.pop_back();
        }
    }

    RespWriter writer = ctx.reply();
    if (hasCount) {
        writer.arrayHeader(taking);
        for (const auto& item : popped) writer.bulkString(item);
    } else {
        writer.bulkString(popped.front());
    }

    touchKey(ctx, ctx.args[1], taking);
    dropIfEmpty(ctx, ctx.args[1]);
}

void cmdLLen(CommandContext& ctx) {
    Object* value = nullptr;
    switch (lookupTyped(ctx, ctx.args[1], ObjectType::List, value)) {
        case LookupOutcome::Found:   ctx.reply().integer(static_cast<std::int64_t>(value->list().size())); break;
        case LookupOutcome::Missing: ctx.reply().integer(0); break;
        case LookupOutcome::WrongType: break;
    }
}

void cmdLRange(CommandContext& ctx) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!readInt(ctx, ctx.args[2], start)) return;
    if (!readInt(ctx, ctx.args[3], stop)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::List, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().arrayHeader(0);
        return;
    }

    const ListValue& list = value->list();
    std::int64_t from = 0;
    std::int64_t to = 0;
    if (!normaliseRange(start, stop, static_cast<std::int64_t>(list.size()), from, to)) {
        ctx.reply().arrayHeader(0);
        return;
    }

    RespWriter writer = ctx.reply();
    writer.arrayHeader(to - from + 1);
    for (std::int64_t i = from; i <= to; ++i) {
        writer.bulkString(list[static_cast<size_t>(i)]);
    }
}

void cmdLIndex(CommandContext& ctx) {
    std::int64_t index = 0;
    if (!readInt(ctx, ctx.args[2], index)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::List, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().nullBulkString();
        return;
    }

    const ListValue& list = value->list();
    if (index < 0) index += static_cast<std::int64_t>(list.size());
    if (index < 0 || index >= static_cast<std::int64_t>(list.size())) {
        ctx.reply().nullBulkString();
        return;
    }
    ctx.reply().bulkString(list[static_cast<size_t>(index)]);
}

void cmdLSet(CommandContext& ctx) {
    std::int64_t index = 0;
    if (!readInt(ctx, ctx.args[2], index)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::List, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().error(err::kNoSuchKey);
        ctx.suppressPropagation();
        return;
    }

    ListValue& list = value->list();
    if (index < 0) index += static_cast<std::int64_t>(list.size());
    if (index < 0 || index >= static_cast<std::int64_t>(list.size())) {
        ctx.reply().error(err::kIndexOutOfRange);
        ctx.suppressPropagation();
        return;
    }

    list[static_cast<size_t>(index)] = ctx.args[3];
    touchKey(ctx, ctx.args[1]);
    ctx.reply().ok();
}

void cmdLRem(CommandContext& ctx) {
    std::int64_t count = 0;
    if (!readInt(ctx, ctx.args[2], count)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::List, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    ListValue& list = value->list();
    const Bytes& target = ctx.args[3];
    std::int64_t removed = 0;

    // count > 0 removes from the head, count < 0 from the tail, count == 0
    // removes every occurrence.
    if (count >= 0) {
        const std::int64_t limit = (count == 0) ? static_cast<std::int64_t>(list.size()) : count;
        for (auto it = list.begin(); it != list.end() && removed < limit;) {
            if (*it == target) {
                it = list.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    } else {
        const std::int64_t limit = -count;
        for (auto it = list.rbegin(); it != list.rend() && removed < limit;) {
            if (*it == target) {
                // Converting a reverse iterator to a forward one needs the
                // base() minus one dance; erase returns a forward iterator, so
                // the reverse iterator is rebuilt from it.
                it = std::reverse_iterator(list.erase(std::next(it).base()));
                ++removed;
            } else {
                ++it;
            }
        }
    }

    if (removed == 0) ctx.suppressPropagation();
    else touchKey(ctx, ctx.args[1], removed);

    ctx.reply().integer(removed);
    dropIfEmpty(ctx, ctx.args[1]);
}

void cmdLTrim(CommandContext& ctx) {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!readInt(ctx, ctx.args[2], start)) return;
    if (!readInt(ctx, ctx.args[3], stop)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::List, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().ok();
        ctx.suppressPropagation();
        return;
    }

    ListValue& list = value->list();
    std::int64_t from = 0;
    std::int64_t to = 0;

    if (!normaliseRange(start, stop, static_cast<std::int64_t>(list.size()), from, to)) {
        // An empty range means "keep nothing", which deletes the key.
        list.clear();
        touchKey(ctx, ctx.args[1]);
        dropIfEmpty(ctx, ctx.args[1]);
        ctx.reply().ok();
        return;
    }

    list.erase(list.begin() + static_cast<long>(to) + 1, list.end());
    list.erase(list.begin(), list.begin() + static_cast<long>(from));

    touchKey(ctx, ctx.args[1]);
    dropIfEmpty(ctx, ctx.args[1]);
    ctx.reply().ok();
}

void cmdLInsert(CommandContext& ctx) {
    const bool before = equalsIgnoreCase(ctx.args[2], "BEFORE");
    if (!before && !equalsIgnoreCase(ctx.args[2], "AFTER")) {
        ctx.reply().error(err::kSyntax);
        return;
    }

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::List, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    ListValue& list = value->list();
    const auto pivot = std::find(list.begin(), list.end(), ctx.args[3]);
    if (pivot == list.end()) {
        // -1 distinguishes "pivot not found" from "key not found" (0).
        ctx.reply().integer(-1);
        ctx.suppressPropagation();
        return;
    }

    list.insert(before ? pivot : std::next(pivot), ctx.args[4]);
    touchKey(ctx, ctx.args[1]);
    ctx.reply().integer(static_cast<std::int64_t>(list.size()));
}

// LMOVE and the older RPOPLPUSH share this body. Moving within one key is the
// idiomatic way to rotate a list, so source == destination must work.
void moveElement(CommandContext& ctx, const Bytes& sourceKey, const Bytes& destinationKey,
                 bool fromHead, bool toHead) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);

    Object* source = keyspace.lookupWrite(sourceKey, ctx.now);
    if (source == nullptr) {
        ctx.reply().nullBulkString();
        ctx.suppressPropagation();
        return;
    }
    if (source->type() != ObjectType::List) {
        ctx.reply().error(err::kWrongType);
        return;
    }
    if (source->list().empty()) {
        ctx.reply().nullBulkString();
        ctx.suppressPropagation();
        return;
    }

    Object* destination = keyspace.lookupWrite(destinationKey, ctx.now);
    if (destination != nullptr && destination->type() != ObjectType::List) {
        ctx.reply().error(err::kWrongType);
        return;
    }

    // Take the element out first, then look the destination up again. Creating
    // the destination can rehash the dictionary and invalidate `source`.
    Bytes element;
    if (fromHead) {
        element = std::move(source->list().front());
        source->list().pop_front();
    } else {
        element = std::move(source->list().back());
        source->list().pop_back();
    }

    Object* target = keyspace.getOrCreate(destinationKey, ObjectType::List, ctx.now);
    if (target == nullptr) {
        // Cannot happen: the type was checked above, and a missing key is
        // created as a list. Restoring the element keeps the failure safe.
        Object* restored = keyspace.lookupWrite(sourceKey, ctx.now);
        if (restored != nullptr && restored->type() == ObjectType::List) {
            if (fromHead) restored->list().push_front(std::move(element));
            else restored->list().push_back(std::move(element));
        }
        ctx.reply().error(err::kWrongType);
        return;
    }

    if (toHead) target->list().push_front(element);
    else target->list().push_back(element);

    ctx.reply().bulkString(element);

    touchKey(ctx, sourceKey);
    touchKey(ctx, destinationKey);
    dropIfEmpty(ctx, sourceKey);
}

void cmdLMove(CommandContext& ctx) {
    const bool fromHead = equalsIgnoreCase(ctx.args[3], "LEFT");
    const bool toHead = equalsIgnoreCase(ctx.args[4], "LEFT");

    if ((!fromHead && !equalsIgnoreCase(ctx.args[3], "RIGHT")) ||
        (!toHead && !equalsIgnoreCase(ctx.args[4], "RIGHT"))) {
        ctx.reply().error(err::kSyntax);
        return;
    }
    moveElement(ctx, ctx.args[1], ctx.args[2], fromHead, toHead);
}

void cmdRPopLPush(CommandContext& ctx) {
    moveElement(ctx, ctx.args[1], ctx.args[2], /*fromHead=*/false, /*toHead=*/true);
}

void cmdLPos(CommandContext& ctx) {
    std::int64_t rank = 1;
    std::int64_t count = -1; // -1 means "no COUNT given": return a single index
    std::int64_t maxComparisons = 0;

    for (size_t i = 3; i < ctx.args.size(); ++i) {
        if (equalsIgnoreCase(ctx.args[i], "RANK") && i + 1 < ctx.args.size()) {
            if (!readInt(ctx, ctx.args[++i], rank)) return;
            if (rank == 0) {
                ctx.reply().error("ERR RANK can't be zero");
                return;
            }
        } else if (equalsIgnoreCase(ctx.args[i], "COUNT") && i + 1 < ctx.args.size()) {
            if (!readInt(ctx, ctx.args[++i], count)) return;
            if (count < 0) {
                ctx.reply().error("ERR COUNT can't be negative");
                return;
            }
        } else if (equalsIgnoreCase(ctx.args[i], "MAXLEN") && i + 1 < ctx.args.size()) {
            if (!readInt(ctx, ctx.args[++i], maxComparisons)) return;
            if (maxComparisons < 0) {
                ctx.reply().error("ERR MAXLEN can't be negative");
                return;
            }
        } else {
            ctx.reply().error(err::kSyntax);
            return;
        }
    }

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::List, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        if (count >= 0) ctx.reply().arrayHeader(0);
        else ctx.reply().nullBulkString();
        return;
    }

    const ListValue& list = value->list();
    const std::int64_t length = static_cast<std::int64_t>(list.size());
    const bool backwards = rank < 0;
    const std::int64_t skip = (backwards ? -rank : rank) - 1;

    std::vector<std::int64_t> found;
    std::int64_t matches = 0;
    std::int64_t compared = 0;

    for (std::int64_t step = 0; step < length; ++step) {
        const std::int64_t index = backwards ? length - 1 - step : step;
        if (maxComparisons > 0 && ++compared > maxComparisons) break;

        if (list[static_cast<size_t>(index)] != ctx.args[2]) continue;
        if (matches++ < skip) continue;

        found.push_back(index);
        if (count == 0) continue;          // COUNT 0 means "all matches"
        if (count < 0) break;              // no COUNT: first match only
        if (static_cast<std::int64_t>(found.size()) >= count) break;
    }

    RespWriter writer = ctx.reply();
    if (count < 0) {
        if (found.empty()) writer.nullBulkString();
        else writer.integer(found.front());
        return;
    }

    writer.arrayHeader(static_cast<std::int64_t>(found.size()));
    for (const std::int64_t index : found) writer.integer(index);
}

} // namespace

void registerListCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"LPUSH", [](CommandContext& c) { pushElements(c, true, false); }, -3, kWrite | kDenyOom, 1, 1, 1});
    table.add({"RPUSH", [](CommandContext& c) { pushElements(c, false, false); }, -3, kWrite | kDenyOom, 1, 1, 1});
    table.add({"LPUSHX", [](CommandContext& c) { pushElements(c, true, true); }, -3, kWrite | kDenyOom, 1, 1, 1});
    table.add({"RPUSHX", [](CommandContext& c) { pushElements(c, false, true); }, -3, kWrite | kDenyOom, 1, 1, 1});
    table.add({"LPOP", [](CommandContext& c) { popElements(c, true); }, -2, kWrite | kFast, 1, 1, 1});
    table.add({"RPOP", [](CommandContext& c) { popElements(c, false); }, -2, kWrite | kFast, 1, 1, 1});

    table.add({"LLEN", cmdLLen, 2, kReadOnly | kFast, 1, 1, 1});
    table.add({"LRANGE", cmdLRange, 4, kReadOnly, 1, 1, 1});
    table.add({"LINDEX", cmdLIndex, 3, kReadOnly, 1, 1, 1});
    table.add({"LSET", cmdLSet, 4, kWrite | kDenyOom, 1, 1, 1});
    table.add({"LREM", cmdLRem, 4, kWrite, 1, 1, 1});
    table.add({"LTRIM", cmdLTrim, 4, kWrite, 1, 1, 1});
    table.add({"LINSERT", cmdLInsert, 5, kWrite | kDenyOom, 1, 1, 1});
    table.add({"LPOS", cmdLPos, -3, kReadOnly, 1, 1, 1});
    table.add({"LMOVE", cmdLMove, 5, kWrite | kDenyOom, 1, 2, 1});
    table.add({"RPOPLPUSH", cmdRPopLPush, 3, kWrite | kDenyOom, 1, 2, 1});
}

} // namespace miniredis
