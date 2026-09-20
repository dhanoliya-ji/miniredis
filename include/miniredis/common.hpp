// MiniRedis - a Redis-style in-memory database built from scratch.
//
// Shared vocabulary types and small helpers used across every subsystem.
#pragma once

#include <cstdint>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace miniredis {

using Bytes = std::string;          // Redis strings are binary safe; so are ours.
using Args = std::vector<Bytes>;    // A command is an array of binary-safe words.

// Milliseconds since the Unix epoch. Every deadline in the server (key expiry,
// cron ticks, replication timeouts) is expressed in this one unit so that no
// subsystem has to reason about clock conversions.
using Millis = std::int64_t;

inline Millis nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// A monotonic clock for latency measurement, which must not jump when the
// wall clock is adjusted.
inline std::int64_t monotonicMicros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::string toUpper(std::string_view s);
std::string toLower(std::string_view s);
std::string trim(std::string_view s);

// Case-insensitive comparison, used for command names and option keywords.
bool equalsIgnoreCase(std::string_view a, std::string_view b);

// Strict integer parsing: the whole string must be consumed, so "12abc" and
// "" are both rejected. Redis is strict here and clients depend on it.
bool parseInt64(std::string_view s, std::int64_t& out);
bool parseDouble(std::string_view s, double& out);

std::string formatInt64(std::int64_t v);

// Redis renders doubles without a trailing ".0" and with up to 17 significant
// digits, so that a value survives a round trip through the wire format.
std::string formatDouble(double v);

// Glob-style matcher supporting *, ?, [abc], [a-c], [^a] and \ escapes.
// This is what KEYS, SCAN MATCH and PSUBSCRIBE use.
bool globMatch(std::string_view pattern, std::string_view text);

// A quick, well-distributed 64-bit hash (FNV-1a) used for sampling and for
// the approximated-LRU eviction pool.
std::uint64_t fnv1a64(std::string_view s);

// Parse a memory size such as "512mb", "1gb", "1048576". Returns false on a
// malformed value.
bool parseMemorySize(std::string_view s, std::int64_t& bytes);
std::string formatMemorySize(std::int64_t bytes);

} // namespace miniredis
