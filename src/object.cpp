#include "miniredis/object.hpp"

#include <algorithm>
#include <cmath>

namespace miniredis {

const char* objectTypeName(ObjectType type) {
    switch (type) {
        case ObjectType::String: return "string";
        case ObjectType::List:   return "list";
        case ObjectType::Hash:   return "hash";
        case ObjectType::Set:    return "set";
        case ObjectType::ZSet:   return "zset";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// ZSet
// ---------------------------------------------------------------------------

bool ZSet::add(const Bytes& member, double score) {
    auto it = m_scores.find(member);
    if (it != m_scores.end()) {
        if (it->second == score) return false; // no change, and not an insert
        // The ordered index is keyed on the score, so a score change means
        // erase-then-reinsert rather than an in-place update.
        m_sorted.erase(Entry{it->second, member});
        it->second = score;
        m_sorted.insert(Entry{score, member});
        return false;
    }

    m_scores.emplace(member, score);
    m_sorted.insert(Entry{score, member});
    return true;
}

bool ZSet::remove(const Bytes& member) {
    auto it = m_scores.find(member);
    if (it == m_scores.end()) return false;
    m_sorted.erase(Entry{it->second, member});
    m_scores.erase(it);
    return true;
}

bool ZSet::score(const Bytes& member, double& out) const {
    auto it = m_scores.find(member);
    if (it == m_scores.end()) return false;
    out = it->second;
    return true;
}

std::int64_t ZSet::rank(const Bytes& member, bool reverse) const {
    auto it = m_scores.find(member);
    if (it == m_scores.end()) return -1;

    // std::set has no O(log n) order statistic, so the rank scan is linear.
    // Redis's skip list carries span counters that make this O(log n); the
    // trade-off is documented here because ZRANK is the one operation where
    // the simpler structure is asymptotically worse.
    const auto position = m_sorted.find(Entry{it->second, member});
    const auto ascending = static_cast<std::int64_t>(std::distance(m_sorted.begin(), position));
    return reverse ? static_cast<std::int64_t>(m_sorted.size()) - 1 - ascending : ascending;
}

std::vector<ZSet::Entry> ZSet::rangeByScore(double min, bool minExclusive,
                                            double max, bool maxExclusive) const {
    std::vector<Entry> result;
    if (min > max) return result;

    // Seek to the first entry that could possibly qualify. Pairing the score
    // with an empty member gives the lowest key at that score.
    auto it = m_sorted.lower_bound(Entry{min, Bytes{}});
    for (; it != m_sorted.end(); ++it) {
        const double score = it->first;
        if (score > max || (maxExclusive && score == max)) break;
        if (minExclusive && score == min) continue;
        if (score < min) continue;
        result.push_back(*it);
    }
    return result;
}

size_t ZSet::memoryUsage() const {
    size_t total = 0;
    for (const auto& [member, score] : m_scores) {
        (void)score;
        // Counted twice: the member string is present in both indexes.
        total += 2 * (member.size() + sizeof(Bytes)) + 2 * sizeof(double) + 64;
    }
    return total + sizeof(ZSet);
}

// ---------------------------------------------------------------------------
// Object
// ---------------------------------------------------------------------------

Object Object::makeString(Bytes value) {
    Object object;
    object.m_storage = std::move(value);
    return object;
}

Object Object::makeList() {
    Object object;
    object.m_storage = ListValue{};
    return object;
}

Object Object::makeHash() {
    Object object;
    object.m_storage = HashValue{};
    return object;
}

Object Object::makeSet() {
    Object object;
    object.m_storage = SetValue{};
    return object;
}

Object Object::makeZSet() {
    Object object;
    object.m_storage = ZSet{};
    return object;
}

ObjectType Object::type() const {
    switch (m_storage.index()) {
        case 0: return ObjectType::String;
        case 1: return ObjectType::List;
        case 2: return ObjectType::Hash;
        case 3: return ObjectType::Set;
        default: return ObjectType::ZSet;
    }
}

size_t Object::length() const {
    switch (type()) {
        case ObjectType::String: return string().size();
        case ObjectType::List:   return list().size();
        case ObjectType::Hash:   return hash().size();
        case ObjectType::Set:    return set().size();
        case ObjectType::ZSet:   return zset().size();
    }
    return 0;
}

bool Object::isEmptyContainer() const {
    switch (type()) {
        case ObjectType::String: return false; // an empty string is a real value
        case ObjectType::List:   return list().empty();
        case ObjectType::Hash:   return hash().empty();
        case ObjectType::Set:    return set().empty();
        case ObjectType::ZSet:   return zset().empty();
    }
    return false;
}

size_t Object::memoryUsage() const {
    // Per-element overheads are rough approximations of what libstdc++ costs:
    // a std::string header plus its buffer, and a node allocation for each
    // entry in a node-based container.
    constexpr size_t kNodeOverhead = 48;

    switch (type()) {
        case ObjectType::String:
            return sizeof(Object) + sizeof(Bytes) + string().capacity();

        case ObjectType::List: {
            size_t total = sizeof(Object) + sizeof(ListValue);
            for (const auto& item : list()) total += item.size() + sizeof(Bytes);
            return total;
        }

        case ObjectType::Hash: {
            size_t total = sizeof(Object) + sizeof(HashValue);
            for (const auto& [field, value] : hash()) {
                total += field.size() + value.size() + 2 * sizeof(Bytes) + kNodeOverhead;
            }
            return total;
        }

        case ObjectType::Set: {
            size_t total = sizeof(Object) + sizeof(SetValue);
            for (const auto& member : set()) {
                total += member.size() + sizeof(Bytes) + kNodeOverhead;
            }
            return total;
        }

        case ObjectType::ZSet:
            return sizeof(Object) + zset().memoryUsage();
    }
    return sizeof(Object);
}

} // namespace miniredis
