// FUSE Remaster gates: a tiny CHECK harness (plain main(), non-zero exit on failure; no gtest dependency).
#pragma once

#include <cstdio>

namespace remaster_test {
inline int& failures() {
    static int n = 0;
    return n;
}
inline int finish(const char* name) {
    if (failures() == 0) {
        std::printf("%s: PASS\n", name);
        return 0;
    }
    std::printf("%s: FAIL (%d)\n", name, failures());
    return 1;
}
} // namespace remaster_test

#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                          \
            ++remaster_test::failures();                                                                           \
        }                                                                                                          \
    } while (0)
