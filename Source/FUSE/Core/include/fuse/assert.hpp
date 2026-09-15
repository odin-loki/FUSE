#pragma once

#include <fuse/types.hpp>

namespace fuse::assertion {

struct FatalContext {
    const char* message = nullptr;
    const char* file = nullptr;
    u32 line = 0;
};

using FatalHandlerFn = void (*)(const FatalContext& context, void* userData);

void setFatalHandler(FatalHandlerFn handler, void* userData = nullptr);
void clearFatalHandler();

/// Logs, invokes the fatal handler hook, then aborts unless suppressed for tests.
void fatal(const char* message, const char* file, u32 line);

/// Test hook — when true, `fatal()` returns after the handler instead of aborting.
bool setSuppressAbortForTests(bool suppress);

} // namespace fuse::assertion

#if defined(FUSE_NO_ASSERT) && FUSE_NO_ASSERT
#define FUSE_ASSERT(cond, msg) ((void)(cond))
#define FUSE_VERIFY(cond, msg) ((void)(cond))
#else

#define FUSE_VERIFY(cond, msg)                                                                 \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            ::fuse::assertion::fatal((msg), __FILE__, static_cast<::fuse::u32>(__LINE__));     \
        }                                                                                      \
    } while (0)

#if defined(FUSE_DEBUG) && FUSE_DEBUG
#define FUSE_ASSERT(cond, msg) FUSE_VERIFY(cond, msg)
#else
#define FUSE_ASSERT(cond, msg) ((void)(cond))
#endif

#endif // FUSE_NO_ASSERT

#define FUSE_PRECONDITION(cond) FUSE_ASSERT(cond, "Precondition failed: " #cond)
#define FUSE_POSTCONDITION(cond) FUSE_ASSERT(cond, "Postcondition failed: " #cond)

#if defined(FUSE_NO_ASSERT) && FUSE_NO_ASSERT
#define FUSE_UNREACHABLE(msg) __builtin_unreachable()
#else
#define FUSE_UNREACHABLE(msg)                                                                    \
    do {                                                                                         \
        ::fuse::assertion::fatal((msg), __FILE__, static_cast<::fuse::u32>(__LINE__));           \
        __builtin_unreachable();                                                                 \
    } while (0)
#endif
