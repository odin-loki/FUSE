#pragma once

#include <string_view>

namespace fuse::math {

/// Parse an RGB color from `text`.
///
/// Accepts "#ff8000", "ff8000", and "#FF8000". Each channel is the hex byte divided by 255.
/// "#ff8000" yields r = 1, g = 128/255, b = 0. Empty text, the wrong length, non-hex
/// characters, and an alpha channel fail. On failure, r, g, and b are set to 0.
[[nodiscard]] inline bool parse_hex_rgb(std::string_view text, float& r, float& g, float& b) {
    const auto fail = [&]() {
        r = 0.f;
        g = 0.f;
        b = 0.f;
        return false;
    };

    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1);
    }
    if (text.size() != 6) {
        return fail();
    }

    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };

    const auto byteAt = [&](std::string_view::size_type index) -> int {
        const int hi = nibble(text[index]);
        const int lo = nibble(text[index + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        return (hi << 4) | lo;
    };

    const int red = byteAt(0);
    const int green = byteAt(2);
    const int blue = byteAt(4);
    if (red < 0 || green < 0 || blue < 0) {
        return fail();
    }

    r = static_cast<float>(red) / 255.f;
    g = static_cast<float>(green) / 255.f;
    b = static_cast<float>(blue) / 255.f;
    return true;
}

} // namespace fuse::math
