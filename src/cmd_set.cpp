// Set commands: unordered collections of unique members.
#include "command_helpers.hpp"

#include <algorithm>
#include <random>

namespace miniredis {

using namespace detail;

namespace {

std::mt19937_64& randomEngine() {
    static std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

void cmdSAdd(CommandContext& ctx) {
    Object* value = openForWrite(ctx, ctx.args[1], ObjectType::Set);
    if (value == nullptr) return;

    std::int64_t added = 0;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        if (value->set().insert(ctx.args[i]).second) ++added;
    }

    if (added == 0) {
        // Every member was already present, so nothing changed. Propagating
        // this would grow the log for no reason.
        ctx.suppressPropagation();
        dropIfEmpty(ctx, ctx.args[1]);
    } else {
        touchKey(ctx, ctx.args[1], added);
    }
    ctx.reply().integer(added);
}

void cmdSRem(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Set, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    std::int64_t removed = 0;
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        removed += static_cast<std::int64_t>(value->set().erase(ctx.args[i]));
    }

    if (removed == 0) ctx.suppressPropagation();
    else touchKey(ctx, ctx.args[1], removed);

    ctx.reply().integer(removed);
    dropIfEmpty(ctx, ctx.args[1]);
}

void cmdSMembers(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Set, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        ctx.reply().arrayHeader(0);
        return;
    }

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(value->set().size()));
    for (const auto& member : value->set()) writer.bulkString(member);
}

void cmdSIsMember(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Set, value);
    if (outcome == LookupOutcome::WrongType) return;

    const bool present = (outcome == LookupOutcome::Found) &&
                         value->set().count(ctx.args[2]) > 0;
    ctx.reply().integer(present ? 1 : 0);
}

void cmdSMIsMember(CommandContext& ctx) {
    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Set, value);
    if (outcome == LookupOutcome::WrongType) return;

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(ctx.args.size() - 2));
    for (size_t i = 2; i < ctx.args.size(); ++i) {
        const bool present = (outcome == LookupOutcome::Found) && value->set().count(ctx.args[i]) > 0;
        writer.integer(present ? 1 : 0);
    }
}

void cmdSCard(CommandContext& ctx) {
    Object* value = nullptr;
    switch (lookupTyped(ctx, ctx.args[1], ObjectType::Set, value)) {
        case LookupOutcome::Found:   ctx.reply().integer(static_cast<std::int64_t>(value->set().size())); break;
        case LookupOutcome::Missing: ctx.reply().integer(0); break;
        case LookupOutcome::WrongType: break;
    }
}

void cmdSPop(CommandContext& ctx) {
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
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Set, value, true);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing) {
        if (hasCount) ctx.reply().arrayHeader(0);
        else ctx.reply().nullBulkString();
        ctx.suppressPropagation();
        return;
    }

    SetValue& set = value->set();
    const std::int64_t taking = std::min<std::int64_t>(requested, static_cast<std::int64_t>(set.size()));

    if (taking == 0) {
        if (hasCount) ctx.reply().arrayHeader(0);
        else ctx.reply().nullBulkString();
        ctx.suppressPropagation();
        return;
    }

    std::vector<Bytes> members(set.begin(), set.end());
    std::shuffle(members.begin(), members.end(), randomEngine());
    members.resize(static_cast<size_t>(taking));

    for (const auto& member : members) set.erase(member);

    RespWriter writer = ctx.reply();
    if (hasCount) {
        writer.arrayHeader(taking);
        for (const auto& member : members) writer.bulkString(member);
    } else {
        writer.bulkString(members.front());
    }

    touchKey(ctx, ctx.args[1], taking);
    dropIfEmpty(ctx, ctx.args[1]);

    // SPOP picks at random, so replaying the command on a replica or from the
    // log would remove different members and the datasets would diverge. The
    // members actually removed are propagated as an explicit SREM instead.
    Args rewritten{"SREM", ctx.args[1]};
    for (const auto& member : members) rewritten.push_back(member);
    ctx.propagateInstead(std::move(rewritten));
}

void cmdSRandMember(CommandContext& ctx) {
    std::int64_t requested = 1;
    const bool hasCount = ctx.args.size() == 3;

    if (hasCount && !readInt(ctx, ctx.args[2], requested)) return;

    Object* value = nullptr;
    const LookupOutcome outcome = lookupTyped(ctx, ctx.args[1], ObjectType::Set, value);
    if (outcome == LookupOutcome::WrongType) return;
    if (outcome == LookupOutcome::Missing || value->set().empty()) {
        if (hasCount) ctx.reply().arrayHeader(0);
        else ctx.reply().nullBulkString();
        return;
    }

    std::vector<Bytes> members(value->set().begin(), value->set().end());

    if (!hasCount) {
        ctx.reply().bulkString(members[randomEngine()() % members.size()]);
        return;
    }

    std::vector<Bytes> chosen;
    if (requested < 0) {
        // A negative count allows the same member more than once and always
        // returns exactly |count| entries.
        const size_t wanted = static_cast<size_t>(-requested);
        chosen.reserve(wanted);
        for (size_t i = 0; i < wanted; ++i) {
            chosen.push_back(members[randomEngine()() % members.size()]);
        }
    } else {
        std::shuffle(members.begin(), members.end(), randomEngine());
        const size_t wanted = std::min(static_cast<size_t>(requested), members.size());
        chosen.assign(members.begin(), members.begin() + static_cast<long>(wanted));
    }

    RespWriter writer = ctx.reply();
    writer.arrayHeader(static_cast<std::int64_t>(chosen.size()));
    for (const auto& member : chosen) writer.bulkString(member);
}

void cmdSMove(CommandContext& ctx) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);

    Object* source = keyspace.lookupWrite(ctx.args[1], ctx.now);
    if (source == nullptr) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }
    if (source->type() != ObjectType::Set) {
        ctx.reply().error(err::kWrongType);
        return;
    }

    Object* destination = keyspace.lookupWrite(ctx.args[2], ctx.now);
    if (destination != nullptr && destination->type() != ObjectType::Set) {
        ctx.reply().error(err::kWrongType);
        return;
    }

    if (source->set().erase(ctx.args[3]) == 0) {
        ctx.reply().integer(0);
        ctx.suppressPropagation();
        return;
    }

    // Look the destination up again after the erase: creating it can rehash
    // the dictionary and invalidate the pointers taken above.
    Object* target = keyspace.getOrCreate(ctx.args[2], ObjectType::Set, ctx.now);
    if (target == nullptr) {
        ctx.reply().error(err::kWrongType);
        return;
    }
    target->set().insert(ctx.args[3]);

    touchKey(ctx, ctx.args[1]);
    touchKey(ctx, ctx.args[2]);
    dropIfEmpty(ctx, ctx.args[1]);
    ctx.reply().integer(1);
}

enum class SetOperation { Intersect, Union, Difference };

// Gathers the operand sets. A missing key is an empty set, which is what makes
// SDIFF of a missing key return the first set unchanged.
bool collectOperands(CommandContext& ctx, size_t firstKey, size_t lastKey,
                     std::vector<const SetValue*>& operands, std::vector<SetValue>& scratch) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    static const SetValue kEmpty;

    for (size_t i = firstKey; i <= lastKey && i < ctx.args.size(); ++i) {
        Object* value = keyspace.lookupRead(ctx.args[i], ctx.now);
        if (value == nullptr) {
            operands.push_back(&kEmpty);
            continue;
        }
        if (value->type() != ObjectType::Set) {
            ctx.reply().error(err::kWrongType);
            return false;
        }
        operands.push_back(&value->set());
    }
    (void)scratch;
    return true;
}

bool computeSetOperation(CommandContext& ctx, SetOperation operation, size_t firstKey,
                         SetValue& result) {
    std::vector<const SetValue*> operands;
    std::vector<SetValue> scratch;
    if (!collectOperands(ctx, firstKey, ctx.args.size() - 1, operands, scratch)) return false;
    if (operands.empty()) return true;

    switch (operation) {
        case SetOperation::Intersect: {
            // Start from the smallest operand: the intersection cannot be
            // larger than it, so this bounds the work by the smallest set
            // rather than the first one named.
            const auto smallest = *std::min_element(
                operands.begin(), operands.end(),
                [](const SetValue* a, const SetValue* b) { return a->size() < b->size(); });

            for (const auto& member : *smallest) {
                const bool inAll = std::all_of(operands.begin(), operands.end(),
                                               [&](const SetValue* set) { return set->count(member) > 0; });
                if (inAll) result.insert(member);
            }
            break;
        }

        case SetOperation::Union:
            for (const SetValue* set : operands) {
                result.insert(set->begin(), set->end());
            }
            break;

        case SetOperation::Difference:
            result = *operands.front();
            for (size_t i = 1; i < operands.size(); ++i) {
                for (const auto& member : *operands[i]) result.erase(member);
            }
            break;
    }
    return true;
}

void setOperation(CommandContext& ctx, SetOperation operation, bool store) {
    const size_t firstKey = store ? 2 : 1;

    SetValue result;
    if (!computeSetOperation(ctx, operation, firstKey, result)) return;

    if (!store) {
        RespWriter writer = ctx.reply();
        writer.arrayHeader(static_cast<std::int64_t>(result.size()));
        for (const auto& member : result) writer.bulkString(member);
        return;
    }

    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    const Bytes& destination = ctx.args[1];

    if (result.empty()) {
        // Storing an empty result deletes the destination, so the key never
        // exists as an empty set.
        const bool existed = keyspace.erase(destination);
        if (existed) touchKey(ctx, destination);
        else ctx.suppressPropagation();
        ctx.reply().integer(0);
        return;
    }

    const std::int64_t size = static_cast<std::int64_t>(result.size());
    Object stored = Object::makeSet();
    stored.set() = std::move(result);
    keyspace.setValue(destination, std::move(stored));

    touchKey(ctx, destination, size);
    ctx.reply().integer(size);
}

} // namespace

void registerSetCommands(CommandTable& table) {
    using namespace cmdflag;

    table.add({"SADD", cmdSAdd, -3, kWrite | kDenyOom | kFast, 1, 1, 1});
    table.add({"SREM", cmdSRem, -3, kWrite | kFast, 1, 1, 1});
    table.add({"SMEMBERS", cmdSMembers, 2, kReadOnly, 1, 1, 1});
    table.add({"SISMEMBER", cmdSIsMember, 3, kReadOnly | kFast, 1, 1, 1});
    table.add({"SMISMEMBER", cmdSMIsMember, -3, kReadOnly | kFast, 1, 1, 1});
    table.add({"SCARD", cmdSCard, 2, kReadOnly | kFast, 1, 1, 1});
    table.add({"SPOP", cmdSPop, -2, kWrite | kFast, 1, 1, 1});
    table.add({"SRANDMEMBER", cmdSRandMember, -2, kReadOnly, 1, 1, 1});
    table.add({"SMOVE", cmdSMove, 4, kWrite | kFast, 1, 2, 1});

    table.add({"SINTER", [](CommandContext& c) { setOperation(c, SetOperation::Intersect, false); }, -2, kReadOnly, 1, -1, 1});
    table.add({"SUNION", [](CommandContext& c) { setOperation(c, SetOperation::Union, false); }, -2, kReadOnly, 1, -1, 1});
    table.add({"SDIFF", [](CommandContext& c) { setOperation(c, SetOperation::Difference, false); }, -2, kReadOnly, 1, -1, 1});

    table.add({"SINTERSTORE", [](CommandContext& c) { setOperation(c, SetOperation::Intersect, true); }, -3, kWrite | kDenyOom, 1, -1, 1});
    table.add({"SUNIONSTORE", [](CommandContext& c) { setOperation(c, SetOperation::Union, true); }, -3, kWrite | kDenyOom, 1, -1, 1});
    table.add({"SDIFFSTORE", [](CommandContext& c) { setOperation(c, SetOperation::Difference, true); }, -3, kWrite | kDenyOom, 1, -1, 1});
}

} // namespace miniredis
