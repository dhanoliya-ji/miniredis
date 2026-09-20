// The RDB snapshot: durability by point-in-time image.
//
// The AOF and the RDB solve the same problem from opposite ends, and a real
// deployment usually runs both:
//
//              AOF (command log)          RDB (snapshot)
//   loss       up to one fsync interval   up to one save interval
//   file size  grows with writes          proportional to the dataset
//   restart    replay every command       one sequential read
//   cost       a little on every write    a burst when it runs
//
// So the AOF is the durability story and the RDB is the restart-time and
// backup story. A 10 GB dataset built from a billion commands reloads from an
// RDB in seconds and from an AOF in a very long time.
//
// The format here is a self-describing binary layout with a length-prefixed
// encoding for every string, a version number so a future change can be
// detected rather than misread, and a CRC64 trailer so a truncated or
// corrupted file is rejected instead of silently loading half a dataset.
#pragma once

#include "miniredis/common.hpp"
#include "miniredis/object.hpp"

#include <cstdio>
#include <string>

namespace miniredis {

class Server;

// On-disk type tags. Explicit values, because these are written to a file and
// must not shift if the enum is ever reordered.
enum class RdbType : std::uint8_t {
    String = 0,
    List = 1,
    Hash = 2,
    Set = 3,
    ZSet = 4,
};

// Opcodes that appear where a type tag would.
enum class RdbOpcode : std::uint8_t {
    ExpireMs = 0xFC,  // an 8-byte absolute expiry precedes the next key
    SelectDb = 0xFE,
    Eof = 0xFF,
};

class RdbFile {
public:
    explicit RdbFile(Server& server) : m_server(server) {}

    // Writes every database to `path` via a temporary file and an atomic
    // rename, so an interrupted save can never leave a half-written snapshot
    // where the real one used to be.
    bool save(const std::string& path, std::string& error);

    // Loads a snapshot, replacing the current dataset. Keys whose expiry has
    // already passed are skipped rather than loaded and immediately expired.
    bool load(const std::string& path, std::string& error);

    std::uint64_t keysWritten() const { return m_keysWritten; }
    std::uint64_t keysLoaded() const { return m_keysLoaded; }
    std::uint64_t keysSkippedExpired() const { return m_keysSkippedExpired; }

    // CRC64 (Jones polynomial), the same check Redis appends to its own RDBs.
    static std::uint64_t crc64(std::uint64_t seed, const char* data, size_t length);

    static constexpr char kMagic[] = "MINIREDIS";
    static constexpr std::uint16_t kVersion = 1;

private:
    Server& m_server;
    std::uint64_t m_keysWritten = 0;
    std::uint64_t m_keysLoaded = 0;
    std::uint64_t m_keysSkippedExpired = 0;
};

// ---------------------------------------------------------------------------
// Buffer encoding helpers, shared with replication (a full resync ships an
// RDB payload over the socket rather than through a file).
// ---------------------------------------------------------------------------

void rdbWriteByte(std::string& out, std::uint8_t value);
void rdbWriteUint32(std::string& out, std::uint32_t value);
void rdbWriteUint64(std::string& out, std::uint64_t value);
void rdbWriteInt64(std::string& out, std::int64_t value);
void rdbWriteDouble(std::string& out, double value);
void rdbWriteString(std::string& out, std::string_view value);

bool rdbReadByte(const std::string& in, size_t& offset, std::uint8_t& value);
bool rdbReadUint32(const std::string& in, size_t& offset, std::uint32_t& value);
bool rdbReadUint64(const std::string& in, size_t& offset, std::uint64_t& value);
bool rdbReadInt64(const std::string& in, size_t& offset, std::int64_t& value);
bool rdbReadDouble(const std::string& in, size_t& offset, double& value);
bool rdbReadString(const std::string& in, size_t& offset, std::string& value);

// Serialises one value (without its key or TTL) and reads it back.
void rdbWriteObject(std::string& out, const Object& value);
bool rdbReadObject(const std::string& in, size_t& offset, RdbType type, Object& value);

// Serialises every database into a buffer, for the replication full-resync
// payload. Same layout as the file, minus the file's magic header.
std::string rdbSerializeDatasets(Server& server);
bool rdbLoadDatasets(Server& server, const std::string& payload, std::string& error);

} // namespace miniredis
