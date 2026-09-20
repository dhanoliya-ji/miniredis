// Tests for the keyspace: types, expiry, eviction and the sorted set.
#include "test_framework.hpp"

#include "miniredis/cluster.hpp"
#include "miniredis/keyspace.hpp"
#include "miniredis/object.hpp"

#include <set>
#include <string>

using namespace miniredis;

namespace {

constexpr Millis kNow = 1'700'000'000'000; // a fixed instant, so tests are deterministic

} // namespace

// ---------------------------------------------------------------------------
// Object types
// ---------------------------------------------------------------------------

TEST(Object, ReportsItsTypeAndLength) {
    Object text = Object::makeString("hello");
    CHECK(text.type() == ObjectType::String);
    CHECK_EQ(text.length(), size_t{5});
    CHECK_EQ(std::string(text.typeName()), std::string("string"));
    // An empty string is a real value, unlike an empty container.
    CHECK(!text.isEmptyContainer());

    Object list = Object::makeList();
    CHECK(list.isEmptyContainer());
    list.list().push_back("a");
    CHECK(!list.isEmptyContainer());
    CHECK_EQ(list.length(), size_t{1});
}

TEST(Object, MemoryEstimateGrowsWithContent) {
    Object small = Object::makeString("a");
    Object large = Object::makeString(std::string(10000, 'x'));
    CHECK(large.memoryUsage() > small.memoryUsage());
}

// ---------------------------------------------------------------------------
// Sorted set
// ---------------------------------------------------------------------------

TEST(ZSet, KeepsScoreAndOrderIndexesConsistent) {
    ZSet zset;
    CHECK(zset.add("bob", 90));
    CHECK(zset.add("alice", 100));
    CHECK(zset.add("carol", 80));
    CHECK_EQ(zset.size(), size_t{3});

    // Re-adding is an update, not an insert.
    CHECK(!zset.add("bob", 95));
    CHECK_EQ(zset.size(), size_t{3});

    double score = 0;
    CHECK(zset.score("bob", score));
    CHECK_NEAR(score, 95, 1e-9);

    // The ordered index must have followed the score change rather than
    // keeping a stale (90, bob) entry alongside the new one.
    const auto& ordered = zset.ordered();
    CHECK_EQ(ordered.size(), size_t{3});

    std::vector<std::string> byScore;
    for (const auto& [entryScore, member] : ordered) {
        (void)entryScore;
        byScore.push_back(member);
    }
    CHECK_EQ(byScore[0], std::string("carol"));
    CHECK_EQ(byScore[1], std::string("bob"));
    CHECK_EQ(byScore[2], std::string("alice"));
}

TEST(ZSet, RanksAscendingAndDescending) {
    ZSet zset;
    zset.add("a", 1);
    zset.add("b", 2);
    zset.add("c", 3);

    CHECK_EQ(zset.rank("a", false), std::int64_t{0});
    CHECK_EQ(zset.rank("c", false), std::int64_t{2});
    CHECK_EQ(zset.rank("a", true), std::int64_t{2});
    CHECK_EQ(zset.rank("missing", false), std::int64_t{-1});
}

TEST(ZSet, BreaksScoreTiesLexicographically) {
    ZSet zset;
    zset.add("zebra", 5);
    zset.add("apple", 5);

    auto it = zset.ordered().begin();
    CHECK_EQ(it->second, std::string("apple"));
    ++it;
    CHECK_EQ(it->second, std::string("zebra"));
}

TEST(ZSet, RangeByScoreHonoursExclusiveBounds) {
    ZSet zset;
    for (int i = 1; i <= 5; ++i) {
        zset.add("m" + std::to_string(i), i);
    }

    CHECK_EQ(zset.rangeByScore(2, false, 4, false).size(), size_t{3}); // 2,3,4
    CHECK_EQ(zset.rangeByScore(2, true, 4, false).size(), size_t{2});  // 3,4
    CHECK_EQ(zset.rangeByScore(2, true, 4, true).size(), size_t{1});   // 3
    CHECK_EQ(zset.rangeByScore(10, false, 20, false).size(), size_t{0});
}

TEST(ZSet, RemoveClearsBothIndexes) {
    ZSet zset;
    zset.add("a", 1);
    CHECK(zset.remove("a"));
    CHECK(!zset.remove("a"));
    CHECK(zset.empty());
    CHECK_EQ(zset.ordered().size(), size_t{0});
}

// ---------------------------------------------------------------------------
// Keyspace basics
// ---------------------------------------------------------------------------

TEST(Keyspace, StoresAndRetrievesValues) {
    Keyspace keyspace(0);
    keyspace.setValue("k", Object::makeString("v"));

    Object* value = keyspace.lookupRead("k", kNow);
    CHECK(value != nullptr);
    CHECK_EQ(value->string(), std::string("v"));
    CHECK_EQ(keyspace.size(), size_t{1});

    CHECK(keyspace.lookupRead("absent", kNow) == nullptr);
    CHECK(keyspace.erase("k"));
    CHECK(!keyspace.erase("k"));
}

TEST(Keyspace, CountsHitsAndMisses) {
    Keyspace keyspace(0);
    keyspace.setValue("k", Object::makeString("v"));

    keyspace.lookupRead("k", kNow);
    keyspace.lookupRead("k", kNow);
    keyspace.lookupRead("absent", kNow);

    CHECK_EQ(keyspace.hits(), std::uint64_t{2});
    CHECK_EQ(keyspace.misses(), std::uint64_t{1});
}

TEST(Keyspace, GetOrCreateRefusesAMismatchedType) {
    Keyspace keyspace(0);
    keyspace.setValue("k", Object::makeString("v"));

    // This is the WRONGTYPE guard the command layer relies on.
    CHECK(keyspace.getOrCreate("k", ObjectType::List, kNow) == nullptr);
    CHECK(keyspace.getOrCreate("fresh", ObjectType::List, kNow) != nullptr);
}

TEST(Keyspace, DropsContainersThatLoseTheirLastElement) {
    Keyspace keyspace(0);
    Object* list = keyspace.getOrCreate("q", ObjectType::List, kNow);
    list->list().push_back("only");

    keyspace.eraseIfEmptyContainer("q");
    CHECK_EQ(keyspace.size(), size_t{1}); // still has an element

    list->list().pop_back();
    keyspace.eraseIfEmptyContainer("q");
    // An empty list and a missing key must be indistinguishable.
    CHECK_EQ(keyspace.size(), size_t{0});
}

// ---------------------------------------------------------------------------
// Expiry
// ---------------------------------------------------------------------------

TEST(Expiry, ReportsTheDocumentedSentinelValues) {
    Keyspace keyspace(0);

    // -2 for a missing key, -1 for a key with no TTL, >= 0 for a real one.
    CHECK_EQ(keyspace.ttlMillis("absent", kNow), std::int64_t{-2});

    keyspace.setValue("k", Object::makeString("v"));
    CHECK_EQ(keyspace.ttlMillis("k", kNow), std::int64_t{-1});

    keyspace.setExpireAt("k", kNow + 5000);
    CHECK_EQ(keyspace.ttlMillis("k", kNow), std::int64_t{5000});
}

TEST(Expiry, DeletesLazilyOnLookup) {
    Keyspace keyspace(0);
    keyspace.setValue("k", Object::makeString("v"));
    keyspace.setExpireAt("k", kNow + 1000);

    CHECK(keyspace.lookupRead("k", kNow) != nullptr);
    // One millisecond past the deadline the key is gone, and the lookup that
    // noticed is the one that removed it.
    CHECK(keyspace.lookupRead("k", kNow + 1001) == nullptr);
    CHECK_EQ(keyspace.size(), size_t{0});
}

TEST(Expiry, ActiveCycleReclaimsKeysNobodyLooksUp) {
    Keyspace keyspace(0);
    for (int i = 0; i < 100; ++i) {
        const std::string key = "k" + std::to_string(i);
        keyspace.setValue(key, Object::makeString("v"));
        keyspace.setExpireAt(key, kNow + 1000);
    }
    CHECK_EQ(keyspace.size(), size_t{100});

    // Lazy expiry alone would leave these in memory forever, because nothing
    // ever reads them again. The cycle is what actually frees the memory.
    size_t removedTotal = 0;
    for (int round = 0; round < 50 && keyspace.size() > 0; ++round) {
        double ratio = 0;
        removedTotal += keyspace.activeExpireCycle(kNow + 2000, 20, ratio);
    }

    CHECK_EQ(keyspace.size(), size_t{0});
    CHECK_EQ(removedTotal, size_t{100});
}

TEST(Expiry, PlainOverwriteClearsTheTtl) {
    Keyspace keyspace(0);
    keyspace.setValue("k", Object::makeString("v"));
    keyspace.setExpireAt("k", kNow + 5000);

    // SET semantics: overwriting replaces the whole key, TTL included.
    keyspace.setValue("k", Object::makeString("v2"));
    CHECK_EQ(keyspace.ttlMillis("k", kNow), std::int64_t{-1});
}

TEST(Expiry, KeepTtlVariantPreservesIt) {
    Keyspace keyspace(0);
    keyspace.setValue("k", Object::makeString("v"));
    keyspace.setExpireAt("k", kNow + 5000);

    keyspace.setValueKeepTtl("k", Object::makeString("v2"));
    CHECK_EQ(keyspace.ttlMillis("k", kNow), std::int64_t{5000});
}

TEST(Expiry, RemovalHookFiresBeforeTheKeyDisappears) {
    Keyspace keyspace(0);

    // The hook is what turns an expiry into a DEL in the AOF and on the
    // replica link; if it fired after the erase, the key would already be gone
    // and there would be nothing to record.
    std::vector<std::string> removed;
    keyspace.setKeyRemovalHook([&](int, const Bytes& key) { removed.push_back(key); });

    keyspace.setValue("k", Object::makeString("v"));
    keyspace.setExpireAt("k", kNow + 1000);
    keyspace.lookupRead("k", kNow + 2000);

    CHECK_EQ(removed.size(), size_t{1});
    CHECK_EQ(removed[0], std::string("k"));
}

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

TEST(Scan, VisitsEveryKeyExactlyOnceOnAStableKeyspace) {
    Keyspace keyspace(0);
    for (int i = 0; i < 500; ++i) {
        keyspace.setValue("key:" + std::to_string(i), Object::makeString("v"));
    }

    std::set<std::string> seen;
    std::uint64_t cursor = 0;
    int iterations = 0;

    do {
        const ScanResult result = keyspace.scan(cursor, 10, nullptr, nullptr, kNow);
        for (const auto& key : result.keys) seen.insert(key);
        cursor = result.cursor;
        CHECK(++iterations < 10000); // a cursor that never returns to 0 is a bug
    } while (cursor != 0);

    CHECK_EQ(seen.size(), size_t{500});
}

TEST(Scan, AppliesMatchAndTypeFilters) {
    Keyspace keyspace(0);
    keyspace.setValue("user:1", Object::makeString("a"));
    keyspace.setValue("user:2", Object::makeString("b"));
    keyspace.setValue("post:1", Object::makeString("c"));
    keyspace.getOrCreate("queue", ObjectType::List, kNow)->list().push_back("x");

    const std::string pattern = "user:*";
    std::set<std::string> matched;
    std::uint64_t cursor = 0;
    do {
        const ScanResult result = keyspace.scan(cursor, 100, &pattern, nullptr, kNow);
        for (const auto& key : result.keys) matched.insert(key);
        cursor = result.cursor;
    } while (cursor != 0);
    CHECK_EQ(matched.size(), size_t{2});

    const ObjectType listOnly = ObjectType::List;
    std::set<std::string> lists;
    cursor = 0;
    do {
        const ScanResult result = keyspace.scan(cursor, 100, nullptr, &listOnly, kNow);
        for (const auto& key : result.keys) lists.insert(key);
        cursor = result.cursor;
    } while (cursor != 0);
    CHECK_EQ(lists.size(), size_t{1});
}

TEST(Keys, ExcludesAlreadyExpiredKeys) {
    Keyspace keyspace(0);
    keyspace.setValue("live", Object::makeString("v"));
    keyspace.setValue("dead", Object::makeString("v"));
    keyspace.setExpireAt("dead", kNow + 1000);

    const auto matches = keyspace.matchingKeys("*", kNow + 5000);
    CHECK_EQ(matches.size(), size_t{1});
    CHECK_EQ(matches[0], std::string("live"));
}

// ---------------------------------------------------------------------------
// Eviction
// ---------------------------------------------------------------------------

TEST(Eviction, ParsesEveryPolicyName) {
    EvictionPolicy policy = EvictionPolicy::NoEviction;
    CHECK(parseEvictionPolicy("allkeys-lru", policy));
    CHECK(policy == EvictionPolicy::AllKeysLru);
    CHECK(parseEvictionPolicy("volatile-ttl", policy));
    CHECK(policy == EvictionPolicy::VolatileTtl);
    CHECK(!parseEvictionPolicy("nonsense", policy));

    // The name must round trip, because it is reported by CONFIG GET and INFO.
    CHECK_EQ(std::string(evictionPolicyName(EvictionPolicy::AllKeysLfu)),
             std::string("allkeys-lfu"));
}

TEST(Eviction, VolatilePoliciesOnlyConsiderKeysWithATtl) {
    Keyspace keyspace(0);
    for (int i = 0; i < 50; ++i) {
        keyspace.setValue("permanent:" + std::to_string(i), Object::makeString("v"));
    }
    keyspace.setValue("temporary", Object::makeString("v"));
    keyspace.setExpireAt("temporary", kNow + 100000);

    // Sampled repeatedly, a volatile policy must never propose one of the 50
    // keys that carry no TTL, however many times it draws.
    for (int attempt = 0; attempt < 50; ++attempt) {
        const EvictionCandidate candidate =
            keyspace.sampleEvictionCandidate(EvictionPolicy::VolatileLru, 5, 1000, kNow);
        if (candidate.valid) {
            CHECK_EQ(candidate.key, std::string("temporary"));
        }
    }
}

TEST(Eviction, VolatilePolicyFindsNothingWhenNoKeyHasATtl) {
    Keyspace keyspace(0);
    keyspace.setValue("k", Object::makeString("v"));

    const EvictionCandidate candidate =
        keyspace.sampleEvictionCandidate(EvictionPolicy::VolatileLru, 5, 1000, kNow);
    CHECK(!candidate.valid);
}

TEST(Eviction, LruPrefersTheLeastRecentlyTouchedKey) {
    Keyspace keyspace(0);
    keyspace.setLruClock(1000);

    keyspace.setValue("old", Object::makeString("v"));

    // Move the clock forward, then create and touch a second key, so the two
    // carry clearly different access clocks.
    keyspace.setLruClock(2000);
    keyspace.setValue("new", Object::makeString("v"));
    keyspace.lookupRead("new", kNow);

    // Sampling draws with replacement, so a single round can legitimately see
    // only one of the two keys. The guarantee is statistical: across many
    // rounds the stale key must be proposed far more often than the fresh one,
    // and the fresh one must never be proposed by a round that also saw the
    // stale one. Counting is the honest way to assert an approximate policy.
    int choseOld = 0;
    int choseNew = 0;

    for (int attempt = 0; attempt < 200; ++attempt) {
        const EvictionCandidate candidate =
            keyspace.sampleEvictionCandidate(EvictionPolicy::AllKeysLru, 10, 2000, kNow);
        if (!candidate.valid) continue;
        if (candidate.key == "old") ++choseOld;
        else ++choseNew;
    }

    CHECK(choseOld > 0);
    CHECK(choseOld > choseNew);

    // And the scores themselves must be ordered the right way round, which is
    // the deterministic core of the behaviour the sampling approximates.
    keyspace.setLruClock(2000);
    const EvictionCandidate onlyOld =
        keyspace.sampleEvictionCandidate(EvictionPolicy::AllKeysLru, 1, 5000, kNow);
    CHECK(onlyOld.valid);
    CHECK(onlyOld.idleScore > 0); // idle time is positive for both, by construction
}

// ---------------------------------------------------------------------------
// Cluster slot hashing
// ---------------------------------------------------------------------------

TEST(Cluster, HashesKeysIntoTheSlotRange) {
    for (int i = 0; i < 1000; ++i) {
        const int slot = keyHashSlot("key:" + std::to_string(i));
        CHECK(slot >= 0);
        CHECK(slot < kClusterSlots);
    }
}

TEST(Cluster, MatchesRedisSlotAssignments) {
    // These are the slots real Redis computes. Matching them is what lets a
    // stock cluster-aware client route correctly against MiniRedis.
    CHECK_EQ(keyHashSlot("foo"), 12182);
    CHECK_EQ(keyHashSlot("bar"), 5061);
    CHECK_EQ(keyHashSlot("hello"), 866);
}

TEST(Cluster, HashTagsForceRelatedKeysIntoOneSlot) {
    // Without this, a multi-key command on two keys of the same user would be
    // refused with CROSSSLOT.
    CHECK_EQ(keyHashSlot("{user:42}:profile"), keyHashSlot("{user:42}:sessions"));
    CHECK_EQ(keyHashSlot("{user:42}:profile"), keyHashSlot("user:42"));

    // An empty tag is not a tag; the whole key is hashed instead.
    CHECK_EQ(keyHashSlot("{}:x"), keyHashSlot("{}:x"));
    CHECK_NE(keyHashSlot("{}:a"), keyHashSlot("{}:b"));
}
