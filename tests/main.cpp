#include "test_framework.hpp"

#include "miniredis/net.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace testing {

int runAll(const char* filter) {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;

    std::string currentSuite;

    for (const auto& testCase : registry()) {
        const std::string fullName = testCase.suite + "." + testCase.name;
        if (filter != nullptr && fullName.find(filter) == std::string::npos) continue;

        if (testCase.suite != currentSuite) {
            currentSuite = testCase.suite;
            std::printf("\n%s\n", currentSuite.c_str());
        }

        const auto started = std::chrono::steady_clock::now();
        std::string failureDetail;
        bool ok = true;

        try {
            testCase.body();
        } catch (const AssertionFailure& failure) {
            ok = false;
            failureDetail = failure.message;
        } catch (const std::exception& error) {
            ok = false;
            failureDetail = std::string("threw std::exception: ") + error.what();
        } catch (...) {
            ok = false;
            failureDetail = "threw an unknown exception";
        }

        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - started).count();

        if (ok) {
            ++passed;
            std::printf("  [ ok ] %-44s %6.1f ms\n", testCase.name.c_str(), elapsed);
        } else {
            ++failed;
            std::printf("  [FAIL] %-44s %6.1f ms\n", testCase.name.c_str(), elapsed);
            std::printf("         %s\n", failureDetail.c_str());
            failures.push_back(fullName);
        }
    }

    std::printf("\n%d passed, %d failed\n", passed, failed);
    if (!failures.empty()) {
        std::printf("\nFailing tests:\n");
        for (const auto& name : failures) std::printf("  %s\n", name.c_str());
    }
    return failed == 0 ? 0 : 1;
}

} // namespace testing

int main(int argc, char* argv[]) {
    // A few tests construct a Server, which opens sockets, so networking has
    // to be up before any case runs.
    miniredis::Net::startup();

    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: miniredis-tests [--filter <substring>]\n";
            miniredis::Net::shutdown();
            return 0;
        }
    }

    const int result = testing::runAll(filter);
    miniredis::Net::shutdown();
    return result;
}
