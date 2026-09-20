// The command table: one entry per command, looked up by name.
//
// Redis routes every request through a table of command descriptors rather than
// a switch statement, because the metadata in that table is what the rest of
// the server reasons about: which commands write (so they must be refused on a
// read-only replica, logged to the AOF and shipped to replicas), which may
// allocate (so they must be refused when memory is exhausted), and where the
// keys sit in the argument list (so cluster mode can find the hash slot).
// MiniRedis uses the same design for the same reasons.
#pragma once

#include "miniredis/common.hpp"
#include "miniredis/resp.hpp"

#include <functional>
#include <string>
#include <unordered_map>

namespace miniredis {

class Server;
struct Client;

// Command flags, combined as a bitmask.
namespace cmdflag {
inline constexpr std::uint32_t kWrite     = 1u << 0; // mutates the keyspace
inline constexpr std::uint32_t kReadOnly  = 1u << 1;
inline constexpr std::uint32_t kAdmin     = 1u << 2; // CONFIG, SHUTDOWN, DEBUG
inline constexpr std::uint32_t kDenyOom   = 1u << 3; // refuse when over maxmemory
inline constexpr std::uint32_t kNoMulti   = 1u << 4; // cannot be queued in a transaction
inline constexpr std::uint32_t kPubSub    = 1u << 5; // allowed while in subscribe mode
inline constexpr std::uint32_t kNoAuth    = 1u << 6; // usable before AUTH succeeds
inline constexpr std::uint32_t kFast      = 1u << 7; // O(1); used by the slow-log budget
inline constexpr std::uint32_t kLoading   = 1u << 8; // usable while loading a dataset
} // namespace cmdflag

// Everything a command implementation is handed.
struct CommandContext {
    CommandContext(Server& serverRef, Client& clientRef, const Args& argsRef,
                   std::string& outRef, Millis nowValue)
        : server(serverRef), client(clientRef), args(argsRef), out(outRef), now(nowValue) {}

    Server& server;
    Client& client;
    const Args& args;    // args[0] is the command name as the client spelled it
    std::string& out;    // the client's output buffer; write the reply here
    Millis now;          // one consistent instant for the whole command

    RespWriter reply() { return RespWriter(out); }

    // Replaces what gets written to the AOF and sent to replicas.
    //
    // Some commands are not deterministic, so replaying the client's own text
    // would diverge: SPOP picks a random member, EXPIRE is relative to the
    // master's clock. Those rewrite themselves into a deterministic form
    // (SREM of the member actually popped, PEXPIREAT with an absolute
    // timestamp) before propagation.
    void propagateInstead(Args replacement) {
        propagationOverride = std::move(replacement);
        hasPropagationOverride = true;
    }

    // Suppresses propagation entirely, for a write command that turned out to
    // be a no-op.
    void suppressPropagation() { propagationSuppressed = true; }

    Args propagationOverride;
    bool hasPropagationOverride = false;
    bool propagationSuppressed = false;
};

using CommandHandler = std::function<void(CommandContext&)>;

struct CommandSpec {
    std::string name;
    CommandHandler handler;

    // Expected argument count including the command name. A negative value
    // means "at least |arity|", which covers variadic commands like MSET.
    int arity = 0;

    std::uint32_t flags = 0;

    // Where the keys live in the argument vector, for cluster slot checking.
    // firstKey == 0 means the command takes no keys.
    int firstKey = 0;
    int lastKey = 0;   // negative counts back from the end
    int keyStep = 1;

    bool isWrite() const { return (flags & cmdflag::kWrite) != 0; }
    bool matchesArity(size_t argc) const {
        return arity >= 0 ? static_cast<int>(argc) == arity
                          : static_cast<int>(argc) >= -arity;
    }
};

class CommandTable {
public:
    void add(CommandSpec spec);

    // Commands are matched case-insensitively, as in Redis: `get`, `GET` and
    // `Get` are the same command.
    const CommandSpec* find(std::string_view name) const;

    size_t size() const { return m_commands.size(); }
    const std::unordered_map<std::string, CommandSpec>& all() const { return m_commands; }

    // Extracts the key arguments of a command, for cluster slot checks and for
    // the WATCH bookkeeping.
    std::vector<Bytes> extractKeys(const CommandSpec& spec, const Args& args) const;

private:
    std::unordered_map<std::string, CommandSpec> m_commands; // keyed by lower-case name
};

// Each family of commands lives in its own translation unit and is registered
// explicitly here rather than through a static initialiser. Self-registration
// looks tidier but breaks in a static library: the linker drops an object file
// nothing references, taking its registrations with it.
void registerConnectionCommands(CommandTable& table);
void registerKeyCommands(CommandTable& table);
void registerStringCommands(CommandTable& table);
void registerListCommands(CommandTable& table);
void registerHashCommands(CommandTable& table);
void registerSetCommands(CommandTable& table);
void registerZSetCommands(CommandTable& table);
void registerServerCommands(CommandTable& table);
void registerPubSubCommands(CommandTable& table);
void registerTransactionCommands(CommandTable& table);
void registerPersistenceCommands(CommandTable& table);
void registerReplicationCommands(CommandTable& table);
void registerClusterCommands(CommandTable& table);
void registerSqlCommands(CommandTable& table);

CommandTable buildCommandTable();

// ---------------------------------------------------------------------------
// Shared reply helpers
// ---------------------------------------------------------------------------

namespace err {
inline constexpr const char* kWrongType =
    "WRONGTYPE Operation against a key holding the wrong kind of value";
inline constexpr const char* kNotInteger = "ERR value is not an integer or out of range";
inline constexpr const char* kNotFloat = "ERR value is not a valid float";
inline constexpr const char* kSyntax = "ERR syntax error";
inline constexpr const char* kIndexOutOfRange = "ERR index out of range";
inline constexpr const char* kNoSuchKey = "ERR no such key";
inline constexpr const char* kOutOfRange = "ERR value is out of range, must be positive";
inline constexpr const char* kOom =
    "OOM command not allowed when used memory > 'maxmemory'";
inline constexpr const char* kReadOnlyReplica =
    "READONLY You can't write against a read only replica";
} // namespace err

std::string wrongArgsError(std::string_view commandName);

} // namespace miniredis
