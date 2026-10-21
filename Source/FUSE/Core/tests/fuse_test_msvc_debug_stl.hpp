// MSVC Debug builds use _ITERATOR_DEBUG_LEVEL > 0: the STL heap-allocates _Container_proxy objects
// for std containers, so steady-state "zero heap allocation" gates are not meaningful (see
// steady_state_alloc.cpp). Release / Profile / Shipping MSVC builds stay fully enforced.

#pragma once

#if defined(_MSC_VER) && !defined(NDEBUG) && defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
inline constexpr bool fuse_test_msvc_debug_stl() {
    return true;
}
#else
inline constexpr bool fuse_test_msvc_debug_stl() {
    return false;
}
#endif
