// Tests for the RESP codec and the shared string helpers.
//
// The parser tests lean hard on the incomplete-input cases, because that is
// where a wire protocol actually breaks: not on a well-formed command, but on
// one that arrived in three TCP segments.
#include "test_framework.hpp"

#include "miniredis/common.hpp"
#include "miniredis/resp.hpp"

using namespace miniredis;

namespace {

ParseStatus parseWhole(const std::string& input, Args& out, std::string& error) {
    size_t offset = 0;
    return RespParser::parseCommand(input, offset, out, error);
}

} // namespace

// ---------------------------------------------------------------------------
// Integer parsing
// ---------------------------------------------------------------------------

TEST(Parsing, AcceptsWellFormedIntegers) {
    std::int64_t value = 0;
    CHECK(parseInt64("0", value));      CHECK_EQ(value, 0);
    CHECK(parseInt64("42", value));     CHECK_EQ(value, 42);
    CHECK(parseInt64("-42", value));    CHECK_EQ(value, -42);
    CHECK(parseInt64("9223372036854775807", value)); CHECK_EQ(value, INT64_MAX);
    CHECK(parseInt64("-9223372036854775808", value)); CHECK_EQ(value, INT64_MIN);
}

TEST(Parsing, RejectsMalformedIntegers) {
    std::int64_t value = 0;
    CHECK(!parseInt64("", value));
    CHECK(!parseInt64("12abc", value));   // trailing junk
    CHECK(!parseInt64("abc", value));
    CHECK(!parseInt64(" 12", value));     // leading space
    CHECK(!parseInt64("12 ", value));
    CHECK(!parseInt64("007", value));     // leading zeros, as Redis rejects
    CHECK(!parseInt64("-", value));
    CHECK(!parseInt64("9223372036854775808", value));  // overflow by one
    CHECK(!parseInt64("-9223372036854775809", value)); // underflow by one
}

TEST(Parsing, FormatsDoublesWithoutSpuriousDecimals) {
    // A whole-number score has to read back as "3", not "3.0", or a client
    // comparing ZSCORE output against its own formatting will disagree.
    CHECK_EQ(formatDouble(3.0), std::string("3"));
    CHECK_EQ(formatDouble(-7.0), std::string("-7"));
    CHECK_EQ(formatDouble(1.0 / 0.0), std::string("inf"));
    CHECK_EQ(formatDouble(-1.0 / 0.0), std::string("-inf"));

    double roundTripped = 0;
    CHECK(parseDouble(formatDouble(3.14159265358979), roundTripped));
    CHECK_NEAR(roundTripped, 3.14159265358979, 1e-12);
}

TEST(Parsing, ReadsMemorySizeSuffixes) {
    std::int64_t bytes = 0;
    CHECK(parseMemorySize("1024", bytes));  CHECK_EQ(bytes, 1024);
    CHECK(parseMemorySize("1kb", bytes));   CHECK_EQ(bytes, 1024);
    CHECK(parseMemorySize("1k", bytes));    CHECK_EQ(bytes, 1000);
    CHECK(parseMemorySize("512mb", bytes)); CHECK_EQ(bytes, 512LL * 1024 * 1024);
    CHECK(parseMemorySize("2GB", bytes));   CHECK_EQ(bytes, 2LL * 1024 * 1024 * 1024);
    CHECK(!parseMemorySize("mb", bytes));
    CHECK(!parseMemorySize("12tb", bytes)); // unsupported unit
}

// ---------------------------------------------------------------------------
// Glob matching
// ---------------------------------------------------------------------------

TEST(Glob, MatchesRedisPatternSyntax) {
    CHECK(globMatch("*", "anything"));
    CHECK(globMatch("*", ""));
    CHECK(globMatch("user:*", "user:1"));
    CHECK(globMatch("*:1", "user:1"));
    CHECK(globMatch("user:?", "user:1"));
    CHECK(!globMatch("user:?", "user:12"));
    CHECK(globMatch("h[ae]llo", "hello"));
    CHECK(globMatch("h[ae]llo", "hallo"));
    CHECK(!globMatch("h[ae]llo", "hillo"));
    CHECK(globMatch("h[^e]llo", "hallo"));
    CHECK(!globMatch("h[^e]llo", "hello"));
    CHECK(globMatch("h[a-c]llo", "hbllo"));
    CHECK(!globMatch("h[a-c]llo", "hdllo"));
    CHECK(globMatch("exact", "exact"));
    CHECK(!globMatch("exact", "exacts"));
}

TEST(Glob, HandlesAdjacentWildcardsWithoutBlowingUp) {
    // "a**b" collapsing to "a*b" is what keeps a pathological pattern from
    // costing exponential time.
    CHECK(globMatch("a**b", "axxxb"));
    CHECK(globMatch("*a*a*a*a*b", "aaaaab"));
    CHECK(!globMatch("*a*a*a*a*b", "aaaaac"));
}

// ---------------------------------------------------------------------------
// RESP request parsing
// ---------------------------------------------------------------------------

TEST(Resp, ParsesAWellFormedCommand) {
    Args args;
    std::string error;
    CHECK(parseWhole("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", args, error) ==
          ParseStatus::Complete);
    CHECK_EQ(args.size(), size_t{3});
    CHECK_EQ(args[0], std::string("SET"));
    CHECK_EQ(args[1], std::string("foo"));
    CHECK_EQ(args[2], std::string("bar"));
}

TEST(Resp, ReportsIncompleteWithoutConsumingInput) {
    // This is the property the whole design rests on: a partial command must
    // leave the offset untouched so the caller can retry after the next read.
    const std::string whole = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";

    for (size_t prefix = 1; prefix < whole.size(); ++prefix) {
        Args args;
        std::string error;
        size_t offset = 0;
        const ParseStatus status =
            RespParser::parseCommand(whole.substr(0, prefix), offset, args, error);
        CHECK(status == ParseStatus::Incomplete);
        CHECK_EQ(offset, size_t{0});
    }
}

TEST(Resp, ParsesBinarySafePayloads) {
    // A bulk string is length prefixed precisely so it can contain CRLF and
    // NUL bytes. If the parser ever searched for a terminator instead, this
    // would be the test that caught it.
    std::string payload;
    payload.push_back('a');
    payload.push_back('\0');
    payload += "\r\n";
    payload.push_back('z');

    std::string request = "*2\r\n$3\r\nGET\r\n$" + std::to_string(payload.size()) + "\r\n" +
                          payload + "\r\n";

    Args args;
    std::string error;
    CHECK(parseWhole(request, args, error) == ParseStatus::Complete);
    CHECK_EQ(args.size(), size_t{2});
    CHECK_EQ(args[1], payload);
}

TEST(Resp, ParsesPipelinedCommandsBackToBack) {
    const std::string pipelined =
        "*1\r\n$4\r\nPING\r\n"
        "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
        "*1\r\n$4\r\nPING\r\n";

    size_t offset = 0;
    std::string error;
    int parsed = 0;

    while (offset < pipelined.size()) {
        Args args;
        const ParseStatus status = RespParser::parseCommand(pipelined, offset, args, error);
        CHECK(status == ParseStatus::Complete);
        ++parsed;
    }
    CHECK_EQ(parsed, 3);
}

TEST(Resp, RejectsHostileLengthHeaders) {
    Args args;
    std::string error;

    // Without a cap, this would ask the server to reserve a billion arguments.
    CHECK(parseWhole("*999999999\r\n", args, error) == ParseStatus::Invalid);
    CHECK(parseWhole("*1\r\n$999999999999\r\n", args, error) == ParseStatus::Invalid);
    CHECK(parseWhole("*1\r\n$abc\r\n", args, error) == ParseStatus::Invalid);
    CHECK(parseWhole("*2\r\n$3\r\nGET\r\n+notbulk\r\n", args, error) == ParseStatus::Invalid);
}

TEST(Resp, ParsesInlineCommands) {
    Args args;
    std::string error;

    CHECK(parseWhole("PING\r\n", args, error) == ParseStatus::Complete);
    CHECK_EQ(args.size(), size_t{1});
    CHECK_EQ(args[0], std::string("PING"));

    // Quotes let a value contain spaces, which is the only reason the inline
    // form is usable for anything beyond PING.
    CHECK(parseWhole("SET greeting \"hello world\"\r\n", args, error) == ParseStatus::Complete);
    CHECK_EQ(args.size(), size_t{3});
    CHECK_EQ(args[2], std::string("hello world"));

    CHECK(parseWhole("SET k 'a b'\n", args, error) == ParseStatus::Complete);
    CHECK_EQ(args[2], std::string("a b"));

    CHECK(parseWhole("SET k \"unbalanced\r\n", args, error) == ParseStatus::Invalid);
}

TEST(Resp, HandlesEmptyAndNullArrays) {
    Args args;
    std::string error;

    CHECK(parseWhole("*0\r\n", args, error) == ParseStatus::Complete);
    CHECK(args.empty());

    CHECK(parseWhole("*-1\r\n", args, error) == ParseStatus::Complete);
    CHECK(args.empty());
}

// ---------------------------------------------------------------------------
// RESP writing
// ---------------------------------------------------------------------------

TEST(Resp, WritesEachReplyType) {
    std::string out;
    RespWriter writer(out);

    writer.simpleString("OK");
    CHECK_EQ(out, std::string("+OK\r\n"));

    out.clear();
    writer.error("ERR nope");
    CHECK_EQ(out, std::string("-ERR nope\r\n"));

    out.clear();
    writer.integer(-17);
    CHECK_EQ(out, std::string(":-17\r\n"));

    out.clear();
    writer.bulkString("hello");
    CHECK_EQ(out, std::string("$5\r\nhello\r\n"));

    out.clear();
    writer.bulkString("");
    CHECK_EQ(out, std::string("$0\r\n\r\n"));

    out.clear();
    writer.nullBulkString();
    CHECK_EQ(out, std::string("$-1\r\n"));

    out.clear();
    writer.nullArray();
    CHECK_EQ(out, std::string("*-1\r\n"));
}

TEST(Resp, EncodedCommandsRoundTrip) {
    // Everything written to the AOF and shipped to replicas goes through
    // encodeCommand, so the round trip has to be exact, including for values
    // that contain the protocol's own delimiters.
    const Args original{"SET", "key with spaces", std::string("value\r\nwith\0nulls", 17)};

    const std::string encoded = encodeCommand(original);

    Args decoded;
    std::string error;
    CHECK(parseWhole(encoded, decoded, error) == ParseStatus::Complete);
    CHECK_EQ(decoded.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        CHECK_EQ(decoded[i], original[i]);
    }
}
