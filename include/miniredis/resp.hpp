// RESP2 - the REdis Serialization Protocol.
//
// RESP is deliberately tiny: a one-byte type tag, a payload, and CRLF. Five
// types cover the whole protocol.
//
//   +OK\r\n                    simple string
//   -ERR unknown command\r\n   error
//   :42\r\n                    integer
//   $5\r\nhello\r\n            bulk string ($-1\r\n is the null bulk string)
//   *2\r\n$3\r\nGET\r\n$1\r\na\r\n   array (*-1\r\n is the null array)
//
// A client request is always an array of bulk strings. Because MiniRedis speaks
// real RESP2, the stock `redis-cli` connects to it unmodified.
#pragma once

#include "miniredis/common.hpp"

#include <memory>
#include <string>
#include <vector>

namespace miniredis {

// ---------------------------------------------------------------------------
// Writing replies
// ---------------------------------------------------------------------------

// Appends RESP-encoded values onto a caller-owned output buffer. Replies are
// built straight into the connection's write buffer, so no reply object is
// ever allocated on the hot path.
class RespWriter {
public:
    explicit RespWriter(std::string& out) : m_out(out) {}

    void simpleString(std::string_view s);   // +s
    void error(std::string_view s);          // -s
    void integer(std::int64_t v);            // :v
    void bulkString(std::string_view s);     // $len s
    void nullBulkString();                   // $-1
    void nullArray();                        // *-1
    void arrayHeader(std::int64_t count);    // *count
    void ok() { simpleString("OK"); }

    // Convenience for the very common "reply with a flat array of strings".
    void stringArray(const std::vector<std::string>& items);

    // Verbatim bytes, for forwarding an already-encoded payload (replication).
    void raw(std::string_view bytes) { m_out.append(bytes); }

private:
    std::string& m_out;
};

// ---------------------------------------------------------------------------
// Reading requests
// ---------------------------------------------------------------------------

enum class ParseStatus {
    Complete,   // a whole command was parsed
    Incomplete, // need more bytes from the socket
    Invalid,    // protocol violation; the connection must be closed
};

// Incremental request parser.
//
// It is fed whatever bytes happen to arrive and is safe to call on a partial
// command: it reports Incomplete and consumes nothing, so the caller simply
// keeps the buffer and retries after the next read. This is what makes the
// server immune to TCP fragmentation without needing a length-prefixed framing
// layer of its own.
class RespParser {
public:
    // Parses one command from `buffer` starting at `offset`. On Complete,
    // `offset` is advanced past the command and `out` holds its arguments.
    // `error` is filled in on Invalid.
    static ParseStatus parseCommand(const std::string& buffer, size_t& offset, Args& out, std::string& error);

    // Parses a single RESP reply (used by the replica reading from its master
    // and by the benchmark client). `out` receives the reply re-encoded as a
    // flat argument list; nested arrays are flattened.
    static ParseStatus parseReply(const std::string& buffer, size_t& offset, Args& out, std::string& error);

    // Inline commands: a bare "PING\r\n" typed into telnet, with no RESP
    // framing at all. Redis supports these and so does MiniRedis, because it
    // makes the server debuggable with nothing but a terminal.
    static ParseStatus parseInline(const std::string& buffer, size_t& offset, Args& out, std::string& error);

    // The maximum number of elements and the maximum bulk length we accept.
    // Without these a hostile client could send "*999999999\r\n" and make the
    // server try to reserve that many arguments.
    static constexpr std::int64_t kMaxMultiBulk = 1024 * 1024;
    static constexpr std::int64_t kMaxBulkLength = 512LL * 1024 * 1024; // 512 MB, as in Redis
    static constexpr size_t kMaxInlineLength = 64 * 1024;
};

// Encodes an argument vector as a RESP array of bulk strings. Used to write
// commands into the AOF and to propagate them to replicas.
std::string encodeCommand(const Args& args);

} // namespace miniredis
