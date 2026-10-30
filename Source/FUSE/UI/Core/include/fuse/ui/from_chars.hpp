// std::from_chars with a floating-point fallback for standard libraries that lack it (libc++ before 20, e.g. the
// Android NDK r27 / Apple toolchains). Integers always go straight to std::from_chars.
#pragma once
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <system_error>
#include <type_traits>

namespace fuse::ui {

template <class T>
std::from_chars_result fromChars(const char* first, const char* last, T& value) {
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000
    if constexpr (std::is_floating_point_v<T>) {
        // from_chars never accepts a leading '+' or whitespace; mirror that before strtod.
        if (first == last || *first == '+' || *first == ' ' || *first == '\t') return {first, std::errc::invalid_argument};
        const std::string tmp(first, last);
        char* end = nullptr;
        errno = 0;
        const long double v = std::strtold(tmp.c_str(), &end);
        if (end == tmp.c_str()) return {first, std::errc::invalid_argument};
        if (errno == ERANGE) return {first + (end - tmp.c_str()), std::errc::result_out_of_range};
        value = T(v);
        return {first + (end - tmp.c_str()), std::errc{}};
    } else
#endif
        return std::from_chars(first, last, value);
}

} // namespace fuse::ui
