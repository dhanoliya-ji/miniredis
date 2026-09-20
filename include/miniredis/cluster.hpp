// Cluster mode: sharding the keyspace across nodes.
//
// Replication makes copies of one dataset; it does not make the dataset
// bigger. Once the data no longer fits on one machine, the keyspace has to be
// split, and the split has to be something every client and every node can
// compute independently -- otherwise every request needs a lookup service.
//
// Redis's answer is a fixed grid of 16384 hash slots:
//
//     slot = CRC16(key) mod 16384
//
// Slots, not keys, are assigned to nodes. That indirection is the whole trick:
// moving data between nodes means reassigning a slot range, and the number of
// slots never changes, so no key ever has to be rehashed. Compare consistent
// hashing on a ring, where adding a node moves an arbitrary fraction of keys.
//
// A node that is asked for a key outside its own slots does not proxy the
// request. It replies:
//
//     -MOVED 3999 127.0.0.1:6381
//
// and the client retries there, caching the mapping. The cluster therefore
// costs one extra round trip only when a client's map is stale.
//
// Multi-key commands are the catch. MGET a b c can only be served locally if
// every key lives in the same slot, so a command whose keys straddle slots is
// rejected with CROSSSLOT. Hash tags -- the {user1} in {user1}:profile -- exist
// to force related keys into the same slot on purpose.
#pragma once

#include "miniredis/common.hpp"

#include <array>
#include <string>
#include <vector>

namespace miniredis {

class Server;

inline constexpr int kClusterSlots = 16384;

// CRC16-CCITT, the exact variant Redis uses for slot assignment. It is here so
// that a MiniRedis cluster and a Redis cluster compute the same slot for the
// same key, which means real cluster-aware clients work unmodified.
std::uint16_t crc16(const char* data, size_t length);

// Computes the slot for a key, honouring hash tags. When a key contains
// "{...}" with a non-empty body, only the text between the first '{' and the
// first '}' after it is hashed.
int keyHashSlot(std::string_view key);

// One node in the cluster's view of the world.
struct ClusterNode {
    std::string id;
    std::string host;
    int port = 0;
    bool isSelf = false;
    bool isMaster = true;
    std::string masterId;          // set when this node is a replica
    std::vector<std::pair<int, int>> slotRanges; // inclusive [start, end]

    std::string address() const { return host + ":" + std::to_string(port); }
    std::int64_t slotCount() const;
};

enum class SlotRouting {
    Local,      // this node owns the slot
    Moved,      // another node owns it; reply -MOVED
    CrossSlot,  // the command's keys span slots
    Down,       // the slot is unassigned, so the cluster is not fully covered
};

struct RoutingDecision {
    SlotRouting result = SlotRouting::Local;
    int slot = -1;
    std::string targetAddress; // filled in for Moved
};

class ClusterState {
public:
    explicit ClusterState(Server& server) : m_server(server) {}

    bool isEnabled() const;

    // Loads the nodes file: one "id host port master|replica slots..." line
    // per node. Absent file means a single-node cluster owning every slot.
    bool loadConfig(const std::string& path, std::string& error);
    bool saveConfig(const std::string& path, std::string& error) const;

    // Declares this node and gives it the full slot range. Used when cluster
    // mode is on but no config file exists yet.
    void initialiseSingleNode(const std::string& id, const std::string& host, int port);

    void addNode(ClusterNode node);
    bool assignSlots(const std::string& nodeId, int startSlot, int endSlot, std::string& error);
    bool forgetNode(const std::string& nodeId);

    const ClusterNode* self() const { return m_selfId.empty() ? nullptr : findNode(m_selfId); }
    const ClusterNode* findNode(const std::string& id) const;
    const ClusterNode* ownerOfSlot(int slot) const;
    const std::vector<ClusterNode>& nodes() const { return m_nodes; }

    // Decides where a command's keys should be served. `keys` comes from the
    // command table's key specification.
    RoutingDecision route(const std::vector<Bytes>& keys) const;

    int assignedSlotCount() const;
    bool isFullyCovered() const { return assignedSlotCount() == kClusterSlots; }

    // The text CLUSTER NODES returns: one line per node.
    std::string describeNodes() const;
    std::string describeInfo() const;

private:
    void rebuildSlotMap();

    Server& m_server;
    std::vector<ClusterNode> m_nodes;
    std::string m_selfId;

    // slot -> index into m_nodes, or -1 when unassigned. A flat array rather
    // than a map, because this is consulted on every single command.
    std::array<int, kClusterSlots> m_slotOwner{};
    bool m_slotMapBuilt = false;
};

} // namespace miniredis
