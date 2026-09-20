#include "miniredis/pubsub.hpp"

#include "miniredis/resp.hpp"
#include "miniredis/server.hpp"

#include <algorithm>

namespace miniredis {

namespace {

// A pub/sub delivery is a three-element array, and a pattern delivery is a
// four-element one carrying the pattern that matched.
void writeMessage(std::string& out, std::string_view kind, std::string_view channel,
                  std::string_view payload) {
    RespWriter writer(out);
    writer.arrayHeader(3);
    writer.bulkString(kind);
    writer.bulkString(channel);
    writer.bulkString(payload);
}

void writePatternMessage(std::string& out, std::string_view pattern, std::string_view channel,
                         std::string_view payload) {
    RespWriter writer(out);
    writer.arrayHeader(4);
    writer.bulkString("pmessage");
    writer.bulkString(pattern);
    writer.bulkString(channel);
    writer.bulkString(payload);
}

} // namespace

size_t PubSub::subscribeChannel(Client& client, const std::string& channel) {
    if (client.subscribedChannels.insert(channel).second) {
        m_channels[channel].insert(client.id);
    }
    return client.subscribedChannels.size() + client.subscribedPatterns.size();
}

size_t PubSub::unsubscribeChannel(Client& client, const std::string& channel) {
    if (client.subscribedChannels.erase(channel) > 0) {
        auto it = m_channels.find(channel);
        if (it != m_channels.end()) {
            it->second.erase(client.id);
            // Drop the channel entirely once nobody is listening, so an idle
            // server does not accumulate an entry per channel ever used.
            if (it->second.empty()) m_channels.erase(it);
        }
    }
    return client.subscribedChannels.size() + client.subscribedPatterns.size();
}

size_t PubSub::subscribePattern(Client& client, const std::string& pattern) {
    if (client.subscribedPatterns.insert(pattern).second) {
        m_patterns[pattern].insert(client.id);
    }
    return client.subscribedChannels.size() + client.subscribedPatterns.size();
}

size_t PubSub::unsubscribePattern(Client& client, const std::string& pattern) {
    if (client.subscribedPatterns.erase(pattern) > 0) {
        auto it = m_patterns.find(pattern);
        if (it != m_patterns.end()) {
            it->second.erase(client.id);
            if (it->second.empty()) m_patterns.erase(it);
        }
    }
    return client.subscribedChannels.size() + client.subscribedPatterns.size();
}

void PubSub::unsubscribeAll(Client& client) {
    for (const auto& channel : client.subscribedChannels) {
        auto it = m_channels.find(channel);
        if (it != m_channels.end()) {
            it->second.erase(client.id);
            if (it->second.empty()) m_channels.erase(it);
        }
    }
    for (const auto& pattern : client.subscribedPatterns) {
        auto it = m_patterns.find(pattern);
        if (it != m_patterns.end()) {
            it->second.erase(client.id);
            if (it->second.empty()) m_patterns.erase(it);
        }
    }
    client.subscribedChannels.clear();
    client.subscribedPatterns.clear();
}

void PubSub::sendChannelMessage(Client& client, const std::string& channel, const std::string& payload) {
    std::string out;
    writeMessage(out, "message", channel, payload);
    m_server.addReply(client, out);
}

void PubSub::sendPatternMessage(Client& client, const std::string& pattern,
                                const std::string& channel, const std::string& payload) {
    std::string out;
    writePatternMessage(out, pattern, channel, payload);
    m_server.addReply(client, out);
}

std::int64_t PubSub::publish(const std::string& channel, const std::string& payload) {
    std::int64_t receivers = 0;

    // Exact-channel subscribers: one hash lookup regardless of how many
    // channels exist.
    auto channelIt = m_channels.find(channel);
    if (channelIt != m_channels.end()) {
        // Copy the id set before delivering. addReply can mark a client for
        // closing, and a later iteration would then be walking a set that the
        // disconnect path has already modified.
        const std::vector<std::uint64_t> ids(channelIt->second.begin(), channelIt->second.end());
        for (const std::uint64_t id : ids) {
            if (Client* subscriber = m_server.findClient(id)) {
                sendChannelMessage(*subscriber, channel, payload);
                ++receivers;
            }
        }
    }

    // Pattern subscribers: every registered pattern must be tested, so this
    // is O(patterns) per publish.
    for (const auto& [pattern, ids] : m_patterns) {
        if (!globMatch(pattern, channel)) continue;
        const std::vector<std::uint64_t> snapshot(ids.begin(), ids.end());
        for (const std::uint64_t id : snapshot) {
            if (Client* subscriber = m_server.findClient(id)) {
                sendPatternMessage(*subscriber, pattern, channel, payload);
                ++receivers;
            }
        }
    }

    m_server.stats().pubsubMessages += static_cast<std::uint64_t>(receivers);
    return receivers;
}

std::vector<std::string> PubSub::channelsMatching(const std::string* pattern) const {
    std::vector<std::string> result;
    result.reserve(m_channels.size());
    for (const auto& [channel, ids] : m_channels) {
        if (ids.empty()) continue;
        if (pattern != nullptr && !globMatch(*pattern, channel)) continue;
        result.push_back(channel);
    }
    std::sort(result.begin(), result.end());
    return result;
}

size_t PubSub::subscriberCount(const std::string& channel) const {
    const auto it = m_channels.find(channel);
    return it == m_channels.end() ? 0 : it->second.size();
}

} // namespace miniredis
