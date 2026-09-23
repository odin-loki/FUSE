// FUSE Relight: hash <-> text conversions (see hash_string.hpp for the upstream behaviour).
#include <fuse/relight/hash/hash_string.hpp>

namespace fuse::relight::hash {

namespace {

struct StrtoullResult {
    bool anyDigits = false;
    bool overflow = false;
    Hash64 value = 0;
};

int hexValue(char c) noexcept {
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
}

bool isCSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// strtoull(text, &end, 16) in the "C" locale, as the Windows CRT implements it. On overflow the
// result is 2^64 - 1 whatever the sign (glibc and UCRT; Wine's msvcrt negates it instead).
StrtoullResult strtoull16(std::string_view s) noexcept {
    StrtoullResult r;
    std::size_t i = 0;
    while (i < s.size() && isCSpace(s[i])) {
        ++i;
    }
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        ++i;
    }
    // A "0x"/"0X" prefix is skipped. Without a hex digit after it the Windows CRT (which Remix
    // runs on; UCRT and msvcrt agree) reports no conversion at all, whereas glibc would parse the
    // "0": FUSE follows Windows, so std::stoull("0x", 16) is an invalid_argument here too.
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        if (i + 2 >= s.size() || hexValue(s[i + 2]) < 0) {
            return r; // no digits
        }
        i += 2;
    }
    for (; i < s.size(); ++i) {
        const int d = hexValue(s[i]);
        if (d < 0) {
            break;
        }
        r.anyDigits = true;
        if (r.value > (~Hash64(0) >> 4)) {
            r.overflow = true;
        }
        r.value = (r.value << 4) | Hash64(d);
    }
    if (r.overflow) {
        r.value = ~Hash64(0);
    } else if (negative) {
        r.value = Hash64(0) - r.value;
    }
    return r;
}

} // namespace

std::string hashToString(Hash64 hash) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[std::size_t(i)] = kDigits[hash & 0xfu];
        hash >>= 4;
    }
    return out;
}

std::string hashToOptionString(Hash64 hash) {
    return "0x" + hashToString(hash);
}

std::string primName(std::string_view prefix, Hash64 hash) {
    return std::string(prefix) + hashToString(hash);
}

std::optional<Hash64> parseHashOption(std::string_view text) {
    const StrtoullResult r = strtoull16(text);
    if (!r.anyDigits || r.overflow) {
        return std::nullopt;
    }
    return r.value;
}

Hash64 hashFromPrimName(std::string_view name, std::string_view prefix) {
    if (name.substr(0, prefix.size()) != prefix) {
        return 0; // Not a replacement
    }
    return strtoull16(name.substr(prefix.size())).value;
}

} // namespace fuse::relight::hash
