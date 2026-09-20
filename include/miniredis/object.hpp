// The value side of the keyspace: Redis's five core data types.
//
// Real Redis picks a different *encoding* per type depending on size (a small
// hash is a flat "listpack" array, a large one is a hash table) purely to save
// memory. MiniRedis keeps one straightforward encoding per type so the data
// structures stay readable, and documents the trade-off rather than hiding it.
#pragma once

#include "miniredis/common.hpp"

#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace miniredis {

enum class ObjectType {
    String,
    List,
    Hash,
    Set,
    ZSet,
};

const char* objectTypeName(ObjectType type);

// ---------------------------------------------------------------------------
// Sorted set
// ---------------------------------------------------------------------------

// A sorted set has to answer two questions efficiently:
//
//   ZSCORE member  -> score          (a hash lookup)
//   ZRANGE 0 10    -> members in score order  (an ordered traversal)
//
// Redis keeps a hash table alongside a skip list for exactly this reason.
// MiniRedis uses the same two-index idea with an ordered std::set, which is a
// balanced tree: O(log n) for insert, erase and rank scans, versus the skip
// list's expected O(log n). The observable behaviour is identical.
//
// Ties are broken lexicographically by member, which is what Redis guarantees
// and what makes ZRANGEBYLEX well defined.
class ZSet {
public:
    using Entry = std::pair<double, Bytes>; // (score, member), ordered by both

    // Returns true when the member was newly added rather than updated.
    bool add(const Bytes& member, double score);
    bool remove(const Bytes& member);
    bool score(const Bytes& member, double& out) const;

    // Zero-based position in ascending score order, or -1 when absent.
    std::int64_t rank(const Bytes& member, bool reverse) const;

    size_t size() const { return m_scores.size(); }
    bool empty() const { return m_scores.empty(); }

    // Ordered view, ascending by (score, member).
    const std::set<Entry>& ordered() const { return m_sorted; }

    // Members whose score falls in [min, max], honouring exclusive bounds.
    std::vector<Entry> rangeByScore(double min, bool minExclusive,
                                    double max, bool maxExclusive) const;

    size_t memoryUsage() const;

private:
    std::unordered_map<Bytes, double> m_scores; // member -> score
    std::set<Entry> m_sorted;                   // (score, member) ordered
};

// ---------------------------------------------------------------------------
// Object
// ---------------------------------------------------------------------------

using ListValue = std::deque<Bytes>;                  // O(1) push/pop at both ends
using HashValue = std::unordered_map<Bytes, Bytes>;
using SetValue = std::unordered_set<Bytes>;

class Object {
public:
    Object() : m_storage(Bytes{}) {}

    static Object makeString(Bytes value);
    static Object makeList();
    static Object makeHash();
    static Object makeSet();
    static Object makeZSet();

    ObjectType type() const;
    const char* typeName() const { return objectTypeName(type()); }

    // Typed accessors. Calling the wrong one is a programming error; command
    // implementations always check type() (via the WRONGTYPE guard) first.
    Bytes& string() { return std::get<Bytes>(m_storage); }
    const Bytes& string() const { return std::get<Bytes>(m_storage); }
    ListValue& list() { return std::get<ListValue>(m_storage); }
    const ListValue& list() const { return std::get<ListValue>(m_storage); }
    HashValue& hash() { return std::get<HashValue>(m_storage); }
    const HashValue& hash() const { return std::get<HashValue>(m_storage); }
    SetValue& set() { return std::get<SetValue>(m_storage); }
    const SetValue& set() const { return std::get<SetValue>(m_storage); }
    ZSet& zset() { return std::get<ZSet>(m_storage); }
    const ZSet& zset() const { return std::get<ZSet>(m_storage); }

    // Element count: string length for strings, cardinality for containers.
    // A container that reaches zero elements is deleted from the keyspace,
    // matching Redis, so that "empty list" and "missing key" are the same.
    size_t length() const;
    bool isEmptyContainer() const;

    // Approximate heap footprint in bytes. Eviction needs a number to compare
    // against maxmemory; it does not need to be exact, but it does need to
    // move in the right direction as a value grows.
    size_t memoryUsage() const;

private:
    std::variant<Bytes, ListValue, HashValue, SetValue, ZSet> m_storage;
};

} // namespace miniredis
