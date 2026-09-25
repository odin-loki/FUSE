// WP-0.8 core_logic test driver: `fuse_core_logic_tests [rg|vsm|residency|all]`.
#include "cl_test_util.hpp"

#include <cstring>

int main(int argc, char** argv) {
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
