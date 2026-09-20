// Publish/subscribe.
//
// Two dispatch paths, because they have very different costs:
//
//   SUBSCRIBE news       exact channel  -> hash lookup, O(1) per publish
//   PSUBSCRIBE news.*    glob pattern   -> must be tested against every publish
//
// Redis keeps them in separate structures for exactly that reason, and so does
// MiniRedis: the channel index is a hash from name to subscriber set, while
// patterns are a flat list that PUBLISH walks. A server with many patterns pays
// for them on every publish, which is worth knowing before adding a thousand.
//
// Delivery is fire-and-forget. A message published while a subscriber is
// disconnected is gone; there is no queue and no replay. That is Redis's
// semantic too, and it is the reason pub/sub is not a message queue.
#pragma once

#include "miniredis/common.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace miniredis {

class Server;
struct Client;

class PubSub {
public:
    explicit PubSub(Server& server) : m_server(server) {}

    // Returns the client's new total subscription count, which is what the
    // SUBSCRIBE reply carries.
    size_t subscribeChannel(Client& client, const std::string& channel);
    size_t unsubscribeChannel(Client& client, const std::string& channel);
    size_t subscribePattern(Client& client, const std::string& pattern);
    size_t unsubscribePattern(Client& client, const std::string& pattern);

    // Removes every subscription a client holds. Called on disconnect.
    void unsubscribeAll(Client& client);

    // Delivers to channel subscribers and pattern subscribers. Returns the
    // number of receivers, which is what PUBLISH returns.
    std::int64_t publish(const std::string& channel, const std::string& payload);

    // Introspection for PUBSUB CHANNELS / NUMSUB / NUMPAT.
    std::vector<std::string> channelsMatching(const std::string* pattern) const;
    size_t subscriberCount(const std::string& channel) const;
    size_t patternCount() const { return m_patterns.size(); }

private:
    void sendChannelMessage(Client& client, const std::string& channel, const std::string& payload);
    void sendPatternMessage(Client& client, const std::string& pattern,
                            const std::string& channel, const std::string& payload);

    Server& m_server;

    // channel -> client ids. Client ids rather than pointers, so a client that
    // disconnects mid-publish cannot leave a dangling entry behind.
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> m_channels;
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> m_patterns;
};

} // namespace miniredis
