// FUSE Relight RL-0.6 options tests (ctest rl_options).
//
// Suites:
//   config    .conf syntax, value parsing/formatting, parse errors and edge cases, hash-set syntax,
//             byte-stable serialization
//   semantics port of dxvk-remix tests/rtx/unit/test_rtx_option.cpp (layering, blending, callbacks,
//             hash-set merging, flags, migration) plus aliases and routing
//   export    port of dxvk-remix tests/rtx/unit/test_option_layer_export.cpp
//   system    dxvk.conf / rtx.conf / user.conf / environment layering from files, save round trip

#include "option_file.hpp"
#include "rl_options_test.hpp"

#include <fuse/relight/options/option_config.hpp>

#include <cstdio>

namespace {

// Option file names: length 1..32, leading ASCII letter, no spaces. No heap.
bool testOptionFileOk() {
    const int before = rl_options_test::totals().failures;
    rl_options_test::totals().currentTest = "testOptionFileOk";
    RL_CHECK(fuse::relight::option_file_ok("Config"));
    RL_CHECK(!fuse::relight::option_file_ok(""));
    RL_CHECK(!fuse::relight::option_file_ok("1Config"));
    RL_CHECK(!fuse::relight::option_file_ok("My Config"));
    rl_options_test::totals().currentTest = "";
    return rl_options_test::totals().failures == before;
}

} // namespace

int main() {
    if (!testOptionFileOk()) {
        return 1;
    }

    using namespace fuse::relight::options;
    // Hermetic: config path overrides from the caller's environment would change what loads.
    setEnvironmentVariable(kDxvkConfEnvVar, "");
    setEnvironmentVariable(kRtxConfEnvVar, "");

    rl_options_test::runConfigTests();
    rl_options_test::runSemanticsTests();
    rl_options_test::runExportTests();
    rl_options_test::runSystemTests();

    const auto& totals = rl_options_test::totals();
    std::printf("rl_options: %d checks, %d failures\n", totals.checks, totals.failures);
    return totals.failures == 0 ? 0 : 1;
}
