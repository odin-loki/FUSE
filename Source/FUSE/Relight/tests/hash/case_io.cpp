// FUSE Relight RL-0.5 tests: case line I/O (see case_io.hpp).
#include "case_io.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace fuse::relight::hash::test {

std::uint64_t SplitMix64::next() noexcept {
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

Bytes genBytes(std::uint64_t seed, std::size_t length) {
    Bytes out(length);
    SplitMix64 sm{seed};
    for (std::size_t i = 0; i < length; i += 8) {
        const std::uint64_t v = sm.next();
        for (std::size_t b = 0; b < 8 && i + b < length; ++b) {
            out[i + b] = std::uint8_t(v >> (8 * b));
        }
    }
    return out;
}

Bytes fgenBytes(std::uint64_t seed, std::size_t words) {
    Bytes out(words * 4);
    SplitMix64 sm{seed};
    std::uint64_t pending = 0;
    for (std::size_t i = 0; i < words; ++i) {
        std::uint32_t w;
        if ((i & 1u) == 0) {
            pending = sm.next();
            w = std::uint32_t(pending);
        } else {
            w = std::uint32_t(pending >> 32);
        }
        std::uint32_t bits;
        const std::uint32_t sel = w & 7u;
        if (sel == 0) {
            bits = kSpecialFloats[(w >> 3) % 16u];
        } else if (sel == 1) {
            bits = w;
        } else {
            bits = (((w >> 3) & 1u) << 31) | ((117u + ((w >> 4) % 24u)) << 23) | (w >> 9);
        }
        for (std::size_t b = 0; b < 4; ++b) {
            out[i * 4 + b] = std::uint8_t(bits >> (8 * b));
        }
    }
    return out;
}

ByteSpec specHex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    ByteSpec s;
    s.spec = "hex:";
    s.spec.reserve(4 + bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        s.spec.push_back(kDigits[b >> 4]);
        s.spec.push_back(kDigits[b & 15]);
    }
    s.bytes.assign(bytes.begin(), bytes.end());
    return s;
}

ByteSpec specGen(std::uint64_t seed, std::size_t length) {
    return {"gen:" + hex64(seed) + ":" + std::to_string(length), genBytes(seed, length)};
}

ByteSpec specFgen(std::uint64_t seed, std::size_t words) {
    return {"fgen:" + hex64(seed) + ":" + std::to_string(words), fgenBytes(seed, words)};
}

std::optional<Bytes> decodeBytes(std::string_view spec) {
    if (spec.starts_with("hex:")) {
        spec.remove_prefix(4);
        if (spec.size() % 2 != 0) {
            return std::nullopt;
        }
        Bytes out(spec.size() / 2);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto v = parseU64(spec.substr(i * 2, 2), 16);
            if (!v) {
                return std::nullopt;
            }
            out[i] = std::uint8_t(*v);
        }
        return out;
    }
    const bool gen = spec.starts_with("gen:");
    const bool fgen = spec.starts_with("fgen:");
    if (!gen && !fgen) {
        return std::nullopt;
    }
    spec.remove_prefix(gen ? 4 : 5);
    const std::size_t colon = spec.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto seed = parseU64(spec.substr(0, colon), 16);
    const auto count = parseU64(spec.substr(colon + 1), 10);
    if (!seed || !count || *count > (1u << 26)) {
        return std::nullopt;
    }
    return gen ? genBytes(*seed, std::size_t(*count)) : fgenBytes(*seed, std::size_t(*count));
}

std::string hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string hex32(std::uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(v));
    return buf;
}

std::optional<std::uint64_t> parseU64(std::string_view text, int base) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (char c : text) {
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return std::nullopt;
        }
        if (d >= base) {
            return std::nullopt;
        }
        v = v * std::uint64_t(base) + std::uint64_t(d);
    }
    return v;
}

bool parseCaseLine(std::string_view line, std::string& function, KeyValues& kv) {
    kv.clear();
    function.clear();
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.empty() || line.front() == '#') {
        return false;
    }
    bool first = true;
    while (!line.empty()) {
        const std::size_t sp = line.find(' ');
        const std::string_view token = line.substr(0, sp);
        if (first) {
            function = token;
            first = false;
        } else if (!token.empty()) {
            const std::size_t eq = token.find('=');
            if (eq == std::string_view::npos) {
                return false;
            }
            kv.emplace_back(std::string(token.substr(0, eq)), std::string(token.substr(eq + 1)));
        }
        if (sp == std::string_view::npos) {
            break;
        }
        line.remove_prefix(sp + 1);
    }
    return !function.empty();
}

std::string formatCaseLine(std::string_view function, const KeyValues& kv) {
    std::string out(function);
    for (const auto& [k, v] : kv) {
        out.push_back(' ');
        out += k;
        out.push_back('=');
        out += v;
    }
    return out;
}

const std::string* findValue(const KeyValues& kv, std::string_view key) {
    for (const auto& [k, v] : kv) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

std::vector<std::string> splitList(std::string_view text, char separator) {
    std::vector<std::string> out;
    while (true) {
        const std::size_t p = text.find(separator);
        out.emplace_back(text.substr(0, p));
        if (p == std::string_view::npos) {
            break;
        }
        text.remove_prefix(p + 1);
    }
    return out;
}

void fail(const std::string& message) {
    throw std::runtime_error(message);
}

const std::string& req(const KeyValues& kv, std::string_view key) {
    const std::string* v = findValue(kv, key);
    if (v == nullptr) {
        fail("missing key '" + std::string(key) + "'");
    }
    return *v;
}

std::uint64_t reqDec(const KeyValues& kv, std::string_view key) {
    const auto v = parseU64(req(kv, key), 10);
    if (!v) {
        fail("bad decimal '" + std::string(key) + "'");
    }
    return *v;
}

std::uint32_t reqU32(const KeyValues& kv, std::string_view key) {
    const std::uint64_t v = reqDec(kv, key);
    if (v > 0xffffffffull) {
        fail("u32 out of range '" + std::string(key) + "'");
    }
    return std::uint32_t(v);
}

std::uint64_t reqHex(const KeyValues& kv, std::string_view key) {
    const auto v = parseU64(req(kv, key), 16);
    if (!v) {
        fail("bad hex '" + std::string(key) + "'");
    }
    return *v;
}

Bytes reqBytes(const KeyValues& kv, std::string_view key) {
    auto b = decodeBytes(req(kv, key));
    if (!b) {
        fail("bad bytes '" + std::string(key) + "'");
    }
    return std::move(*b);
}

std::string listU32(std::span<const std::uint32_t> values) {
    if (values.empty()) {
        return "-";
    }
    std::string s;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) {
            s.push_back(',');
        }
        s += std::to_string(values[i]);
    }
    return s;
}

std::vector<std::uint32_t> parseListU32(const std::string& text) {
    std::vector<std::uint32_t> out;
    if (text == "-") {
        return out;
    }
    for (const std::string& part : splitList(text, ',')) {
        const auto v = parseU64(part, 10);
        if (!v || *v > 0xffffffffull) {
            fail("bad list value");
        }
        out.push_back(std::uint32_t(*v));
    }
    return out;
}

} // namespace fuse::relight::hash::test
