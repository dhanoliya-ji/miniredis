#include "miniredis/rdb.hpp"

#include "miniredis/server.hpp"

#include <cstring>
#include <filesystem>
#include <vector>

namespace miniredis {

// ---------------------------------------------------------------------------
// Primitive encoding
//
// Everything is little-endian and explicitly byte-assembled rather than
// memcpy'd from the host representation, so a snapshot written on one machine
// loads on a machine with the opposite byte order.
// ---------------------------------------------------------------------------

void rdbWriteByte(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

void rdbWriteUint32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

void rdbWriteUint64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

void rdbWriteInt64(std::string& out, std::int64_t value) {
    rdbWriteUint64(out, static_cast<std::uint64_t>(value));
}

void rdbWriteDouble(std::string& out, double value) {
    // Bit-cast through a uint64 so the exact IEEE-754 payload is preserved.
    // Writing the decimal rendering instead would lose the last ulp.
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    rdbWriteUint64(out, bits);
}

void rdbWriteString(std::string& out, std::string_view value) {
    rdbWriteUint64(out, static_cast<std::uint64_t>(value.size()));
    out.append(value);
}

bool rdbReadByte(const std::string& in, size_t& offset, std::uint8_t& value) {
    if (offset + 1 > in.size()) return false;
    value = static_cast<std::uint8_t>(in[offset++]);
    return true;
}

bool rdbReadUint32(const std::string& in, size_t& offset, std::uint32_t& value) {
    if (offset + 4 > in.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[offset + static_cast<size_t>(i)])) << (i * 8);
    }
    offset += 4;
    return true;
}

bool rdbReadUint64(const std::string& in, size_t& offset, std::uint64_t& value) {
    if (offset + 8 > in.size()) return false;
    value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[offset + static_cast<size_t>(i)])) << (i * 8);
    }
    offset += 8;
    return true;
}

bool rdbReadInt64(const std::string& in, size_t& offset, std::int64_t& value) {
    std::uint64_t raw = 0;
    if (!rdbReadUint64(in, offset, raw)) return false;
    value = static_cast<std::int64_t>(raw);
    return true;
}

bool rdbReadDouble(const std::string& in, size_t& offset, double& value) {
    std::uint64_t bits = 0;
    if (!rdbReadUint64(in, offset, bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

bool rdbReadString(const std::string& in, size_t& offset, std::string& value) {
    std::uint64_t length = 0;
    if (!rdbReadUint64(in, offset, length)) return false;

    // A corrupt length field must not become a gigantic allocation, so it is
    // validated against what is actually left in the buffer before reserving.
    if (length > in.size() - offset) return false;

    value.assign(in, offset, static_cast<size_t>(length));
    offset += static_cast<size_t>(length);
    return true;
}

// ---------------------------------------------------------------------------
// Object encoding
// ---------------------------------------------------------------------------

namespace {

RdbType rdbTypeOf(const Object& value) {
    switch (value.type()) {
        case ObjectType::String: return RdbType::String;
        case ObjectType::List:   return RdbType::List;
        case ObjectType::Hash:   return RdbType::Hash;
        case ObjectType::Set:    return RdbType::Set;
        case ObjectType::ZSet:   return RdbType::ZSet;
    }
    return RdbType::String;
}

} // namespace

void rdbWriteObject(std::string& out, const Object& value) {
    switch (value.type()) {
        case ObjectType::String:
            rdbWriteString(out, value.string());
            break;

        case ObjectType::List:
            rdbWriteUint64(out, value.list().size());
            for (const auto& item : value.list()) rdbWriteString(out, item);
            break;

        case ObjectType::Hash:
            rdbWriteUint64(out, value.hash().size());
            for (const auto& [field, fieldValue] : value.hash()) {
                rdbWriteString(out, field);
                rdbWriteString(out, fieldValue);
            }
            break;

        case ObjectType::Set:
            rdbWriteUint64(out, value.set().size());
            for (const auto& member : value.set()) rdbWriteString(out, member);
            break;

        case ObjectType::ZSet:
            rdbWriteUint64(out, value.zset().size());
            // Written in score order so that a reload rebuilds the tree by
            // ascending insertion, and so a snapshot diff is stable.
            for (const auto& [score, member] : value.zset().ordered()) {
                rdbWriteString(out, member);
                rdbWriteDouble(out, score);
            }
            break;
    }
}

bool rdbReadObject(const std::string& in, size_t& offset, RdbType type, Object& value) {
    switch (type) {
        case RdbType::String: {
            std::string payload;
            if (!rdbReadString(in, offset, payload)) return false;
            value = Object::makeString(std::move(payload));
            return true;
        }

        case RdbType::List: {
            std::uint64_t count = 0;
            if (!rdbReadUint64(in, offset, count)) return false;
            value = Object::makeList();
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string item;
                if (!rdbReadString(in, offset, item)) return false;
                value.list().push_back(std::move(item));
            }
            return true;
        }

        case RdbType::Hash: {
            std::uint64_t count = 0;
            if (!rdbReadUint64(in, offset, count)) return false;
            value = Object::makeHash();
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string field;
                std::string fieldValue;
                if (!rdbReadString(in, offset, field)) return false;
                if (!rdbReadString(in, offset, fieldValue)) return false;
                value.hash().emplace(std::move(field), std::move(fieldValue));
            }
            return true;
        }

        case RdbType::Set: {
            std::uint64_t count = 0;
            if (!rdbReadUint64(in, offset, count)) return false;
            value = Object::makeSet();
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string member;
                if (!rdbReadString(in, offset, member)) return false;
                value.set().insert(std::move(member));
            }
            return true;
        }

        case RdbType::ZSet: {
            std::uint64_t count = 0;
            if (!rdbReadUint64(in, offset, count)) return false;
            value = Object::makeZSet();
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string member;
                double score = 0;
                if (!rdbReadString(in, offset, member)) return false;
                if (!rdbReadDouble(in, offset, score)) return false;
                value.zset().add(member, score);
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Dataset encoding
// ---------------------------------------------------------------------------

std::string rdbSerializeDatasets(Server& server) {
    std::string out;
    const Millis now = nowMillis();

    for (int index = 0; index < server.databaseCount(); ++index) {
        Keyspace& keyspace = server.db(index);
        if (keyspace.empty()) continue;

        rdbWriteByte(out, static_cast<std::uint8_t>(RdbOpcode::SelectDb));
        rdbWriteUint32(out, static_cast<std::uint32_t>(index));

        keyspace.forEach([&](const Bytes& key, const KeyEntry& entry) {
            Millis expireAt = 0;
            const bool hasExpiry = keyspace.getExpireAt(key, expireAt);
            if (hasExpiry && expireAt <= now) return; // already dead

            if (hasExpiry) {
                rdbWriteByte(out, static_cast<std::uint8_t>(RdbOpcode::ExpireMs));
                rdbWriteInt64(out, expireAt);
            }

            rdbWriteByte(out, static_cast<std::uint8_t>(rdbTypeOf(entry.value)));
            rdbWriteString(out, key);
            rdbWriteObject(out, entry.value);
        });
    }

    rdbWriteByte(out, static_cast<std::uint8_t>(RdbOpcode::Eof));
    return out;
}

bool rdbLoadDatasets(Server& server, const std::string& payload, std::string& error) {
    size_t offset = 0;
    int currentDb = 0;
    Millis pendingExpiry = 0;
    bool havePendingExpiry = false;
    const Millis now = nowMillis();

    while (offset < payload.size()) {
        std::uint8_t tag = 0;
        if (!rdbReadByte(payload, offset, tag)) {
            error = "snapshot ended unexpectedly while reading a type tag";
            return false;
        }

        if (tag == static_cast<std::uint8_t>(RdbOpcode::Eof)) return true;

        if (tag == static_cast<std::uint8_t>(RdbOpcode::SelectDb)) {
            std::uint32_t index = 0;
            if (!rdbReadUint32(payload, offset, index)) {
                error = "snapshot ended unexpectedly while reading a database index";
                return false;
            }
            if (static_cast<int>(index) >= server.databaseCount()) {
                error = "snapshot references database " + formatInt64(index) +
                        " but this server has only " + formatInt64(server.databaseCount());
                return false;
            }
            currentDb = static_cast<int>(index);
            continue;
        }

        if (tag == static_cast<std::uint8_t>(RdbOpcode::ExpireMs)) {
            if (!rdbReadInt64(payload, offset, pendingExpiry)) {
                error = "snapshot ended unexpectedly while reading an expiry";
                return false;
            }
            havePendingExpiry = true;
            continue;
        }

        if (tag > static_cast<std::uint8_t>(RdbType::ZSet)) {
            error = "snapshot contains unknown type tag " + formatInt64(tag);
            return false;
        }

        std::string key;
        if (!rdbReadString(payload, offset, key)) {
            error = "snapshot ended unexpectedly while reading a key";
            return false;
        }

        Object value;
        if (!rdbReadObject(payload, offset, static_cast<RdbType>(tag), value)) {
            error = "snapshot ended unexpectedly while reading the value of key '" + key + "'";
            return false;
        }

        // A key whose TTL has already passed is dropped here rather than
        // loaded and expired a moment later, which keeps a long-stopped server
        // from briefly serving stale data on startup.
        if (havePendingExpiry && pendingExpiry <= now) {
            havePendingExpiry = false;
            continue;
        }

        Keyspace& keyspace = server.db(currentDb);
        keyspace.setValue(key, std::move(value));
        if (havePendingExpiry) {
            keyspace.setExpireAt(key, pendingExpiry);
            havePendingExpiry = false;
        }
    }

    error = "snapshot is missing its end-of-file marker";
    return false;
}

// ---------------------------------------------------------------------------
// CRC64
// ---------------------------------------------------------------------------

namespace {

// Jones polynomial, reflected. The table is built once on first use rather
// than being a 2 KB literal in the source.
struct Crc64Table {
    std::uint64_t entries[256];

    Crc64Table() {
        constexpr std::uint64_t kPoly = UINT64_C(0xad93d23594c935a9);
        for (int i = 0; i < 256; ++i) {
            std::uint64_t crc = static_cast<std::uint64_t>(i);
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1) ? ((crc >> 1) ^ kPoly) : (crc >> 1);
            }
            entries[i] = crc;
        }
    }
};

const Crc64Table& crc64Table() {
    static const Crc64Table table;
    return table;
}

} // namespace

std::uint64_t RdbFile::crc64(std::uint64_t seed, const char* data, size_t length) {
    const Crc64Table& table = crc64Table();
    std::uint64_t crc = seed;
    for (size_t i = 0; i < length; ++i) {
        const std::uint8_t index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint8_t>(data[i]));
        crc = table.entries[index] ^ (crc >> 8);
    }
    return crc;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

bool RdbFile::save(const std::string& path, std::string& error) {
    const std::string tempPath = path + ".tmp";

    std::string body = rdbSerializeDatasets(m_server);

    std::string header;
    header.append(kMagic, std::strlen(kMagic));
    rdbWriteUint32(header, static_cast<std::uint32_t>(kVersion));
    rdbWriteInt64(header, nowMillis());

    std::string file = header + body;

    // The checksum covers the header and the body, so a truncated file or a
    // flipped bit is caught on load instead of producing a plausible-looking
    // but wrong dataset.
    const std::uint64_t checksum = crc64(0, file.data(), file.size());
    rdbWriteUint64(file, checksum);

    std::FILE* out = std::fopen(tempPath.c_str(), "wb");
    if (out == nullptr) {
        error = "cannot create the snapshot temporary file: " + tempPath;
        return false;
    }

    const size_t written = std::fwrite(file.data(), 1, file.size(), out);
    std::fflush(out);
    std::fclose(out);

    if (written != file.size()) {
        std::remove(tempPath.c_str());
        error = "short write while saving the snapshot";
        return false;
    }

    // Rename last. Until this succeeds the previous snapshot is still intact,
    // so a crash during a save never destroys the last good one.
    std::error_code ec;
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tempPath, path, ec);
    }
    if (ec) {
        error = "cannot move the snapshot into place: " + ec.message();
        return false;
    }

    m_keysWritten = 0;
    for (int index = 0; index < m_server.databaseCount(); ++index) {
        m_keysWritten += m_server.db(index).size();
    }
    return true;
}

bool RdbFile::load(const std::string& path, std::string& error) {
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) {
        error = "snapshot not found: " + path;
        return false;
    }

    std::string file;
    char chunk[64 * 1024];
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
        file.append(chunk, read);
    }
    std::fclose(in);

    const size_t magicLength = std::strlen(kMagic);
    if (file.size() < magicLength + 4 + 8 + 8) {
        error = "snapshot is too small to be valid";
        return false;
    }
    if (file.compare(0, magicLength, kMagic) != 0) {
        error = "snapshot has the wrong magic header; this is not a MiniRedis snapshot";
        return false;
    }

    const std::uint64_t storedChecksum = [&] {
        size_t offset = file.size() - 8;
        std::uint64_t value = 0;
        rdbReadUint64(file, offset, value);
        return value;
    }();

    const std::uint64_t actualChecksum = crc64(0, file.data(), file.size() - 8);
    if (storedChecksum != actualChecksum) {
        error = "snapshot checksum mismatch; the file is corrupt or truncated";
        return false;
    }

    size_t offset = magicLength;
    std::uint32_t version = 0;
    if (!rdbReadUint32(file, offset, version)) {
        error = "snapshot is missing its version field";
        return false;
    }
    if (version > kVersion) {
        error = "snapshot was written by a newer MiniRedis (format version " +
                formatInt64(version) + ", this build understands " + formatInt64(kVersion) + ")";
        return false;
    }

    std::int64_t savedAt = 0;
    if (!rdbReadInt64(file, offset, savedAt)) {
        error = "snapshot is missing its timestamp";
        return false;
    }

    const std::string body(file, offset, file.size() - offset - 8);
    if (!rdbLoadDatasets(m_server, body, error)) return false;

    m_keysLoaded = 0;
    for (int index = 0; index < m_server.databaseCount(); ++index) {
        m_keysLoaded += m_server.db(index).size();
    }
    return true;
}

} // namespace miniredis
