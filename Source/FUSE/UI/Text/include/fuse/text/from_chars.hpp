// fuse::text::fromChars / toChars: std::from_chars / std::to_chars for every arithmetic type on every toolchain.
//
// libc++ only gained the floating-point std::from_chars overloads in LLVM 20, so the Android NDK and Apple clang
// reject them. Where the standard library has them (__cpp_lib_to_chars) this forwards; otherwise floating-point
// input is parsed with strtod on a bounded copy (decimal / exponent forms as std::chars_format::general, no leading
// '+' or whitespace, same result fields). Integers always forward.
#pragma once

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <type_traits>

namespace fuse::text {

template <class T>
    requires std::is_integral_v<T>
inline std::from_chars_result fromChars(const char* first, const char* last, T& out) {
    return std::from_chars(first, last, out);
}

template <class T>
    requires std::is_floating_point_v<T>
inline std::from_chars_result fromChars(const char* first, const char* last, T& out) {
#if defined(__cpp_lib_to_chars) && !defined(FUSE_TEXT_FROM_CHARS_FALLBACK)
    return std::from_chars(first, last, out);
#else
    // the longest prefix that can be a number: [-] digits [. digits] [(e|E) [+|-] digits] | [-] inf / nan
    const char* p = first;
    if (p < last && *p == '-') ++p;
    const char* digits = p;
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    auto word = [&](const char* w) {
        const std::size_t n = std::strlen(w);
        if (std::size_t(last - p) < n) return false;
        for (std::size_t i = 0; i < n; ++i)
            if ((p[i] | 0x20) != w[i]) return false;
        return true;
    };
    if (word("inf")) p += word("infinity") ? 8 : 3;
    else if (word("nan")) p += 3;
    else {
        while (p < last && isDigit(*p)) ++p;
        if (p < last && *p == '.') {
            ++p;
            while (p < last && isDigit(*p)) ++p;
        }
        if (p == digits || (p == digits + 1 && *digits == '.')) return {first, std::errc::invalid_argument};
        if (p < last && (*p == 'e' || *p == 'E')) {
            const char* e = p + 1;
            if (e < last && (*e == '+' || *e == '-')) ++e;
            if (e < last && isDigit(*e)) {
                while (e < last && isDigit(*e)) ++e;
                p = e;
            }
        }
    }
    char buf[128];
    const std::size_t n = std::size_t(p - first);
    if (n >= sizeof buf) return {first, std::errc::invalid_argument};
    std::memcpy(buf, first, n);
    buf[n] = '\0';
    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    if (end == buf) return {first, std::errc::invalid_argument};
    out = static_cast<T>(v);
    return {first + (end - buf), std::errc{}};
#endif
}

/// std::to_chars(first, last, value) for floating point: the shortest text that parses back to `value`. Apple marks
/// the standard overloads unavailable below macOS 13.3 / iOS 16.3, and older libc++ lacks them; there this prints
/// with the smallest %g precision that round-trips. Integers forward.
template <class T>
    requires std::is_integral_v<T>
inline std::to_chars_result toChars(char* first, char* last, T value) {
    return std::to_chars(first, last, value);
}

template <class T>
    requires std::is_floating_point_v<T>
inline std::to_chars_result toChars(char* first, char* last, T value) {
#if defined(__cpp_lib_to_chars) && !defined(__APPLE__) && !defined(FUSE_TEXT_FROM_CHARS_FALLBACK)
    return std::to_chars(first, last, value);
#else
    char buf[64];
    int n = 0;
    const int maxDigits = sizeof(T) <= sizeof(float) ? 9 : 17;
    for (int digits = 1; digits <= maxDigits; ++digits) {
        n = std::snprintf(buf, sizeof buf, "%.*g", digits, static_cast<double>(value));
        if (static_cast<T>(std::strtod(buf, nullptr)) == value || value != value) break;
    }
    if (n <= 0 || n > last - first) return {last, std::errc::value_too_large};
    std::memcpy(first, buf, std::size_t(n));
    return {first + n, std::errc{}};
#endif
}

} // namespace fuse::text
