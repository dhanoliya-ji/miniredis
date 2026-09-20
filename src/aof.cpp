#include "miniredis/aof.hpp"

#include "miniredis/resp.hpp"
#include "miniredis/server.hpp"

#include <cstdio>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace miniredis {

namespace {

// Forces the file's data to stable storage. fflush() only moves bytes from the
// C library's buffer into the kernel; without this step a power cut still
// loses everything sitting in the page cache.
bool fsyncFile(std::FILE* file) {
    if (file == nullptr) return false;
    if (std::fflush(file) != 0) return false;

#ifdef _WIN32
    const int fd = _fileno(file);
    if (fd < 0) return false;
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE) return false;
    return FlushFileBuffers(handle) != 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

std::int64_t fileSize(std::FILE* file) {
    if (file == nullptr) return 0;
    const long position = std::ftell(file);
    if (position < 0) return 0;
    return static_cast<std::int64_t>(position);
}

} // namespace

Aof::Aof(Server& server) : m_server(server) {}

Aof::~Aof() {
    disable();
}

bool Aof::enable(std::string& error) {
    if (m_enabled && m_file != nullptr) return true;

    const std::string path = m_server.config().resolvePath(m_server.config().appendFilename);
    m_file = std::fopen(path.c_str(), "ab");
    if (m_file == nullptr) {
        error = "cannot open append-only file: " + path;
        return false;
    }

    std::fseek(m_file, 0, SEEK_END);
    m_currentSize = fileSize(m_file);
    if (m_sizeAtLastRewrite == 0) m_sizeAtLastRewrite = m_currentSize;

    m_enabled = true;
    m_lastSelectedDb = -1; // force a SELECT on the first record after reopening
    m_lastFsync = nowMillis();
    return true;
}

void Aof::disable() {
    if (m_file != nullptr) {
        // Never drop buffered commands on the way out; a clean shutdown must
        // leave a complete file behind.
        if (!m_buffer.empty()) {
            std::fwrite(m_buffer.data(), 1, m_buffer.size(), m_file);
            m_buffer.clear();
        }
        fsyncFile(m_file);
        std::fclose(m_file);
        m_file = nullptr;
    }
    m_enabled = false;
}

void Aof::feed(int dbIndex, const Args& args) {
    if (!m_enabled || args.empty()) return;

    // The log has no notion of "the client's current database", so a SELECT is
    // emitted whenever the target database changes. Emitting one per command
    // would work too but roughly doubles the file size.
    if (dbIndex != m_lastSelectedDb) {
        const Args select{"SELECT", formatInt64(dbIndex)};
        m_buffer += encodeCommand(select);
        m_lastSelectedDb = dbIndex;
    }

    m_buffer += encodeCommand(args);
}

void Aof::flush(Millis now) {
    if (!m_enabled || m_file == nullptr) return;

    if (!m_buffer.empty()) {
        const size_t written = std::fwrite(m_buffer.data(), 1, m_buffer.size(), m_file);
        if (written != m_buffer.size()) {
            // A short write means the disk is full or failing. Report it
            // rather than silently losing records; the server surfaces it
            // through INFO persistence as aof_last_write_status.
            m_lastWriteFailed = true;
            m_server.log(LogLevel::Warning, "short write to the append-only file");
        } else {
            m_lastWriteFailed = false;
            m_currentSize += static_cast<std::int64_t>(written);
        }
        m_buffer.clear();
    }

    switch (m_server.config().appendFsync) {
        case AofFsyncPolicy::Always:
            // Durable on return, at the cost of a disk round trip per event
            // loop iteration.
            fsyncFile(m_file);
            m_lastFsync = now;
            break;

        case AofFsyncPolicy::EverySec:
            // At most one second of writes is at risk. This is the default
            // because it is the knee of the durability/throughput curve.
            if (now - m_lastFsync >= 1000) {
                fsyncFile(m_file);
                m_lastFsync = now;
            }
            break;

        case AofFsyncPolicy::No:
            // Never fsync explicitly; whatever the OS has not flushed is lost
            // on a power cut. Fastest, and the right choice only when the data
            // is reconstructible from elsewhere.
            std::fflush(m_file);
            break;
    }
}

bool Aof::load(const std::string& path, bool& truncated, std::string& error) {
    truncated = false;

    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "append-only file not found: " + path;
        return false;
    }

    std::string contents;
    char chunk[64 * 1024];
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        contents.append(chunk, read);
    }
    std::fclose(file);

    size_t offset = 0;
    int currentDb = 0;
    std::uint64_t replayed = 0;

    while (offset < contents.size()) {
        Args args;
        std::string parseError;
        const size_t commandStart = offset;
        const ParseStatus status = RespParser::parseCommand(contents, offset, args, parseError);

        if (status == ParseStatus::Incomplete) {
            // The expected shape of a crash: the process died partway through
            // writing a record. Everything before it is valid, so the sane
            // recovery is to keep it and drop the partial tail.
            truncated = true;
            m_server.log(LogLevel::Warning,
                         "append-only file truncated at byte " + formatInt64(static_cast<std::int64_t>(commandStart)) +
                         "; the partial trailing command was discarded");
            break;
        }
        if (status == ParseStatus::Invalid) {
            error = "corrupt append-only file at byte " +
                    formatInt64(static_cast<std::int64_t>(commandStart)) + ": " + parseError;
            return false;
        }
        if (args.empty()) continue;

        // SELECT is handled here rather than dispatched, because the loader has
        // no client whose database index could be changed.
        if (equalsIgnoreCase(args[0], "SELECT") && args.size() == 2) {
            std::int64_t index = 0;
            if (parseInt64(args[1], index) && index >= 0 && index < m_server.databaseCount()) {
                currentDb = static_cast<int>(index);
            }
            continue;
        }

        m_server.executeFromStream(currentDb, args);
        ++replayed;
    }

    m_server.log(LogLevel::Notice,
                 "replayed " + formatInt64(static_cast<std::int64_t>(replayed)) +
                 " commands from the append-only file");
    return true;
}

void Aof::emitKeyCommands(const Bytes& key, const Object& value, Millis expireAt, std::string& out) {
    switch (value.type()) {
        case ObjectType::String:
            out += encodeCommand(Args{"SET", key, value.string()});
            break;

        case ObjectType::List: {
            // One RPUSH carrying every element, rather than one per element.
            Args args{"RPUSH", key};
            for (const auto& item : value.list()) args.push_back(item);
            if (args.size() > 2) out += encodeCommand(args);
            break;
        }

        case ObjectType::Hash: {
            Args args{"HSET", key};
            for (const auto& [field, fieldValue] : value.hash()) {
                args.push_back(field);
                args.push_back(fieldValue);
            }
            if (args.size() > 2) out += encodeCommand(args);
            break;
        }

        case ObjectType::Set: {
            Args args{"SADD", key};
            for (const auto& member : value.set()) args.push_back(member);
            if (args.size() > 2) out += encodeCommand(args);
            break;
        }

        case ObjectType::ZSet: {
            Args args{"ZADD", key};
            for (const auto& [score, member] : value.zset().ordered()) {
                args.push_back(formatDouble(score));
                args.push_back(member);
            }
            if (args.size() > 2) out += encodeCommand(args);
            break;
        }
    }

    // The TTL is written as an absolute PEXPIREAT. A relative EXPIRE would be
    // re-anchored to whenever the file happens to be replayed, quietly
    // extending every key's life by the length of the outage.
    if (expireAt > 0) {
        out += encodeCommand(Args{"PEXPIREAT", key, formatInt64(expireAt)});
    }
}

bool Aof::rewrite(std::string& error) {
    if (m_rewriteInProgress) {
        error = "an append-only file rewrite is already in progress";
        return false;
    }
    m_rewriteInProgress = true;

    const ServerConfig& config = m_server.config();
    const std::string finalPath = config.resolvePath(config.appendFilename);
    const std::string tempPath = finalPath + ".rewrite.tmp";

    std::FILE* temp = std::fopen(tempPath.c_str(), "wb");
    if (temp == nullptr) {
        m_rewriteInProgress = false;
        error = "cannot create the rewrite temporary file: " + tempPath;
        return false;
    }

    const Millis now = nowMillis();
    std::string payload;
    bool writeFailed = false;

    for (int index = 0; index < m_server.databaseCount() && !writeFailed; ++index) {
        Keyspace& keyspace = m_server.db(index);
        if (keyspace.empty()) continue;

        payload += encodeCommand(Args{"SELECT", formatInt64(index)});

        keyspace.forEach([&](const Bytes& key, const KeyEntry& entry) {
            Millis expireAt = 0;
            if (keyspace.getExpireAt(key, expireAt) && expireAt <= now) {
                // Already dead; there is no point writing it out only to have
                // it expire on the next load.
                return;
            }
            emitKeyCommands(key, entry.value, expireAt, payload);

            // Drain the buffer periodically so a huge dataset does not have to
            // be held in memory in its entirety alongside the dataset itself.
            if (payload.size() >= 4 * 1024 * 1024) {
                if (std::fwrite(payload.data(), 1, payload.size(), temp) != payload.size()) {
                    writeFailed = true;
                }
                payload.clear();
            }
        });
    }

    if (!writeFailed && !payload.empty()) {
        if (std::fwrite(payload.data(), 1, payload.size(), temp) != payload.size()) {
            writeFailed = true;
        }
    }

    const bool synced = fsyncFile(temp);
    const std::int64_t rewrittenSize = fileSize(temp);
    std::fclose(temp);

    if (writeFailed || !synced) {
        std::remove(tempPath.c_str());
        m_rewriteInProgress = false;
        error = "failed while writing the rewritten append-only file";
        return false;
    }

    // Swap the new file in only after it is complete and durable. Until the
    // rename, a crash leaves the old file untouched and still loadable.
    if (m_file != nullptr) {
        std::fclose(m_file);
        m_file = nullptr;
    }

    std::error_code ec;
    std::filesystem::rename(tempPath, finalPath, ec);
    if (ec) {
        // Windows will not rename onto an existing file, so fall back to
        // removing the target first.
        std::filesystem::remove(finalPath, ec);
        std::filesystem::rename(tempPath, finalPath, ec);
    }
    if (ec) {
        m_rewriteInProgress = false;
        error = "cannot replace the append-only file: " + ec.message();
        // Reopen whatever is there so the server keeps logging.
        std::string reopenError;
        enable(reopenError);
        return false;
    }

    m_currentSize = rewrittenSize;
    m_sizeAtLastRewrite = rewrittenSize;
    ++m_rewriteCount;
    m_rewriteInProgress = false;

    std::string reopenError;
    if (m_enabled && !enable(reopenError)) {
        error = reopenError;
        return false;
    }

    m_server.log(LogLevel::Notice,
                 "append-only file rewritten to " + formatMemorySize(rewrittenSize));
    return true;
}

bool Aof::shouldAutoRewrite() const {
    if (!m_enabled || m_rewriteInProgress) return false;

    const ServerConfig& config = m_server.config();
    if (config.autoAofRewritePercentage <= 0) return false;
    if (m_currentSize < config.autoAofRewriteMinSize) return false;

    const std::int64_t baseline = m_sizeAtLastRewrite > 0 ? m_sizeAtLastRewrite : 1;
    const std::int64_t growthPercent = ((m_currentSize - baseline) * 100) / baseline;
    return growthPercent >= config.autoAofRewritePercentage;
}

} // namespace miniredis
