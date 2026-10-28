// WP-0.8 core_logic test driver: `fuse_core_logic_tests [rg|vsm|residency|all]`.
#include "cl_test_util.hpp"
#include "logic_step.hpp"

#include <cstring>

namespace {

// Steps 0..8 inclusive. No heap.
int testLogicStepOk() {
    if (!fuse::renderer::logic_step_ok(0)) {
        std::fprintf(stderr, "FAIL: testLogicStepOk: logic_step_ok(0) should be true\n");
        return 1;
    }
    if (!fuse::renderer::logic_step_ok(8)) {
        std::fprintf(stderr, "FAIL: testLogicStepOk: logic_step_ok(8) should be true\n");
        return 1;
    }
    if (fuse::renderer::logic_step_ok(-1)) {
        std::fprintf(stderr, "FAIL: testLogicStepOk: logic_step_ok(-1) should be false\n");
        return 1;
    }
    if (fuse::renderer::logic_step_ok(9)) {
        std::fprintf(stderr, "FAIL: testLogicStepOk: logic_step_ok(9) should be false\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (const int logicStepRc = testLogicStepOk()) {
        return logicStepRc;
    }
    const char* which = argc > 1 ? argv[1] : "all";
    const bool all = std::strcmp(which, "all") == 0;
    bool ran = false;
    if (all || std::strcmp(which, "rg") == 0) {
        ran = true;
        run_rg_tests();
    }
    if (all || std::strcmp(which, "vsm") == 0) {
        ran = true;
        run_vsm_tests();
    }
    if (all || std::strcmp(which, "residency") == 0) {
        ran = true;
        run_residency_tests();
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite '%s' (rg|vsm|residency|all)\n", which);
        return 2;
    }
    const int f = cltest::failures();
    std::printf("core_logic[%s]: %s (%d failed checks)\n", which, f == 0 ? "PASS" : "FAIL", f);
    return f == 0 ? 0 : 1;
}
