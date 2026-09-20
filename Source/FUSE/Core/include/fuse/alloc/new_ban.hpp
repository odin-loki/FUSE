#pragma once

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::alloc {

#if defined(FUSE_DEBUG) && FUSE_DEBUG
/// Thread-local armed flag. When true, fuse_core heap hooks report/abort.
/// Not a process-wide `::operator new` replacement — Qt/STL stay on the CRT heap.
extern thread_local bool engineHeapGuard;
#else
inline constexpr bool engineHeapGuard = false;
#endif

/// Arms `engineHeapGuard` for the current thread until destruction (nested-safe).
class HeapGuard {
public:
    HeapGuard();
    ~HeapGuard();

    HeapGuard(const HeapGuard&) = delete;
    HeapGuard& operator=(const HeapGuard&) = delete;

private:
    bool m_previous = false;
};

using BanHandlerFn = void (*)(const char* kind, void* userData);

/// Testable ban sink. When set, an armed hit records via the callback and does not abort.
void setBanHandler(BanHandlerFn handler, void* userData = nullptr);
void clearBanHandler();

bool isEngineHeapGuardArmed();

/// If the current-thread guard is armed (FUSE_DEBUG only), invoke the ban handler or abort.
void checkEngineHeapBan(const char* kind);

/// malloc/free wrappers for new allocators. Check the guard, then call the CRT heap.
void* checkedMalloc(std::size_t size);
void checkedFree(void* ptr);

/// fuse_core-TU probe: runs the same ban check as a naked `operator new`/`delete`.
void* invokeBannedOperatorNew(std::size_t size);
void invokeBannedOperatorDelete(void* ptr) noexcept;

} // namespace fuse::alloc

#if defined(FUSE_ENGINE_HEAP_GUARD) && FUSE_ENGINE_HEAP_GUARD && !defined(FUSE_NEW_BAN_NO_KEYWORD_INTERCEPT)
// Keyword intercept for fuse_core TUs only (PRIVATE compile def). Does not replace
// ::operator new for the process. Include this header after CRT/STL headers.
#define malloc ::fuse::alloc::checkedMalloc
#define free ::fuse::alloc::checkedFree
#define new if (::fuse::alloc::checkEngineHeapBan("operator new"), false) {} else new
#define delete if (::fuse::alloc::checkEngineHeapBan("operator delete"), false) {} else delete
#endif
