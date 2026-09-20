#include "miniredis/cluster.hpp"

#include "miniredis/server.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace miniredis {

// ---------------------------------------------------------------------------
// CRC16-CCITT (XModem), as used by Redis Cluster
// ---------------------------------------------------------------------------

namespace {

struct Crc16Table {
    std::uint16_t entries[256];

    Crc16Table() {
        constexpr std::uint16_t kPoly = 0x1021;
        for (int i = 0; i < 256; ++i) {
            std::uint16_t crc = static_cast<std::uint16_t>(i << 8);
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ kPoly)
                                     : static_cast<std::uint16_t>(crc << 1);
            }
            entries[i] = crc;
        }
    }
};

const Crc16Table& crc16Table() {
    static const Crc16Table table;
    return table;
}

} // namespace

std::uint16_t crc16(const char* data, size_t length) {
    const Crc16Table& table = crc16Table();
    std::uint16_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        const std::uint8_t index = static_cast<std::uint8_t>((crc >> 8) ^ static_cast<std::uint8_t>(data[i]));
        crc = static_cast<std::uint16_t>((crc << 8) ^ table.entries[index]);
    }
    return crc;
}

int keyHashSlot(std::string_view key) {
    // Hash tags: if the key contains "{...}" with a non-empty body, hash only
    // what is inside the braces. This is how an application forces keys that
    // must be operated on together -- {user:42}:profile and {user:42}:sessions
    // -- onto the same node, which is what makes multi-key commands usable at
    // all in a cluster.
    const size_t open = key.find('{');
    if (open != std::string_view::npos) {
        const size_t close = key.find('}', open + 1);
        if (close != std::string_view::npos && close != open + 1) {
            const std::string_view tag = key.substr(open + 1, close - open - 1);
            return crc16(tag.data(), tag.size()) & (kClusterSlots - 1);
        }
    }
    return crc16(key.data(), key.size()) & (kClusterSlots - 1);
}

// ---------------------------------------------------------------------------
// ClusterNode
// ---------------------------------------------------------------------------

std::int64_t ClusterNode::slotCount() const {
    std::int64_t total = 0;
    for (const auto& [start, end] : slotRanges) total += end - start + 1;
    return total;
}

// ---------------------------------------------------------------------------
// ClusterState
// ---------------------------------------------------------------------------

bool ClusterState::isEnabled() const {
    return m_server.config().clusterEnabled;
}

void ClusterState::rebuildSlotMap() {
    m_slotOwner.fill(-1);
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        for (const auto& [start, end] : m_nodes[i].slotRanges) {
            for (int slot = start; slot <= end && slot < kClusterSlots; ++slot) {
                if (slot >= 0) m_slotOwner[static_cast<size_t>(slot)] = static_cast<int>(i);
            }
        }
    }
    m_slotMapBuilt = true;
}

void ClusterState::initialiseSingleNode(const std::string& id, const std::string& host, int port) {
    ClusterNode node;
    node.id = id;
    node.host = host;
    node.port = port;
    node.isSelf = true;
    node.isMaster = true;
    node.slotRanges.push_back({0, kClusterSlots - 1});

    m_nodes.clear();
    m_nodes.push_back(std::move(node));
    m_selfId = id;
    rebuildSlotMap();
}

void ClusterState::addNode(ClusterNode node) {
    auto existing = std::find_if(m_nodes.begin(), m_nodes.end(),
                                 [&](const ClusterNode& n) { return n.id == node.id; });
    if (node.isSelf) m_selfId = node.id;

    if (existing != m_nodes.end()) {
        *existing = std::move(node);
    } else {
        m_nodes.push_back(std::move(node));
    }
    rebuildSlotMap();
}

bool ClusterState::assignSlots(const std::string& nodeId, int startSlot, int endSlot, std::string& error) {
    if (startSlot < 0 || endSlot >= kClusterSlots || startSlot > endSlot) {
        error = "ERR Invalid or out of range slot";
        return false;
    }

    auto target = std::find_if(m_nodes.begin(), m_nodes.end(),
                               [&](const ClusterNode& n) { return n.id == nodeId; });
    if (target == m_nodes.end()) {
        error = "ERR Unknown node " + nodeId;
        return false;
    }

    // Taking a slot away from whoever currently holds it keeps the invariant
    // that a slot has exactly one owner. Ranges are rebuilt from a flat slot
    // array afterwards so overlapping assignments cannot accumulate.
    if (!m_slotMapBuilt) rebuildSlotMap();

    std::vector<std::vector<bool>> ownership(m_nodes.size(), std::vector<bool>(kClusterSlots, false));
    for (int slot = 0; slot < kClusterSlots; ++slot) {
        const int owner = m_slotOwner[static_cast<size_t>(slot)];
        if (owner >= 0) ownership[static_cast<size_t>(owner)][static_cast<size_t>(slot)] = true;
    }

    const size_t targetIndex = static_cast<size_t>(std::distance(m_nodes.begin(), target));
    for (int slot = startSlot; slot <= endSlot; ++slot) {
        for (auto& owned : ownership) owned[static_cast<size_t>(slot)] = false;
        ownership[targetIndex][static_cast<size_t>(slot)] = true;
    }

    for (size_t i = 0; i < m_nodes.size(); ++i) {
        m_nodes[i].slotRanges.clear();
        int rangeStart = -1;
        for (int slot = 0; slot <= kClusterSlots; ++slot) {
            const bool owns = (slot < kClusterSlots) && ownership[i][static_cast<size_t>(slot)];
            if (owns && rangeStart < 0) {
                rangeStart = slot;
            } else if (!owns && rangeStart >= 0) {
                m_nodes[i].slotRanges.push_back({rangeStart, slot - 1});
                rangeStart = -1;
            }
        }
    }

    rebuildSlotMap();
    return true;
}

bool ClusterState::forgetNode(const std::string& nodeId) {
    if (nodeId == m_selfId) return false; // a node cannot forget itself

    const size_t before = m_nodes.size();
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(),
                                 [&](const ClusterNode& n) { return n.id == nodeId; }),
                  m_nodes.end());
    if (m_nodes.size() == before) return false;

    rebuildSlotMap();
    return true;
}

const ClusterNode* ClusterState::findNode(const std::string& id) const {
    const auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                                 [&](const ClusterNode& n) { return n.id == id; });
    return it == m_nodes.end() ? nullptr : &*it;
}

const ClusterNode* ClusterState::ownerOfSlot(int slot) const {
    if (slot < 0 || slot >= kClusterSlots) return nullptr;
    if (!m_slotMapBuilt) return nullptr;
    const int index = m_slotOwner[static_cast<size_t>(slot)];
    return index < 0 ? nullptr : &m_nodes[static_cast<size_t>(index)];
}

RoutingDecision ClusterState::route(const std::vector<Bytes>& keys) const {
    RoutingDecision decision;
    if (keys.empty()) return decision; // no keys, always servable locally

    const int firstSlot = keyHashSlot(keys.front());
    decision.slot = firstSlot;

    // Every key must land in the same slot. A node cannot serve MGET across
    // two slots without proxying, and Redis deliberately does not proxy, so
    // the command is refused and the client is expected to split it.
    for (size_t i = 1; i < keys.size(); ++i) {
        if (keyHashSlot(keys[i]) != firstSlot) {
            decision.result = SlotRouting::CrossSlot;
            return decision;
        }
    }

    const ClusterNode* owner = ownerOfSlot(firstSlot);
    if (owner == nullptr) {
        decision.result = SlotRouting::Down;
        return decision;
    }
    if (owner->isSelf) {
        decision.result = SlotRouting::Local;
        return decision;
    }

    decision.result = SlotRouting::Moved;
    decision.targetAddress = owner->address();
    return decision;
}

int ClusterState::assignedSlotCount() const {
    if (!m_slotMapBuilt) return 0;
    int total = 0;
    for (int slot = 0; slot < kClusterSlots; ++slot) {
        if (m_slotOwner[static_cast<size_t>(slot)] >= 0) ++total;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Config file
// ---------------------------------------------------------------------------

bool ClusterState::loadConfig(const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cluster config not found: " + path;
        return false;
    }

    std::vector<ClusterNode> loaded;
    std::string line;
    int lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Format: <id> <host> <port> <master|replica[:masterId]> [slot|start-end]...
        std::istringstream stream(trimmed);
        ClusterNode node;
        std::string role;
        if (!(stream >> node.id >> node.host >> node.port >> role)) {
            error = "malformed cluster config at line " + std::to_string(lineNumber);
            return false;
        }

        const size_t colon = role.find(':');
        if (colon != std::string::npos) {
            node.masterId = role.substr(colon + 1);
            role = role.substr(0, colon);
        }
        node.isMaster = equalsIgnoreCase(role, "master");

        std::string token;
        while (stream >> token) {
            if (equalsIgnoreCase(token, "myself")) {
                node.isSelf = true;
                continue;
            }
            const size_t dash = token.find('-');
            std::int64_t start = 0;
            std::int64_t end = 0;
            if (dash == std::string::npos) {
                if (!parseInt64(token, start)) continue;
                end = start;
            } else {
                if (!parseInt64(token.substr(0, dash), start)) continue;
                if (!parseInt64(token.substr(dash + 1), end)) continue;
            }
            if (start < 0 || end >= kClusterSlots || start > end) {
                error = "cluster config line " + std::to_string(lineNumber) +
                        " has an out-of-range slot range";
                return false;
            }
            node.slotRanges.push_back({static_cast<int>(start), static_cast<int>(end)});
        }

        loaded.push_back(std::move(node));
    }

    if (loaded.empty()) {
        error = "cluster config contains no nodes";
        return false;
    }

    m_nodes = std::move(loaded);
    m_selfId.clear();
    for (const auto& node : m_nodes) {
        if (node.isSelf) m_selfId = node.id;
    }
    if (m_selfId.empty()) {
        error = "cluster config does not mark any node as 'myself'";
        return false;
    }

    rebuildSlotMap();
    return true;
}

bool ClusterState::saveConfig(const std::string& path, std::string& error) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        error = "cannot write cluster config: " + path;
        return false;
    }

    file << "# MiniRedis cluster configuration\n";
    file << "# <id> <host> <port> <master|replica:masterId> [myself] [slot|start-end]...\n";

    for (const auto& node : m_nodes) {
        file << node.id << ' ' << node.host << ' ' << node.port << ' ';
        if (node.isMaster) {
            file << "master";
        } else {
            file << "replica";
            if (!node.masterId.empty()) file << ':' << node.masterId;
        }
        if (node.isSelf) file << " myself";
        for (const auto& [start, end] : node.slotRanges) {
            file << ' ' << start;
            if (end != start) file << '-' << end;
        }
        file << '\n';
    }
    return true;
}

std::string ClusterState::describeNodes() const {
    std::string out;
    for (const auto& node : m_nodes) {
        out += node.id;
        out += ' ';
        out += node.address();
        out += ' ';
        if (node.isSelf) out += "myself,";
        out += node.isMaster ? "master" : "slave";
        out += ' ';
        out += node.masterId.empty() ? "-" : node.masterId;
        out += " 0 0 0 connected";
        for (const auto& [start, end] : node.slotRanges) {
            out += ' ';
            out += formatInt64(start);
            if (end != start) {
                out += '-';
                out += formatInt64(end);
            }
        }
        out += '\n';
    }
    return out;
}

std::string ClusterState::describeInfo() const {
    const int assigned = assignedSlotCount();
    std::string out;
    out += std::string("cluster_enabled:") + (isEnabled() ? "1" : "0") + "\r\n";
    out += std::string("cluster_state:") + (isFullyCovered() ? "ok" : "fail") + "\r\n";
    out += "cluster_slots_assigned:" + formatInt64(assigned) + "\r\n";
    out += "cluster_slots_ok:" + formatInt64(assigned) + "\r\n";
    out += "cluster_slots_pfail:0\r\n";
    out += "cluster_slots_fail:0\r\n";
    out += "cluster_known_nodes:" + formatInt64(static_cast<std::int64_t>(m_nodes.size())) + "\r\n";

    std::int64_t masters = 0;
    for (const auto& node : m_nodes) {
        if (node.isMaster) ++masters;
    }
    out += "cluster_size:" + formatInt64(masters) + "\r\n";
    return out;
}

} // namespace miniredis
