#pragma once
// Sanitizer build detection for tests that cannot run meaningfully under
// ASan/UBSan (fork-crash probes, wall-clock timing gates, RSS budgets).
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

namespace fuse::core {

inline constexpr bool kSanitizerBuild = FUSE_SANITIZER_BUILD != 0;

} // namespace fuse::core
