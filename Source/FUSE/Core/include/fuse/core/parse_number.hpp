#pragma once

#include <fuse/types.hpp>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

namespace fuse::core {

/// Locale-independent float parse. NDK libc++ lacks `std::from_chars` for floating point.
inline bool parse_f64(std::string_view text, f64& out) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
#if defined(__ANDROID__)
    const std::string buffer(text);
    char* end = nullptr;
    errno = 0;
    const char* const start = buffer.c_str();
    const double value = std::strtod(start, &end);
    if (errno != 0 || end != start + static_cast<std::ptrdiff_t>(buffer.size()) || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
#else
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc() && result.ptr == text.data() + text.size() && std::isfinite(out);
#endif
}

} // namespace fuse::core
