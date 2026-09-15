#pragma once

#include <cstdio>
#include <cstdlib>

namespace fuse::demo {

inline int& failureCount() {
    static int failures = 0;
    return failures;
}

inline void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failureCount();
    }
}

inline int finish(const char* demoName) {
    if (failureCount() == 0) {
        std::printf("%s: PASS\n", demoName);
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "%s: %d failure(s)\n", demoName, failureCount());
    return EXIT_FAILURE;
}

} // namespace fuse::demo
