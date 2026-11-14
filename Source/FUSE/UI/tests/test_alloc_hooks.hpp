// Counting replacements of the global allocation functions for the UI gates that check heap allocations (zero-alloc
// steady frames, allocation budgets). Every replaceable form is replaced: plain, array, nothrow and aligned
// (C++17 align_val_t) new, and every matching delete. A form left to the runtime would allocate through the runtime's
// operator new and could be freed through ours (std::stable_sort's temporary buffer uses nothrow new: an
// alloc-dealloc mismatch under ASan), and would go uncounted.
//
// Include in exactly one translation unit of the executable, which defines the counting hook
//     void fuse::ui::test::countAlloc(std::size_t bytes) noexcept
// called once per allocation through any form (bytes = the requested size).
#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

// GCC may inline the replacement operators into callers and then report a false -Wmismatched-new-delete at -O2;
// replacement functions must not be inline anyway.
#ifndef FUSE_TEST_REPLACEMENT_NOINLINE
#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif
#endif

namespace fuse::ui::test {
/// Defined by the including test: called for every allocation made through a replaced operator new.
void countAlloc(std::size_t bytes) noexcept;

namespace alloc_hooks_detail {
inline void* allocPlain(std::size_t n) noexcept { return std::malloc(n ? n : 1); }
inline void* allocAligned(std::size_t n, std::align_val_t al) noexcept {
    std::size_t a = static_cast<std::size_t>(al);
    if (a < sizeof(void*)) a = sizeof(void*);
    if (n == 0) n = 1;
#if defined(_WIN32)
    return _aligned_malloc(n, a);
#else
    void* p = nullptr;
    return posix_memalign(&p, a, n) == 0 ? p : nullptr;
#endif
}
inline void freePlain(void* p) noexcept { std::free(p); }
inline void freeAligned(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
/// The throwing forms: retry through the new-handler, then throw (as the standard operator new does).
template <class Alloc>
inline void* allocOrThrow(Alloc alloc) {
    for (;;) {
        if (void* p = alloc()) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}
} // namespace alloc_hooks_detail
} // namespace fuse::ui::test

// ---- new ----
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t n) {
    fuse::ui::test::countAlloc(n);
    return fuse::ui::test::alloc_hooks_detail::allocOrThrow(
        [n] { return fuse::ui::test::alloc_hooks_detail::allocPlain(n); });
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t n) { return ::operator new(n); }
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    fuse::ui::test::countAlloc(n);
    return fuse::ui::test::alloc_hooks_detail::allocPlain(n);
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t n, std::align_val_t al) {
    fuse::ui::test::countAlloc(n);
    return fuse::ui::test::alloc_hooks_detail::allocOrThrow(
        [n, al] { return fuse::ui::test::alloc_hooks_detail::allocAligned(n, al); });
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t n, std::align_val_t al) {
    return ::operator new(n, al);
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    fuse::ui::test::countAlloc(n);
    return fuse::ui::test::alloc_hooks_detail::allocAligned(n, al);
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t n, std::align_val_t al,
                                                    const std::nothrow_t& t) noexcept {
    return ::operator new(n, al, t);
}

// ---- delete ----
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept {
    fuse::ui::test::alloc_hooks_detail::freePlain(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept {
    fuse::ui::test::alloc_hooks_detail::freePlain(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept {
    fuse::ui::test::alloc_hooks_detail::freePlain(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept {
    fuse::ui::test::alloc_hooks_detail::freePlain(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, const std::nothrow_t&) noexcept {
    fuse::ui::test::alloc_hooks_detail::freePlain(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, const std::nothrow_t&) noexcept {
    fuse::ui::test::alloc_hooks_detail::freePlain(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::align_val_t) noexcept {
    fuse::ui::test::alloc_hooks_detail::freeAligned(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::align_val_t) noexcept {
    fuse::ui::test::alloc_hooks_detail::freeAligned(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    fuse::ui::test::alloc_hooks_detail::freeAligned(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    fuse::ui::test::alloc_hooks_detail::freeAligned(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    fuse::ui::test::alloc_hooks_detail::freeAligned(p);
}
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    fuse::ui::test::alloc_hooks_detail::freeAligned(p);
}
