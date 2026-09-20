// Helpers shared by the command implementations.
//
// Internal to src/; not part of the public include/ surface.
#pragma once

#include "miniredis/command.hpp"
#include "miniredis/server.hpp"

namespace miniredis {
namespace detail {

// Looks up a key for reading and checks its type in one step.
//
// Returns:
//   found   = the key exists and has the expected type (value is set)
//   missing = the key does not exist (the caller replies with its own empty case)
//   wrong   = the key exists with a different type; the WRONGTYPE error has
//             already been written, so the caller must simply return
enum class LookupOutcome { Found, Missing, WrongType };

inline LookupOutcome lookupTyped(CommandContext& ctx, const Bytes& key, ObjectType expected,
                                 Object*& valueOut, bool forWrite = false) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* value = forWrite ? keyspace.lookupWrite(key, ctx.now) : keyspace.lookupRead(key, ctx.now);

    if (value == nullptr) {
        valueOut = nullptr;
        return LookupOutcome::Missing;
    }
    if (value->type() != expected) {
        ctx.reply().error(err::kWrongType);
        valueOut = nullptr;
        return LookupOutcome::WrongType;
    }

    valueOut = value;
    return LookupOutcome::Found;
}

// Creates the key with an empty container of `type` if it is absent. Writes
// the WRONGTYPE error and returns nullptr when the key holds another type.
inline Object* openForWrite(CommandContext& ctx, const Bytes& key, ObjectType type) {
    Keyspace& keyspace = ctx.server.currentDb(ctx.client);
    Object* value = keyspace.getOrCreate(key, type, ctx.now);
    if (value == nullptr) {
        ctx.reply().error(err::kWrongType);
    }
    return value;
}

// Parses an integer argument, writing the standard error on failure.
inline bool readInt(CommandContext& ctx, const Bytes& text, std::int64_t& out) {
    if (!parseInt64(text, out)) {
        ctx.reply().error(err::kNotInteger);
        return false;
    }
    return true;
}

inline bool readDouble(CommandContext& ctx, const Bytes& text, double& out) {
    if (!parseDouble(text, out)) {
        ctx.reply().error(err::kNotFloat);
        return false;
    }
    return true;
}

// Normalises a Redis range. Negative indexes count back from the end, the
// range is inclusive, and an empty result is signalled by start > stop.
//
// Returns false when the range selects nothing.
inline bool normaliseRange(std::int64_t start, std::int64_t stop, std::int64_t length,
                           std::int64_t& startOut, std::int64_t& stopOut) {
    if (length == 0) return false;

    if (start < 0) start += length;
    if (stop < 0) stop += length;
    if (start < 0) start = 0;
    if (stop >= length) stop = length - 1;

    if (start > stop || start >= length) return false;

    startOut = start;
    stopOut = stop;
    return true;
}

// Records that a key changed: bumps the dirty counter (which is what makes the
// dispatcher propagate the command) and invalidates any WATCH on it.
inline void touchKey(CommandContext& ctx, const Bytes& key, std::int64_t changes = 1) {
    ctx.server.markDirty(changes);
    ctx.server.signalKeyModified(ctx.client.dbIndex, key);
}

// Deletes a container key that just lost its last element, so that an empty
// list and a missing key remain indistinguishable, as in Redis.
inline void dropIfEmpty(CommandContext& ctx, const Bytes& key) {
    ctx.server.currentDb(ctx.client).eraseIfEmptyContainer(key);
}

} // namespace detail
} // namespace miniredis
