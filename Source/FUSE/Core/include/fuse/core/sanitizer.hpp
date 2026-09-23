#pragma once
// Sanitizer build detection for tests that cannot run meaningfully under
// ASan/UBSan (fork-crash probes, wall-clock timing gates, RSS budgets), plus
// timingBudgetsEnforced() for wall-clock gates that must be skipped under any
// instrumentation (sanitizers, valgrind).
//
// FUSE_SANITIZE_ADDRESS / FUSE_SANITIZE_UNDEFINED are defined 0/1 by
// fuse_sanitize_finalize() (cmake/FuseSanitizers.cmake) on every FUSE target
// when FUSE_SANITIZE is set; compiler macros cover the older per-target
// FUSE_*_ENABLE_ASAN switches.

#if !defined(FUSE_SANITIZE_ADDRESS)
#  if defined(__SANITIZE_ADDRESS__)
#    define FUSE_SANITIZE_ADDRESS 1
#  elif defined(__has_feature)
#    if __has_feature(address_sanitizer)
#      define FUSE_SANITIZE_ADDRESS 1
#    endif
#  endif
#endif
#if !defined(FUSE_SANITIZE_ADDRESS)
#  define FUSE_SANITIZE_ADDRESS 0
#endif

#if !defined(FUSE_SANITIZE_UNDEFINED)
#  if defined(__has_feature)
#    if __has_feature(undefined_behavior_sanitizer)
#      define FUSE_SANITIZE_UNDEFINED 1
#    endif
#  endif
#endif
#if !defined(FUSE_SANITIZE_UNDEFINED)
#  define FUSE_SANITIZE_UNDEFINED 0
#endif

#define FUSE_SANITIZER_BUILD (FUSE_SANITIZE_ADDRESS || FUSE_SANITIZE_UNDEFINED)

// TSan (FUSE_CORE_ENABLE_TSAN) is not part of FUSE_SANITIZER_BUILD, but it
// still slows code down far too much for wall-clock budgets.
#if !defined(FUSE_SANITIZE_THREAD)
#  if defined(__SANITIZE_THREAD__)
#    define FUSE_SANITIZE_THREAD 1
#  elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#      define FUSE_SANITIZE_THREAD 1
#    endif
#  endif
#endif
#if !defined(FUSE_SANITIZE_THREAD)
#  define FUSE_SANITIZE_THREAD 0
#endif

// valgrind client requests are optional: only used when the header exists.
#if defined(__has_include)
#  if __has_include(<valgrind/valgrind.h>)
#    include <valgrind/valgrind.h>
#    define FUSE_HAVE_VALGRIND_H 1
#  endif
#endif

#include <cstdio>
#include <cstdlib>
#include <string>

namespace fuse::core {

inline constexpr bool kSanitizerBuild = FUSE_SANITIZER_BUILD != 0;

/// True when the binary itself is instrumented (ASan, UBSan or TSan).
inline constexpr bool kInstrumentedBuild = kSanitizerBuild || FUSE_SANITIZE_THREAD != 0;

/// Name of the environment variable set on instrumented test runs (cmake/FuseValgrind.cmake
/// sets FUSE_INSTRUMENTED_RUN=valgrind on every valgrind.* ctest).
inline constexpr const char* kInstrumentedRunEnv = "FUSE_INSTRUMENTED_RUN";

/// True when the process is running under an external instrumentation tool
/// (valgrind detected directly, or FUSE_INSTRUMENTED_RUN set to a non-empty value other than "0").
inline bool instrumentedRun() {
#if defined(FUSE_HAVE_VALGRIND_H)
    if (RUNNING_ON_VALGRIND) {
        return true;
    }
#endif
    const char* env = std::getenv(kInstrumentedRunEnv);
    return env != nullptr && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
}

/// Whether tests should enforce wall-clock timing budgets (e.g. "build < 2 ms").
/// False in sanitizer builds and under valgrind/other instrumented runs; tests must still
/// run their correctness checks and print measured times either way.
inline bool timingBudgetsEnforced() {
    return !kInstrumentedBuild && !instrumentedRun();
}

/// Why timingBudgetsEnforced() is false (e.g. "FUSE_INSTRUMENTED_RUN=wine", "sanitizer build"),
/// or an empty string when budgets are enforced.
inline std::string timingBudgetsSkipReason() {
    if (kSanitizerBuild) {
        return "sanitizer build";
    }
    if (kInstrumentedBuild) {
        return "thread sanitizer build";
    }
#if defined(FUSE_HAVE_VALGRIND_H)
    if (RUNNING_ON_VALGRIND) {
        return "running under valgrind";
    }
#endif
    if (instrumentedRun()) {
        return std::string(kInstrumentedRunEnv) + "=" + std::getenv(kInstrumentedRunEnv);
    }
    return {};
}

/// timingBudgetsEnforced(), and when it is false prints
/// "timing budgets not enforced (<reason>)" once per process. Use for wall-clock
/// assertions; always print the measured value regardless of the result.
inline bool timingBudgetsEnforcedNoted() {
    static const bool enforced = [] {
        const bool on = timingBudgetsEnforced();
        if (!on) {
            std::printf("timing budgets not enforced (%s)\n", timingBudgetsSkipReason().c_str());
            std::fflush(stdout);
        }
        return on;
    }();
    return enforced;
}

} // namespace fuse::core
