// The append-only file: durability by logging commands.
//
// The AOF is the evolution of the write-ahead log this project started with,
// with three differences that matter:
//
//  1. Records are RESP-encoded commands, not a bespoke text format. Replaying
//     the AOF is literally feeding the file back through the command
//     dispatcher, so there is no second parser to keep in sync with the first
//     and no escaping scheme to get wrong. The original WAL's space-escaping
//     was a whole class of bugs that simply does not exist here.
//
//  2. The fsync policy is explicit. "Written" and "durable" are different
//     things: write() only reaches the OS page cache, and a machine that loses
//     power then loses everything still in it. Redis exposes three points on
//     that curve and so do we.
//
//  3. It rewrites itself. A log of every command ever run grows without bound,
//     so periodically it is replaced by the shortest command sequence that
//     reproduces the current dataset -- one SET per key rather than the
//     thousand INCRs that produced it.
#pragma once

#include "miniredis/common.hpp"
#include "miniredis/server.hpp"

#include <cstdio>
#include <string>

namespace miniredis {

class Server;

class Aof {
public:
    explicit Aof(Server& server);
    ~Aof();

    bool isEnabled() const { return m_enabled; }

    // Opens the file for appending. Safe to call when already open.
    bool enable(std::string& error);
    void disable();

    // Buffers one command. Nothing reaches the file until flush(), which the
    // event loop calls once per iteration -- so a pipeline of a thousand
    // commands costs one write(), not a thousand.
    void feed(int dbIndex, const Args& args);

    // Writes the buffer to the file and applies the fsync policy.
    void flush(Millis now);

    // Replays the file through the command dispatcher. Returns false only on a
    // corrupt file; a truncated tail (the normal result of a crash mid-write)
    // is reported through `truncated` and treated as recoverable.
    bool load(const std::string& path, bool& truncated, std::string& error);

    // Rewrites the file as the minimal command sequence that reproduces the
    // current dataset, then swaps it into place atomically.
    bool rewrite(std::string& error);

    std::int64_t currentSizeBytes() const { return m_currentSize; }
    std::int64_t sizeAtLastRewrite() const { return m_sizeAtLastRewrite; }
    bool rewriteInProgress() const { return m_rewriteInProgress; }
    Millis lastFsyncTime() const { return m_lastFsync; }
    std::uint64_t rewriteCount() const { return m_rewriteCount; }
    bool lastWriteFailed() const { return m_lastWriteFailed; }

    // True when the configured auto-rewrite thresholds have been crossed.
    bool shouldAutoRewrite() const;

private:
    // Emits the commands that recreate one key, including its TTL.
    static void emitKeyCommands(const Bytes& key, const Object& value, Millis expireAt,
                                std::string& out);

    Server& m_server;
    std::FILE* m_file = nullptr;
    bool m_enabled = false;

    std::string m_buffer;
    int m_lastSelectedDb = -1; // so a SELECT is only emitted when the db changes

    std::int64_t m_currentSize = 0;
    std::int64_t m_sizeAtLastRewrite = 0;
    Millis m_lastFsync = 0;
    bool m_rewriteInProgress = false;
    bool m_lastWriteFailed = false;
    std::uint64_t m_rewriteCount = 0;
};

} // namespace miniredis
