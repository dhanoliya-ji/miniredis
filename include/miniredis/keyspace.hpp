// One logical database: the key dictionary, the expiry table, and the
// per-key metadata that the eviction policies read.
//
// Redis keeps expiring keys in a *second* dictionary rather than a flag on the
// main one, for a specific reason: the background expiry cycle wants to sample
// only keys that actually have a TTL. If TTLs lived on the main dict, sampling
// 20 keys from a million-key database with ten volatile keys would almost
// always sample ten non-volatile ones and do nothing. MiniRedis keeps the same
// split for the same reason.
#pragma once

#include "miniredis/common.hpp"
#include "miniredis/object.hpp"

#include <functional>
#include <optional>
#include <random>
#include <unordered_map>

namespace miniredis {

// What to do when memory exceeds maxmemory. These are the Redis policy names
// verbatim, because operators already know them.
enum class EvictionPolicy {
    NoEviction,      // reject writes with OOM
    AllKeysLru,      // approximated LRU across every key
    AllKeysLfu,      // approximated LFU across every key
    AllKeysRandom,
    VolatileLru,     // only keys that carry a TTL
    VolatileLfu,
    VolatileRandom,
    VolatileTtl,     // the key closest to expiring
};

bool parseEvictionPolicy(std::string_view name, EvictionPolicy& out);
const char* evictionPolicyName(EvictionPolicy policy);
bool policyOnlyConsidersVolatileKeys(EvictionPolicy policy);

// Per-key bookkeeping that sits beside the value.
struct KeyEntry {
    Object value;

    // Coarse access clock, in seconds, for approximated LRU. A 32-bit counter
    // wrapping every ~136 years is plenty and costs a quarter of a timestamp.
    std::uint32_t lruClock = 0;

    // Logarithmic access counter for LFU, in the same 8-bit encoding Redis
    // uses: increments get rarer as the counter climbs, and the counter decays
    // over time so that a key that was hot an hour ago does not stay immortal.
    std::uint8_t lfuCounter = 5;

    // When the LFU counter was last decayed, in minutes since server start.
    std::uint16_t lfuDecayMinute = 0;
};

// A single key sampled as an eviction candidate.
struct EvictionCandidate {
    Bytes key;
    long long idleScore = 0; // higher means "evict me first"
    bool valid = false;
};

// Result of a SCAN step.
struct ScanResult {
    std::uint64_t cursor = 0; // 0 means the iteration completed
    std::vector<Bytes> keys;
};

class Keyspace {
public:
    explicit Keyspace(int index) : m_index(index) {}

    int index() const { return m_index; }

    // ---------------------------------------------------------------------
    // Lookup
    // ---------------------------------------------------------------------

    // Reads bump the access metadata the eviction policies depend on, and
    // lazily delete the key if its TTL has already passed. `now` is threaded
    // through rather than read from the clock inside so that a single command
    // sees one consistent instant.
    Object* lookupRead(const Bytes& key, Millis now);
    Object* lookupWrite(const Bytes& key, Millis now);

    // Existence check that does not disturb the access metadata, used by
    // TYPE, EXISTS and the WATCH machinery.
    bool exists(const Bytes& key, Millis now);

    // ---------------------------------------------------------------------
    // Mutation
    // ---------------------------------------------------------------------

    // Installs a value, clearing any TTL the key had. This is SET semantics:
    // overwriting a key drops its expiry unless KEEPTTL is given.
    Object& setValue(const Bytes& key, Object value);

    // Installs a value while preserving an existing TTL (SET ... KEEPTTL, and
    // every in-place container mutation).
    Object& setValueKeepTtl(const Bytes& key, Object value);

    // Creates the key with an empty container of the given type if absent.
    // Returns nullptr when the key exists with a different type.
    Object* getOrCreate(const Bytes& key, ObjectType type, Millis now);

    bool erase(const Bytes& key);

    // Drops a container key that has just lost its last element, mirroring
    // Redis, where an empty list and a missing key are the same thing.
    void eraseIfEmptyContainer(const Bytes& key);

    void clear();

    // ---------------------------------------------------------------------
    // Expiry
    // ---------------------------------------------------------------------

    void setExpireAt(const Bytes& key, Millis absoluteMs);
    bool persist(const Bytes& key);                 // returns true if a TTL was removed
    bool getExpireAt(const Bytes& key, Millis& out) const;

    // Remaining life in milliseconds: >=0 when a TTL is set, -1 when the key
    // exists without one, -2 when the key does not exist. These are the exact
    // sentinel values the TTL and PTTL commands return.
    std::int64_t ttlMillis(const Bytes& key, Millis now);

    // Deletes the key if its TTL has passed. Returns true if it did.
    bool expireIfNeeded(const Bytes& key, Millis now);

    // Samples up to `sampleSize` volatile keys and deletes the expired ones.
    // Returns how many it removed, and sets `sampledExpiredRatio` so the
    // caller can decide whether to run another round.
    size_t activeExpireCycle(Millis now, size_t sampleSize, double& sampledExpiredRatio);

    size_t volatileCount() const { return m_expires.size(); }

    // ---------------------------------------------------------------------
    // Iteration and inspection
    // ---------------------------------------------------------------------

    size_t size() const { return m_dict.size(); }
    bool empty() const { return m_dict.empty(); }

    std::vector<Bytes> matchingKeys(std::string_view pattern, Millis now);
    std::optional<Bytes> randomKey(Millis now);

    // Cursor-based incremental iteration. The cursor is a bucket index, so a
    // SCAN never blocks the server the way KEYS on a large database would.
    ScanResult scan(std::uint64_t cursor, size_t count,
                    const std::string* matchPattern,
                    const ObjectType* typeFilter,
                    Millis now);

    // Visits every live key. Used by snapshotting and AOF rewriting, both of
    // which need a consistent full pass.
    void forEach(const std::function<void(const Bytes&, const KeyEntry&)>& visit) const;

    size_t memoryUsage() const;

    // ---------------------------------------------------------------------
    // Eviction support
    // ---------------------------------------------------------------------

    // Draws `sampleSize` random keys and returns the best eviction candidate
    // under `policy`. Redis samples rather than maintaining a true LRU list
    // because an exact LRU needs a linked-list pointer pair on every key, and
    // sampling five keys already lands within a few percent of exact LRU.
    EvictionCandidate sampleEvictionCandidate(EvictionPolicy policy, size_t sampleSize,
                                              std::uint32_t lruClockNow, Millis now);

    // Advances the LFU decay clock. Called from the server cron.
    void setLfuMinute(std::uint16_t minute) { m_lfuMinute = minute; }

    // Access counters, reported by INFO.
    std::uint64_t hits() const { return m_hits; }
    std::uint64_t misses() const { return m_misses; }
    std::uint64_t expiredKeys() const { return m_expiredKeys; }
    void addExpiredKeys(std::uint64_t n) { m_expiredKeys += n; }

    // Called by the server whenever a key expires or is evicted, so the change
    // can be written to the AOF and propagated to replicas as an explicit DEL.
    // Without this, a replica would keep a key its master had already dropped.
    using KeyRemovalHook = std::function<void(int dbIndex, const Bytes& key)>;
    void setKeyRemovalHook(KeyRemovalHook hook) { m_removalHook = std::move(hook); }

    void setLruClock(std::uint32_t clock) { m_lruClock = clock; }

private:
    void touch(KeyEntry& entry);
    std::uint8_t decayedLfuCounter(const KeyEntry& entry) const;
    KeyEntry* randomEntry(bool volatileOnly, Bytes& keyOut);

    int m_index;
    std::unordered_map<Bytes, KeyEntry> m_dict;
    std::unordered_map<Bytes, Millis> m_expires; // only keys that carry a TTL

    std::uint32_t m_lruClock = 0;
    std::uint16_t m_lfuMinute = 0;

    std::uint64_t m_hits = 0;
    std::uint64_t m_misses = 0;
    std::uint64_t m_expiredKeys = 0;

    KeyRemovalHook m_removalHook;
    std::mt19937_64 m_random{0x9E3779B97F4A7C15ULL};
};

} // namespace miniredis
