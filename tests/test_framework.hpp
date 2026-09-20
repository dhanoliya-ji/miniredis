// A minimal test framework.
//
// Pulling in GoogleTest or Catch2 for a project whose whole point is building
// things from scratch would be odd, and it would make the repository depend on
// a package manager to build. This is about eighty lines and does what the
// suite needs: named cases, assertions that report the file and line, and a
// summary that fails the process on the first non-zero count.
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

// Thrown by a failed assertion; caught by the runner so one bad case does not
// abort the rest of the suite.
struct AssertionFailure {
    std::string message;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int registerCase(const char* suite, const char* name, std::function<void()> body) {
    registry().push_back(TestCase{suite, name, std::move(body)});
    return 0;
}

inline void fail(const char* file, int line, const std::string& detail) {
    throw AssertionFailure{std::string(file) + ":" + std::to_string(line) + "  " + detail};
}

// Renders a value for a failure message. Binary-safe: non-printable bytes are
// escaped so a failing assertion on a binary value is still readable.
template <typename T>
std::string describe(const T& value) {
    if constexpr (std::is_same_v<T, std::string>) {
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('"');
        for (unsigned char c : value) {
            if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c < 32 || c > 126) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\x%02x", c);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        out.push_back('"');
        return out;
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(value);
    } else {
        return "<value>";
    }
}

int runAll(const char* filter);

} // namespace testing

// A test case. The registration happens through a file-scope initialiser,
// which is safe here because the tests are compiled straight into the test
// executable rather than into a library the linker could prune.
#define TEST(suite, name)                                                       \
    static void suite##_##name##_body();                                        \
    static const int suite##_##name##_registration =                            \
        ::testing::registerCase(#suite, #name, suite##_##name##_body);          \
    static void suite##_##name##_body()

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            ::testing::fail(__FILE__, __LINE__, "expected: " #condition);       \
        }                                                                       \
    } while (false)

#define CHECK_EQ(actual, expected)                                              \
    do {                                                                        \
        const auto& actualValue = (actual);                                     \
        const auto& expectedValue = (expected);                                 \
        if (!(actualValue == expectedValue)) {                                  \
            ::testing::fail(__FILE__, __LINE__,                                 \
                            std::string(#actual) + "\n      actual:   " +       \
                                ::testing::describe(actualValue) +              \
                                "\n      expected: " +                          \
                                ::testing::describe(expectedValue));            \
        }                                                                       \
    } while (false)

#define CHECK_NE(actual, unexpected)                                            \
    do {                                                                        \
        if ((actual) == (unexpected)) {                                         \
            ::testing::fail(__FILE__, __LINE__,                                 \
                            std::string(#actual) + " should not equal " +       \
                                ::testing::describe(unexpected));               \
        }                                                                       \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                 \
    do {                                                                        \
        const double difference = static_cast<double>(actual) -                 \
                                  static_cast<double>(expected);                \
        if (difference > (tolerance) || -difference > (tolerance)) {            \
            ::testing::fail(__FILE__, __LINE__,                                 \
                            std::string(#actual) + " = " +                      \
                                std::to_string(static_cast<double>(actual)) +   \
                                ", expected " +                                 \
                                std::to_string(static_cast<double>(expected))); \
        }                                                                       \
    } while (false)
