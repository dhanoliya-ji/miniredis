#include "miniredis/common.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace miniredis {

std::string toUpper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool parseInt64(std::string_view s, std::int64_t& out) {
    if (s.empty() || s.size() > 20) return false;

    size_t i = 0;
    bool negative = false;
    if (s[0] == '-' || s[0] == '+') {
        negative = (s[0] == '-');
        i = 1;
        if (s.size() == 1) return false;
    }

    // Reject leading zeros ("007") the way Redis does, except for "0" itself.
    if (s.size() - i > 1 && s[i] == '0') return false;

    std::uint64_t value = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_C(0xFFFFFFFFFFFFFFFF) - digit) / 10) return false;
        value = value * 10 + digit;
    }

    if (negative) {
        if (value > static_cast<std::uint64_t>(INT64_MAX) + 1) return false;
        out = (value == static_cast<std::uint64_t>(INT64_MAX) + 1)
                  ? INT64_MIN
                  : -static_cast<std::int64_t>(value);
    } else {
        if (value > static_cast<std::uint64_t>(INT64_MAX)) return false;
        out = static_cast<std::int64_t>(value);
    }
    return true;
}

bool parseDouble(std::string_view s, double& out) {
    if (s.empty()) return false;

    // Redis accepts the three infinity spellings in ZADD and INCRBYFLOAT.
    if (equalsIgnoreCase(s, "inf") || equalsIgnoreCase(s, "+inf") || equalsIgnoreCase(s, "infinity")) {
        out = HUGE_VAL;
        return true;
    }
    if (equalsIgnoreCase(s, "-inf") || equalsIgnoreCase(s, "-infinity")) {
        out = -HUGE_VAL;
        return true;
    }

    const std::string buf(s);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(buf.c_str(), &end);
    if (errno == ERANGE || end != buf.c_str() + buf.size() || std::isnan(value)) {
        return false;
    }
    out = value;
    return true;
}

std::string formatInt64(std::int64_t v) {
    char buf[24];
    const int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return std::string(buf, static_cast<size_t>(n));
}

std::string formatDouble(double v) {
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";

    // An integral double is rendered without a decimal point, matching Redis,
    // so that ZSCORE of a whole-number score reads back as "3" not "3.0".
    if (v == static_cast<double>(static_cast<std::int64_t>(v)) && std::fabs(v) < 1e17) {
        return formatInt64(static_cast<std::int64_t>(v));
    }

    char buf[40];
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf, static_cast<size_t>(n));
}

namespace {

// Recursive descent glob matcher with Redis semantics. The recursion on '*'
// only ever advances the text pointer forwards, so the worst case is bounded
// by pattern length times text length rather than being exponential.
bool globMatchImpl(const char* p, size_t plen, const char* t, size_t tlen) {
    while (plen > 0) {
        switch (p[0]) {
            case '*':
                // Collapse runs of '*' so "a**b" costs no more than "a*b".
                while (plen > 1 && p[1] == '*') { ++p; --plen; }
                if (plen == 1) return true; // a trailing '*' matches the rest
                while (tlen > 0) {
                    if (globMatchImpl(p + 1, plen - 1, t, tlen)) return true;
                    ++t;
                    --tlen;
                }
                return globMatchImpl(p + 1, plen - 1, t, tlen);

            case '?':
                if (tlen == 0) return false;
                ++t; --tlen;
                break;

            case '[': {
                if (tlen == 0) return false;
                ++p; --plen;
                const bool negate = (plen > 0 && p[0] == '^');
                if (negate) { ++p; --plen; }

                bool matched = false;
                while (true) {
                    if (plen == 0) break;                      // unterminated class
                    if (p[0] == ']') { ++p; --plen; break; }
                    if (p[0] == '\\' && plen >= 2) {
                        ++p; --plen;
                        if (p[0] == t[0]) matched = true;
                    } else if (plen >= 3 && p[1] == '-' && p[2] != ']') {
                        char lo = p[0];
                        char hi = p[2];
                        if (lo > hi) std::swap(lo, hi);
                        if (t[0] >= lo && t[0] <= hi) matched = true;
                        p += 2; plen -= 2;
                    } else if (p[0] == t[0]) {
                        matched = true;
                    }
                    ++p; --plen;
                }
                if (negate) matched = !matched;
                if (!matched) return false;
                ++t; --tlen;
                continue; // 'p' was already advanced past the closing bracket
            }

            case '\\':
                if (plen >= 2) { ++p; --plen; }
                [[fallthrough]];

            default:
                if (tlen == 0 || p[0] != t[0]) return false;
                ++t; --tlen;
                break;
        }
        ++p; --plen;

        // A pattern tail made only of '*' can still match an exhausted text.
        if (tlen == 0) {
            while (plen > 0 && p[0] == '*') { ++p; --plen; }
            break;
        }
    }
    return plen == 0 && tlen == 0;
}

} // namespace

bool globMatch(std::string_view pattern, std::string_view text) {
    return globMatchImpl(pattern.data(), pattern.size(), text.data(), text.size());
}

std::uint64_t fnv1a64(std::string_view s) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char c : s) {
        hash ^= c;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

bool parseMemorySize(std::string_view s, std::int64_t& bytes) {
    if (s.empty()) return false;

    size_t digits = 0;
    while (digits < s.size() && (std::isdigit(static_cast<unsigned char>(s[digits])) ||
                                 (digits == 0 && s[digits] == '-'))) {
        ++digits;
    }
    if (digits == 0) return false;

    std::int64_t value = 0;
    if (!parseInt64(s.substr(0, digits), value)) return false;

    const std::string unit = toLower(trim(s.substr(digits)));
    std::int64_t multiplier = 1;
    if (unit.empty() || unit == "b") multiplier = 1;
    else if (unit == "k")            multiplier = 1000;
    else if (unit == "kb")           multiplier = 1024;
    else if (unit == "m")            multiplier = 1000LL * 1000;
    else if (unit == "mb")           multiplier = 1024LL * 1024;
    else if (unit == "g")            multiplier = 1000LL * 1000 * 1000;
    else if (unit == "gb")           multiplier = 1024LL * 1024 * 1024;
    else return false;

    const std::int64_t magnitude = value < 0 ? -value : value;
    if (magnitude != 0 && multiplier > INT64_MAX / magnitude) return false;
    bytes = value * multiplier;
    return true;
}

std::string formatMemorySize(std::int64_t bytes) {
    static const char* kUnits[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buf[48];
    const int n = (unit == 0)
        ? std::snprintf(buf, sizeof(buf), "%lldB", static_cast<long long>(bytes))
        : std::snprintf(buf, sizeof(buf), "%.2f%s", value, kUnits[unit]);
    return std::string(buf, static_cast<size_t>(n));
}

} // namespace miniredis
