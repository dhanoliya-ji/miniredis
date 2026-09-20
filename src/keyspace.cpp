#include "miniredis/keyspace.hpp"

#include <algorithm>
#include <cmath>

namespace miniredis {

namespace {

// Redis's LFU counter is logarithmic: the probability of an increment falls as
// the counter rises, so an 8-bit counter can represent access frequencies from
// "once" to "millions of times" without saturating after 255 hits.
constexpr double kLfuLogFactor = 10.0;

// How many minutes of inactivity halve the LFU counter.
constexpr std::uint16_t kLfuDecayMinutes = 1;

} // namespace

bool parseEvictionPolicy(std::string_view name, EvictionPolicy& out) {
    const std::string lowered = toLower(name);
    if (lowered == "noeviction")        { out = EvictionPolicy::NoEviction;      return true; }
    if (lowered == "allkeys-lru")       { out = EvictionPolicy::AllKeysLru;      return true; }
    if (lowered == "allkeys-lfu")       { out = EvictionPolicy::AllKeysLfu;      return true; }
    if (lowered == "allkeys-random")    { out = EvictionPolicy::AllKeysRandom;   return true; }
    if (lowered == "volatile-lru")      { out = EvictionPolicy::VolatileLru;     return true; }
    if (lowered == "volatile-lfu")      { out = EvictionPolicy::VolatileLfu;     return true; }
    if (lowered == "volatile-random")   { out = EvictionPolicy::VolatileRandom;  return true; }
    if (lowered == "volatile-ttl")      { out = EvictionPolicy::VolatileTtl;     return true; }
    return false;
}

const char* evictionPolicyName(EvictionPolicy policy) {
    switch (policy) {
        case EvictionPolicy::NoEviction:     return "noeviction";
        case EvictionPolicy::AllKeysLru:     return "allkeys-lru";
        case EvictionPolicy::AllKeysLfu:     return "allkeys-lfu";
        case EvictionPolicy::AllKeysRandom:  return "allkeys-random";
        case EvictionPolicy::VolatileLru:    return "volatile-lru";
        case EvictionPolicy::VolatileLfu:    return "volatile-lfu";
        case EvictionPolicy::VolatileRandom: return "volatile-random";
        case EvictionPolicy::VolatileTtl:    return "volatile-ttl";
    }
    return "noeviction";
}

bool policyOnlyConsidersVolatileKeys(EvictionPolicy policy) {
    switch (policy) {
        case EvictionPolicy::VolatileLru:
        case EvictionPolicy::VolatileLfu:
        case EvictionPolicy::VolatileRandom:
        case EvictionPolicy::VolatileTtl:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Access metadata
// ---------------------------------------------------------------------------

void Keyspace::touch(KeyEntry& entry) {
    entry.lruClock = m_lruClock;

    // Logarithmic increment, as in Redis's LFULogIncr. The higher the counter
    // already is, the less likely another hit is to raise it.
    if (entry.lfuCounter < 255) {
        const std::uint8_t decayed = decayedLfuCounter(entry);
        const double baseline = static_cast<double>(decayed) - 5.0;
        const double chance = baseline < 0
            ? 1.0
            : 1.0 / (baseline * kLfuLogFactor + 1.0);

        const double roll = static_cast<double>(m_random() % 1000000) / 1000000.0;
        entry.lfuCounter = (roll < chance && decayed < 255)
            ? static_cast<std::uint8_t>(decayed + 1)
            : decayed;
    }
    entry.lfuDecayMinute = m_lfuMinute;
}

std::uint8_t Keyspace::decayedLfuCounter(const KeyEntry& entry) const {
    if (m_lfuMinute < entry.lfuDecayMinute) return entry.lfuCounter; // clock moved back
    const std::uint16_t elapsed = static_cast<std::uint16_t>(m_lfuMinute - entry.lfuDecayMinute);
    const std::uint16_t periods = elapsed / kLfuDecayMinutes;
    if (periods == 0) return entry.lfuCounter;
    if (periods >= 8) return 0;
    const std::uint8_t decayed = static_cast<std::uint8_t>(entry.lfuCounter >> periods);
    return decayed;
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

Object* Keyspace::lookupRead(const Bytes& key, Millis now) {
    if (expireIfNeeded(key, now)) {
        ++m_misses;
        return nullptr;
    }

    auto it = m_dict.find(key);
    if (it == m_dict.end()) {
        ++m_misses;
        return nullptr;
    }

    ++m_hits;
    touch(it->second);
    return &it->second.value;
}

Object* Keyspace::lookupWrite(const Bytes& key, Millis now) {
    expireIfNeeded(key, now);
    auto it = m_dict.find(key);
    if (it == m_dict.end()) return nullptr;
    touch(it->second);
    return &it->second.value;
}

bool Keyspace::exists(const Bytes& key, Millis now) {
    if (expireIfNeeded(key, now)) return false;
    return m_dict.find(key) != m_dict.end();
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

Object& Keyspace::setValue(const Bytes& key, Object value) {
    m_expires.erase(key); // a plain overwrite clears any TTL
    KeyEntry& entry = m_dict[key];
    entry.value = std::move(value);
    entry.lfuCounter = 5;
    touch(entry);
    return entry.value;
}

Object& Keyspace::setValueKeepTtl(const Bytes& key, Object value) {
    KeyEntry& entry = m_dict[key];
    entry.value = std::move(value);
    touch(entry);
    return entry.value;
}

Object* Keyspace::getOrCreate(const Bytes& key, ObjectType type, Millis now) {
    expireIfNeeded(key, now);

    auto it = m_dict.find(key);
    if (it != m_dict.end()) {
        if (it->second.value.type() != type) return nullptr; // caller reports WRONGTYPE
        touch(it->second);
        return &it->second.value;
    }

    Object fresh;
    switch (type) {
        case ObjectType::List: fresh = Object::makeList(); break;
        case ObjectType::Hash: fresh = Object::makeHash(); break;
        case ObjectType::Set:  fresh = Object::makeSet();  break;
        case ObjectType::ZSet: fresh = Object::makeZSet(); break;
        case ObjectType::String: fresh = Object::makeString({}); break;
    }

    KeyEntry& entry = m_dict[key];
    entry.value = std::move(fresh);
    entry.lfuCounter = 5;
    touch(entry);
    return &entry.value;
}

bool Keyspace::erase(const Bytes& key) {
    m_expires.erase(key);
    return m_dict.erase(key) > 0;
}

void Keyspace::eraseIfEmptyContainer(const Bytes& key) {
    auto it = m_dict.find(key);
    if (it != m_dict.end() && it->second.value.isEmptyContainer()) {
        m_expires.erase(key);
        m_dict.erase(it);
    }
}

void Keyspace::clear() {
    m_dict.clear();
    m_expires.clear();
}

// ---------------------------------------------------------------------------
// Expiry
// ---------------------------------------------------------------------------

void Keyspace::setExpireAt(const Bytes& key, Millis absoluteMs) {
    if (m_dict.find(key) == m_dict.end()) return;
    m_expires[key] = absoluteMs;
}

bool Keyspace::persist(const Bytes& key) {
    return m_expires.erase(key) > 0;
}

bool Keyspace::getExpireAt(const Bytes& key, Millis& out) const {
    auto it = m_expires.find(key);
    if (it == m_expires.end()) return false;
    out = it->second;
    return true;
}

std::int64_t Keyspace::ttlMillis(const Bytes& key, Millis now) {
    if (expireIfNeeded(key, now)) return -2;
    if (m_dict.find(key) == m_dict.end()) return -2;

    auto it = m_expires.find(key);
    if (it == m_expires.end()) return -1;

    const std::int64_t remaining = it->second - now;
    return remaining < 0 ? 0 : remaining;
}

bool Keyspace::expireIfNeeded(const Bytes& key, Millis now) {
    auto it = m_expires.find(key);
    if (it == m_expires.end()) return false;
    if (it->second > now) return false;

    // The removal hook must fire before the erase, because AOF and replication
    // want to record a DEL for a key that still exists at that moment. Without
    // it a replica would serve a key its master had already expired.
    if (m_removalHook) m_removalHook(m_index, key);

    m_expires.erase(it);
    m_dict.erase(key);
    ++m_expiredKeys;
    return true;
}

size_t Keyspace::activeExpireCycle(Millis now, size_t sampleSize, double& sampledExpiredRatio) {
    sampledExpiredRatio = 0.0;
    if (m_expires.empty()) return 0;

    const size_t bucketCount = m_expires.bucket_count();
    if (bucketCount == 0) return 0;

    std::vector<Bytes> expiredKeys;
    size_t sampled = 0;

    // Sample by walking from a random bucket. Redis's cycle is time-budgeted;
    // ours is sample-count budgeted, which is simpler and bounds the work per
    // cron tick just as effectively.
    size_t bucket = static_cast<size_t>(m_random() % bucketCount);
    for (size_t scanned = 0; scanned < bucketCount && sampled < sampleSize; ++scanned) {
        for (auto it = m_expires.begin(bucket); it != m_expires.end(bucket); ++it) {
            ++sampled;
            if (it->second <= now) expiredKeys.push_back(it->first);
            if (sampled >= sampleSize) break;
        }
        bucket = (bucket + 1) % bucketCount;
    }

    for (const auto& key : expiredKeys) {
        if (m_removalHook) m_removalHook(m_index, key);
        m_expires.erase(key);
        m_dict.erase(key);
        ++m_expiredKeys;
    }

    if (sampled > 0) {
        sampledExpiredRatio = static_cast<double>(expiredKeys.size()) / static_cast<double>(sampled);
    }
    return expiredKeys.size();
}

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

std::vector<Bytes> Keyspace::matchingKeys(std::string_view pattern, Millis now) {
    std::vector<Bytes> result;
    const bool matchAll = (pattern == "*");

    std::vector<Bytes> expiredKeys;
    for (const auto& [key, entry] : m_dict) {
        (void)entry;
        auto expireIt = m_expires.find(key);
        if (expireIt != m_expires.end() && expireIt->second <= now) {
            expiredKeys.push_back(key);
            continue;
        }
        if (matchAll || globMatch(pattern, key)) result.push_back(key);
    }

    // Collected first, deleted after: erasing inside the loop above would
    // invalidate the iterator we are standing on.
    for (const auto& key : expiredKeys) expireIfNeeded(key, now);
    return result;
}

std::optional<Bytes> Keyspace::randomKey(Millis now) {
    for (int attempt = 0; attempt < 20 && !m_dict.empty(); ++attempt) {
        Bytes key;
        if (randomEntry(/*volatileOnly=*/false, key) == nullptr) break;
        if (!expireIfNeeded(key, now)) return key;
    }
    return std::nullopt;
}

ScanResult Keyspace::scan(std::uint64_t cursor, size_t count,
                          const std::string* matchPattern,
                          const ObjectType* typeFilter,
                          Millis now) {
    ScanResult result;

    const size_t bucketCount = m_dict.bucket_count();
    if (bucketCount == 0 || cursor >= bucketCount) {
        result.cursor = 0;
        return result;
    }

    // The cursor is a bucket index. SCAN's contract is only that a key present
    // for the whole iteration is returned at least once, and that keys may be
    // returned more than once; it does not promise a snapshot. Rehashing
    // between calls can therefore repeat or, in the worst case, skip keys --
    // Redis avoids the skip with reverse-binary cursor increments, which is
    // noted here as the one guarantee this simpler cursor does not provide.
    size_t bucket = static_cast<size_t>(cursor);
    size_t visited = 0;

    while (bucket < bucketCount && visited < count) {
        for (auto it = m_dict.begin(bucket); it != m_dict.end(bucket); ++it) {
            ++visited;

            auto expireIt = m_expires.find(it->first);
            if (expireIt != m_expires.end() && expireIt->second <= now) continue;
            if (typeFilter != nullptr && it->second.value.type() != *typeFilter) continue;
            if (matchPattern != nullptr && !globMatch(*matchPattern, it->first)) continue;

            result.keys.push_back(it->first);
        }
        ++bucket;
    }

    result.cursor = (bucket >= bucketCount) ? 0 : static_cast<std::uint64_t>(bucket);
    return result;
}

void Keyspace::forEach(const std::function<void(const Bytes&, const KeyEntry&)>& visit) const {
    for (const auto& [key, entry] : m_dict) {
        visit(key, entry);
    }
}

size_t Keyspace::memoryUsage() const {
    size_t total = sizeof(Keyspace);
    for (const auto& [key, entry] : m_dict) {
        total += key.size() + sizeof(Bytes) + sizeof(KeyEntry) + entry.value.memoryUsage() + 48;
    }
    total += m_expires.size() * (sizeof(Bytes) + sizeof(Millis) + 48);
    return total;
}

// ---------------------------------------------------------------------------
// Eviction
// ---------------------------------------------------------------------------

KeyEntry* Keyspace::randomEntry(bool volatileOnly, Bytes& keyOut) {
    if (volatileOnly) {
        if (m_expires.empty()) return nullptr;
        const size_t bucketCount = m_expires.bucket_count();
        if (bucketCount == 0) return nullptr;

        size_t bucket = static_cast<size_t>(m_random() % bucketCount);
        for (size_t scanned = 0; scanned < bucketCount; ++scanned) {
            auto it = m_expires.begin(bucket);
            if (it != m_expires.end(bucket)) {
                auto entryIt = m_dict.find(it->first);
                if (entryIt != m_dict.end()) {
                    keyOut = entryIt->first;
                    return &entryIt->second;
                }
            }
            bucket = (bucket + 1) % bucketCount;
        }
        return nullptr;
    }

    if (m_dict.empty()) return nullptr;
    const size_t bucketCount = m_dict.bucket_count();
    if (bucketCount == 0) return nullptr;

    size_t bucket = static_cast<size_t>(m_random() % bucketCount);
    for (size_t scanned = 0; scanned < bucketCount; ++scanned) {
        auto it = m_dict.begin(bucket);
        if (it != m_dict.end(bucket)) {
            keyOut = it->first;
            return &it->second;
        }
        bucket = (bucket + 1) % bucketCount;
    }
    return nullptr;
}

EvictionCandidate Keyspace::sampleEvictionCandidate(EvictionPolicy policy, size_t sampleSize,
                                                    std::uint32_t lruClockNow, Millis now) {
    EvictionCandidate best;
    const bool volatileOnly = policyOnlyConsidersVolatileKeys(policy);

    if (volatileOnly ? m_expires.empty() : m_dict.empty()) return best;

    for (size_t i = 0; i < sampleSize; ++i) {
        Bytes key;
        KeyEntry* entry = randomEntry(volatileOnly, key);
        if (entry == nullptr) break;

        long long score = 0;
        switch (policy) {
            case EvictionPolicy::AllKeysLru:
            case EvictionPolicy::VolatileLru:
                // Idle time in seconds: the longer since the last touch, the
                // better a candidate.
                score = static_cast<long long>(lruClockNow) - static_cast<long long>(entry->lruClock);
                break;

            case EvictionPolicy::AllKeysLfu:
            case EvictionPolicy::VolatileLfu:
                // Lower frequency evicts first, so negate to keep "higher is
                // better" uniform across policies.
                score = -static_cast<long long>(decayedLfuCounter(*entry));
                break;

            case EvictionPolicy::VolatileTtl: {
                // Evict whatever dies soonest.
                Millis expireAt = 0;
                if (!getExpireAt(key, expireAt)) continue;
                score = -(expireAt - now);
                break;
            }

            case EvictionPolicy::AllKeysRandom:
            case EvictionPolicy::VolatileRandom:
                score = static_cast<long long>(m_random() & 0x7FFFFFFF);
                break;

            case EvictionPolicy::NoEviction:
                return best;
        }

        if (!best.valid || score > best.idleScore) {
            best.key = key;
            best.idleScore = score;
            best.valid = true;
        }
    }

    return best;
}

} // namespace miniredis
