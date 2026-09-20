#include "miniredis/resp.hpp"

#include <cstring>

namespace miniredis {

namespace {

constexpr char kCrlf[] = "\r\n";

// Finds the CRLF terminating the token that starts at `from`. Returns npos if
// the line has not fully arrived yet.
size_t findCrlf(const std::string& buffer, size_t from) {
    const size_t pos = buffer.find(kCrlf, from, 2);
    return pos;
}

// Parses the integer that follows a type tag, e.g. the "3" in "$3\r\n".
bool parseLengthToken(const std::string& buffer, size_t start, size_t end, std::int64_t& out) {
    return parseInt64(std::string_view(buffer).substr(start, end - start), out);
}

} // namespace

// ---------------------------------------------------------------------------
// RespWriter
// ---------------------------------------------------------------------------

void RespWriter::simpleString(std::string_view s) {
    m_out.push_back('+');
    m_out.append(s);
    m_out.append(kCrlf, 2);
}

void RespWriter::error(std::string_view s) {
    m_out.push_back('-');
    m_out.append(s);
    m_out.append(kCrlf, 2);
}

void RespWriter::integer(std::int64_t v) {
    m_out.push_back(':');
    m_out.append(formatInt64(v));
    m_out.append(kCrlf, 2);
}

void RespWriter::bulkString(std::string_view s) {
    m_out.push_back('$');
    m_out.append(formatInt64(static_cast<std::int64_t>(s.size())));
    m_out.append(kCrlf, 2);
    m_out.append(s);
    m_out.append(kCrlf, 2);
}

void RespWriter::nullBulkString() {
    m_out.append("$-1\r\n", 5);
}

void RespWriter::nullArray() {
    m_out.append("*-1\r\n", 5);
}

void RespWriter::arrayHeader(std::int64_t count) {
    m_out.push_back('*');
    m_out.append(formatInt64(count));
    m_out.append(kCrlf, 2);
}

void RespWriter::stringArray(const std::vector<std::string>& items) {
    arrayHeader(static_cast<std::int64_t>(items.size()));
    for (const auto& item : items) {
        bulkString(item);
    }
}

// ---------------------------------------------------------------------------
// RespParser
// ---------------------------------------------------------------------------

ParseStatus RespParser::parseInline(const std::string& buffer, size_t& offset, Args& out, std::string& error) {
    const size_t lineEnd = buffer.find('\n', offset);
    if (lineEnd == std::string::npos) {
        if (buffer.size() - offset > kMaxInlineLength) {
            error = "Protocol error: too big inline request";
            return ParseStatus::Invalid;
        }
        return ParseStatus::Incomplete;
    }

    size_t textEnd = lineEnd;
    if (textEnd > offset && buffer[textEnd - 1] == '\r') --textEnd;

    // Split on whitespace, honouring single and double quotes so that
    // `SET greeting "hello world"` behaves as a user would expect.
    out.clear();
    std::string current;
    bool haveToken = false;
    char quote = 0;

    for (size_t i = offset; i < textEnd; ++i) {
        const char c = buffer[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < textEnd) {
                current.push_back(buffer[++i]);
            } else if (c == quote) {
                quote = 0;
            } else {
                current.push_back(c);
            }
            haveToken = true;
        } else if (c == '"' || c == '\'') {
            quote = c;
            haveToken = true;
        } else if (c == ' ' || c == '\t') {
            if (haveToken) {
                out.push_back(current);
                current.clear();
                haveToken = false;
            }
        } else {
            current.push_back(c);
            haveToken = true;
        }
    }

    if (quote != 0) {
        error = "Protocol error: unbalanced quotes in request";
        return ParseStatus::Invalid;
    }
    if (haveToken) out.push_back(current);

    offset = lineEnd + 1;
    return ParseStatus::Complete;
}

ParseStatus RespParser::parseCommand(const std::string& buffer, size_t& offset, Args& out, std::string& error) {
    if (offset >= buffer.size()) return ParseStatus::Incomplete;

    // Anything that does not start with '*' is an inline command.
    if (buffer[offset] != '*') {
        return parseInline(buffer, offset, out, error);
    }

    size_t cursor = offset;
    const size_t headerEnd = findCrlf(buffer, cursor + 1);
    if (headerEnd == std::string::npos) return ParseStatus::Incomplete;

    std::int64_t elementCount = 0;
    if (!parseLengthToken(buffer, cursor + 1, headerEnd, elementCount)) {
        error = "Protocol error: invalid multibulk length";
        return ParseStatus::Invalid;
    }
    if (elementCount > kMaxMultiBulk) {
        error = "Protocol error: invalid multibulk length";
        return ParseStatus::Invalid;
    }

    cursor = headerEnd + 2;

    // A null or empty array is a well-formed no-op; the caller skips it.
    if (elementCount <= 0) {
        out.clear();
        offset = cursor;
        return ParseStatus::Complete;
    }

    Args parsed;
    parsed.reserve(static_cast<size_t>(elementCount));

    for (std::int64_t i = 0; i < elementCount; ++i) {
        if (cursor >= buffer.size()) return ParseStatus::Incomplete;
        if (buffer[cursor] != '$') {
            error = "Protocol error: expected '$', got '" + std::string(1, buffer[cursor]) + "'";
            return ParseStatus::Invalid;
        }

        const size_t lengthEnd = findCrlf(buffer, cursor + 1);
        if (lengthEnd == std::string::npos) return ParseStatus::Incomplete;

        std::int64_t bulkLength = 0;
        if (!parseLengthToken(buffer, cursor + 1, lengthEnd, bulkLength)) {
            error = "Protocol error: invalid bulk length";
            return ParseStatus::Invalid;
        }
        if (bulkLength < -1 || bulkLength > kMaxBulkLength) {
            error = "Protocol error: invalid bulk length";
            return ParseStatus::Invalid;
        }

        const size_t payloadStart = lengthEnd + 2;
        if (bulkLength < 0) {
            // A null element inside a command array is not meaningful, but it
            // is well formed, so treat it as an empty argument.
            parsed.emplace_back();
            cursor = payloadStart;
            continue;
        }

        const size_t needed = payloadStart + static_cast<size_t>(bulkLength) + 2;
        if (buffer.size() < needed) return ParseStatus::Incomplete;

        parsed.emplace_back(buffer, payloadStart, static_cast<size_t>(bulkLength));
        cursor = needed;
    }

    out = std::move(parsed);
    offset = cursor;
    return ParseStatus::Complete;
}

ParseStatus RespParser::parseReply(const std::string& buffer, size_t& offset, Args& out, std::string& error) {
    if (offset >= buffer.size()) return ParseStatus::Incomplete;

    const char tag = buffer[offset];
    switch (tag) {
        case '+':
        case '-':
        case ':': {
            const size_t end = findCrlf(buffer, offset + 1);
            if (end == std::string::npos) return ParseStatus::Incomplete;
            out.clear();
            out.emplace_back(buffer, offset + 1, end - offset - 1);
            offset = end + 2;
            return ParseStatus::Complete;
        }

        case '$': {
            const size_t lengthEnd = findCrlf(buffer, offset + 1);
            if (lengthEnd == std::string::npos) return ParseStatus::Incomplete;

            std::int64_t bulkLength = 0;
            if (!parseLengthToken(buffer, offset + 1, lengthEnd, bulkLength) ||
                bulkLength < -1 || bulkLength > kMaxBulkLength) {
                error = "Protocol error: invalid bulk length in reply";
                return ParseStatus::Invalid;
            }

            out.clear();
            if (bulkLength < 0) {
                offset = lengthEnd + 2;
                return ParseStatus::Complete;
            }

            const size_t payloadStart = lengthEnd + 2;
            const size_t needed = payloadStart + static_cast<size_t>(bulkLength) + 2;
            if (buffer.size() < needed) return ParseStatus::Incomplete;

            out.emplace_back(buffer, payloadStart, static_cast<size_t>(bulkLength));
            offset = needed;
            return ParseStatus::Complete;
        }

        case '*': {
            const size_t headerEnd = findCrlf(buffer, offset + 1);
            if (headerEnd == std::string::npos) return ParseStatus::Incomplete;

            std::int64_t elementCount = 0;
            if (!parseLengthToken(buffer, offset + 1, headerEnd, elementCount) ||
                elementCount > kMaxMultiBulk) {
                error = "Protocol error: invalid multibulk length in reply";
                return ParseStatus::Invalid;
            }

            size_t cursor = headerEnd + 2;
            Args flattened;
            for (std::int64_t i = 0; i < elementCount; ++i) {
                Args element;
                const ParseStatus status = parseReply(buffer, cursor, element, error);
                if (status != ParseStatus::Complete) return status;
                for (auto& value : element) flattened.push_back(std::move(value));
            }

            out = std::move(flattened);
            offset = cursor;
            return ParseStatus::Complete;
        }

        default:
            error = "Protocol error: unknown reply type '" + std::string(1, tag) + "'";
            return ParseStatus::Invalid;
    }
}

std::string encodeCommand(const Args& args) {
    std::string out;
    // Reserve a close-enough size up front; this function runs once per write
    // command on the AOF and replication paths.
    size_t estimate = 16;
    for (const auto& arg : args) estimate += arg.size() + 16;
    out.reserve(estimate);

    RespWriter writer(out);
    writer.arrayHeader(static_cast<std::int64_t>(args.size()));
    for (const auto& arg : args) writer.bulkString(arg);
    return out;
}

} // namespace miniredis
