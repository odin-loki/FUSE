// Shared helpers for the B5 RHI gate-row tests (fuse_b5_rhi_*): failure accounting and the
// CTest skip convention (exit 77 = SKIP_RETURN_CODE when no Vulkan backend / device / tool).
#pragma once

#include <cstdio>
#include <cstdlib>

namespace b5rhi {

constexpr int kSkipReturnCode = 77;

inline int& failures() {
    static int count = 0;
    return count;
}

inline void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures();
    }
}

inline int skip(const char* testName, const char* reason) {
    std::printf("SKIP %s: %s\n", testName, reason);
    return kSkipReturnCode;
}

inline int finish(const char* testName) {
    if (failures() == 0) {
        std::printf("%s: all checks passed\n", testName);
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "%s: %d failure(s)\n", testName, failures());
    return EXIT_FAILURE;
}

} // namespace b5rhi
