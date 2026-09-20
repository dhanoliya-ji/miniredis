// Tests for the snapshot format and the replication backlog.
//
// These are the two places where a bug is silent: a corrupt snapshot that
// still loads, or a backlog that serves the wrong bytes, both produce a
// plausible-looking dataset that is quietly wrong.
#include "test_framework.hpp"

#include "miniredis/rdb.hpp"
#include "miniredis/replication.hpp"
#include "miniredis/server.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

using namespace miniredis;

namespace {

// A server bound to an ephemeral port in a scratch directory, so the tests
// never collide with a real instance or with each other.
std::unique_ptr<Server> makeServer(const std::string& suffix, int port) {
    ServerConfig config;
    config.port = port;
    config.nodeId = "test-" + suffix;
    config.workingDir = (std::filesystem::temp_directory_path() / ("miniredis-test-" + suffix)).string();
    config.saveRules.clear();      // no scheduled snapshots during a test
    config.loadOnStartup = false;
    config.logLevel = LogLevel::Warning;

    std::error_code ec;
    std::filesystem::remove_all(config.workingDir, ec);
    std::filesystem::create_directories(config.workingDir, ec);

    return std::make_unique<Server>(std::move(config));
}

} // namespace

// ---------------------------------------------------------------------------
// Primitive encoding
// ---------------------------------------------------------------------------

TEST(Rdb, EncodesPrimitivesLittleEndian) {
    std::string buffer;
    rdbWriteUint32(buffer, 0x01020304u);

    // Written low byte first, explicitly, so a snapshot moves between machines
    // of opposite byte order.
    CHECK_EQ(static_cast<unsigned char>(buffer[0]), static_cast<unsigned char>(0x04));
    CHECK_EQ(static_cast<unsigned char>(buffer[3]), static_cast<unsigned char>(0x01));

    size_t offset = 0;
    std::uint32_t readBack = 0;
    CHECK(rdbReadUint32(buffer, offset, readBack));
    CHECK_EQ(readBack, 0x01020304u);
}

TEST(Rdb, PreservesDoublesExactly) {
    // Scores are bit-cast rather than printed, so the last ulp survives.
    const double original = 3.141592653589793;

    std::string buffer;
    rdbWriteDouble(buffer, original);

    size_t offset = 0;
    double readBack = 0;
    CHECK(rdbReadDouble(buffer, offset, readBack));
    CHECK(readBack == original);
}

TEST(Rdb, StringsAreBinarySafe) {
    std::string payload("a\0b\r\nc", 6);

    std::string buffer;
    rdbWriteString(buffer, payload);

    size_t offset = 0;
    std::string readBack;
    CHECK(rdbReadString(buffer, offset, readBack));
    CHECK_EQ(readBack, payload);
}

TEST(Rdb, RejectsALengthLongerThanTheBuffer) {
    // A corrupt length field must not become a gigantic allocation.
    std::string buffer;
    rdbWriteUint64(buffer, 1'000'000'000);
    buffer += "short";

    size_t offset = 0;
    std::string value;
    CHECK(!rdbReadString(buffer, offset, value));
}

// ---------------------------------------------------------------------------
// Object round trip
// ---------------------------------------------------------------------------

TEST(Rdb, RoundTripsEveryType) {
    struct Case {
        const char* name;
        Object value;
        RdbType tag;
    };

    Object list = Object::makeList();
    list.list().push_back("a");
    list.list().push_back("b");

    Object hash = Object::makeHash();
    hash.hash().emplace("field", "value");

    Object set = Object::makeSet();
    set.set().insert("member");

    Object zset = Object::makeZSet();
    zset.zset().add("alice", 100.5);
    zset.zset().add("bob", 90);

    std::vector<Case> cases;
    cases.push_back({"string", Object::makeString("hello"), RdbType::String});
    cases.push_back({"list", std::move(list), RdbType::List});
    cases.push_back({"hash", std::move(hash), RdbType::Hash});
    cases.push_back({"set", std::move(set), RdbType::Set});
    cases.push_back({"zset", std::move(zset), RdbType::ZSet});

    for (const auto& testCase : cases) {
        std::string buffer;
        rdbWriteObject(buffer, testCase.value);

        size_t offset = 0;
        Object restored;
        CHECK(rdbReadObject(buffer, offset, testCase.tag, restored));
        CHECK(restored.type() == testCase.value.type());
        CHECK_EQ(restored.length(), testCase.value.length());
    }
}

TEST(Rdb, RestoresSortedSetScores) {
    Object zset = Object::makeZSet();
    zset.zset().add("alice", 100.5);
    zset.zset().add("bob", -3.25);

    std::string buffer;
    rdbWriteObject(buffer, zset);

    size_t offset = 0;
    Object restored;
    CHECK(rdbReadObject(buffer, offset, RdbType::ZSet, restored));

    double score = 0;
    CHECK(restored.zset().score("alice", score));
    CHECK(score == 100.5);
    CHECK(restored.zset().score("bob", score));
    CHECK(score == -3.25);
}

// ---------------------------------------------------------------------------
// Whole-dataset snapshots
// ---------------------------------------------------------------------------

TEST(Snapshot, SurvivesASaveAndLoadCycle) {
    auto server = makeServer("snapshot", 17311);

    server->db(0).setValue("greeting", Object::makeString("hello"));
    server->db(0).setValue("counter", Object::makeString("42"));
    Object* list = server->db(0).getOrCreate("queue", ObjectType::List, nowMillis());
    list->list().push_back("first");
    list->list().push_back("second");
    server->db(1).setValue("other-db", Object::makeString("v"));

    const std::string path = server->config().resolvePath("test.rdb");

    RdbFile writer(*server);
    std::string error;
    CHECK(writer.save(path, error));

    server->flushAllDatabases();
    CHECK_EQ(server->db(0).size(), size_t{0});

    RdbFile reader(*server);
    CHECK(reader.load(path, error));

    Object* greeting = server->db(0).lookupRead("greeting", nowMillis());
    CHECK(greeting != nullptr);
    CHECK_EQ(greeting->string(), std::string("hello"));

    Object* restoredList = server->db(0).lookupRead("queue", nowMillis());
    CHECK(restoredList != nullptr);
    CHECK_EQ(restoredList->list().size(), size_t{2});
    CHECK_EQ(restoredList->list()[0], std::string("first"));

    // Database separation has to survive too, or SELECT would be meaningless
    // after a restart.
    CHECK(server->db(1).lookupRead("other-db", nowMillis()) != nullptr);
    CHECK(server->db(0).lookupRead("other-db", nowMillis()) == nullptr);
}

TEST(Snapshot, CarriesTtlsAsAbsoluteDeadlines) {
    auto server = makeServer("snapshot-ttl", 17312);
    const Millis now = nowMillis();

    server->db(0).setValue("expiring", Object::makeString("v"));
    server->db(0).setExpireAt("expiring", now + 60000);
    server->db(0).setValue("permanent", Object::makeString("v"));

    const std::string path = server->config().resolvePath("ttl.rdb");
    std::string error;
    RdbFile writer(*server);
    CHECK(writer.save(path, error));

    server->flushAllDatabases();
    RdbFile reader(*server);
    CHECK(reader.load(path, error));

    // Roughly a minute left, not a fresh minute: the deadline is absolute, so
    // a restart does not silently extend every key's life.
    const std::int64_t remaining = server->db(0).ttlMillis("expiring", nowMillis());
    CHECK(remaining > 50000);
    CHECK(remaining <= 60000);
    CHECK_EQ(server->db(0).ttlMillis("permanent", nowMillis()), std::int64_t{-1});
}

TEST(Snapshot, DropsKeysThatAlreadyExpired) {
    auto server = makeServer("snapshot-dead", 17313);

    server->db(0).setValue("doomed", Object::makeString("v"));
    server->db(0).setExpireAt("doomed", nowMillis() - 1000); // already past
    server->db(0).setValue("alive", Object::makeString("v"));

    const std::string path = server->config().resolvePath("dead.rdb");
    std::string error;
    RdbFile writer(*server);
    CHECK(writer.save(path, error));

    server->flushAllDatabases();
    RdbFile reader(*server);
    CHECK(reader.load(path, error));

    CHECK_EQ(server->db(0).size(), size_t{1});
    CHECK(server->db(0).lookupRead("alive", nowMillis()) != nullptr);
}

TEST(Snapshot, RejectsACorruptedFile) {
    auto server = makeServer("snapshot-corrupt", 17314);
    server->db(0).setValue("k", Object::makeString("v"));

    const std::string path = server->config().resolvePath("corrupt.rdb");
    std::string error;
    RdbFile writer(*server);
    CHECK(writer.save(path, error));

    // Flip a byte in the middle. Without the checksum this would load as a
    // plausible but wrong dataset, which is worse than failing.
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        CHECK(file.is_open());
        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        file.seekp(size / 2);
        file.put('X');
    }

    server->flushAllDatabases();
    RdbFile reader(*server);
    CHECK(!reader.load(path, error));
    CHECK(error.find("checksum") != std::string::npos);
}

TEST(Snapshot, RejectsAForeignFile) {
    auto server = makeServer("snapshot-foreign", 17315);
    const std::string path = server->config().resolvePath("foreign.rdb");

    {
        std::ofstream file(path, std::ios::binary);
        file << "NOT A MINIREDIS SNAPSHOT AT ALL, NOT EVEN CLOSE";
    }

    std::string error;
    RdbFile reader(*server);
    CHECK(!reader.load(path, error));
}

TEST(Snapshot, Crc64IsStableAndSensitive) {
    const std::string payload = "the quick brown fox";
    const std::uint64_t checksum = RdbFile::crc64(0, payload.data(), payload.size());

    // Deterministic across calls...
    CHECK_EQ(RdbFile::crc64(0, payload.data(), payload.size()), checksum);
    // ...and changes when any byte does.
    const std::string altered = "the quick brown fix";
    CHECK_NE(RdbFile::crc64(0, altered.data(), altered.size()), checksum);
}

// ---------------------------------------------------------------------------
// Replication backlog
// ---------------------------------------------------------------------------

TEST(Backlog, ServesRecentBytesForAPartialResync) {
    ReplicationBacklog backlog;
    backlog.resize(1024);

    const std::string first = "hello ";
    const std::string second = "world";
    backlog.append(first.data(), first.size());
    const std::int64_t offsetAfterFirst = backlog.endOffset();
    backlog.append(second.data(), second.size());

    CHECK(backlog.canServe(offsetAfterFirst));

    std::string missed;
    CHECK(backlog.readFrom(offsetAfterFirst, missed));
    // Exactly the bytes written after that point, which is what makes a
    // partial resync correct rather than approximately correct.
    CHECK_EQ(missed, second);

    std::string everything;
    CHECK(backlog.readFrom(0, everything));
    CHECK_EQ(everything, first + second);
}

TEST(Backlog, RefusesAnOffsetItHasAlreadyOverwritten) {
    ReplicationBacklog backlog;
    backlog.resize(64);

    // Push far more than the ring can hold, so the early offsets fall out.
    for (int i = 0; i < 20; ++i) {
        const std::string chunk = "0123456789";
        backlog.append(chunk.data(), chunk.size());
    }

    // Offset 0 is long gone; the replica must be told to do a full resync
    // rather than handed the wrong bytes.
    CHECK(!backlog.canServe(0));
    CHECK(backlog.canServe(backlog.endOffset()));
    CHECK(backlog.firstAvailableOffset() > 0);
}

TEST(Backlog, HandlesAWriteLargerThanItself) {
    ReplicationBacklog backlog;
    backlog.resize(16);

    const std::string large(100, 'x');
    backlog.append(large.data(), large.size());

    // It can only keep the tail, and must report that honestly.
    CHECK_EQ(backlog.endOffset(), std::int64_t{100});
    CHECK_EQ(backlog.histlen(), std::int64_t{16});
    CHECK(!backlog.canServe(0));

    std::string tail;
    CHECK(backlog.readFrom(backlog.firstAvailableOffset(), tail));
    CHECK_EQ(tail.size(), size_t{16});
}

TEST(Backlog, WrapsWithoutCorruptingContent) {
    ReplicationBacklog backlog;
    backlog.resize(10);

    const std::string first = "aaaaa";
    const std::string second = "bbbbb";
    const std::string third = "ccccc";
    backlog.append(first.data(), first.size());
    backlog.append(second.data(), second.size());
    const std::int64_t beforeThird = backlog.endOffset();
    backlog.append(third.data(), third.size()); // wraps, evicting "aaaaa"

    std::string recovered;
    CHECK(backlog.readFrom(beforeThird, recovered));
    CHECK_EQ(recovered, third);

    CHECK(backlog.readFrom(backlog.firstAvailableOffset(), recovered));
    CHECK_EQ(recovered, second + third);
}
