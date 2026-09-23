// FUSE Relight RL-0.5 tests: the text "case line" format shared by the C++ tests and the Python
// reference (Tools/FUSE/Relight/remix_hash_ref.py).
//
// One case per line: `<function> key=value key=value ...`, no spaces inside values. Byte strings
// are written as one of
//   hex:<hex>            explicit bytes ("hex:" alone is an empty / null buffer);
//   gen:<seed>:<len>     <len> bytes of the splitmix64 stream seeded with <seed> (hex), each
//                        64-bit output stored little-endian;
//   fgen:<seed>:<words>  <words> float bit patterns (4 bytes each, little-endian) derived from the
//                        same stream: each output gives two 32-bit words (low half first), and each
//                        word w maps by (w & 7): 0 -> kSpecialFloats[(w >> 3) % 16], 1 -> w itself,
//                        2..7 -> sign (w >> 3) & 1, exponent 117 + ((w >> 4) % 24), mantissa w >> 9.
// Integers are decimal, 64-bit hashes 16 lower-case hex digits, float bit patterns 8 hex digits.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::relight::hash::test {

using Bytes = std::vector<std::uint8_t>;
using KeyValues = std::vector<std::pair<std::string, std::string>>;

inline constexpr std::uint32_t kSpecialFloats[16] = {
    0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00001u, 0x7f800001u, 0x00000001u,
    0x807fffffu, 0x7f7fffffu, 0xff7fffffu, 0x00800000u, 0x3f800000u, 0xbf800000u, 0x4b000000u, 0x4b7fffffu,
};

/// splitmix64 (Steele, Lea, Flood 2014).
struct SplitMix64 {
    std::uint64_t state = 0;
    std::uint64_t next() noexcept;
};

/// Test-case random source (splitmix64 as well; independent of the byte generators' seeds).
struct Rng {
    SplitMix64 sm;
    explicit Rng(std::uint64_t seed) : sm{seed} {}
    std::uint64_t u64() noexcept { return sm.next(); }
    std::uint32_t u32() noexcept { return std::uint32_t(sm.next() >> 32); }
    /// Uniform in [0, n) (n > 0).
    std::uint32_t below(std::uint32_t n) noexcept { return std::uint32_t((std::uint64_t(u32()) * n) >> 32); }
    /// Uniform in [lo, hi].
    std::uint32_t range(std::uint32_t lo, std::uint32_t hi) noexcept { return lo + below(hi - lo + 1); }
    bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }
};

Bytes genBytes(std::uint64_t seed, std::size_t length);
Bytes fgenBytes(std::uint64_t seed, std::size_t words);

/// A byte string plus the spec text it is written as.
struct ByteSpec {
    std::string spec;
    Bytes bytes;
};

ByteSpec specHex(std::span<const std::uint8_t> bytes);
ByteSpec specGen(std::uint64_t seed, std::size_t length);
ByteSpec specFgen(std::uint64_t seed, std::size_t words);
/// Decodes any of the three spellings (nullopt on malformed text).
std::optional<Bytes> decodeBytes(std::string_view spec);

std::string hex64(std::uint64_t v);
std::string hex32(std::uint32_t v);
std::optional<std::uint64_t> parseU64(std::string_view text, int base);

/// Splits a line into its function name and key/value pairs.
bool parseCaseLine(std::string_view line, std::string& function, KeyValues& kv);
std::string formatCaseLine(std::string_view function, const KeyValues& kv);

/// Lookup helpers (nullptr / nullopt when missing).
const std::string* findValue(const KeyValues& kv, std::string_view key);
std::vector<std::string> splitList(std::string_view text, char separator);

/// Required-value accessors for compute functions; they throw std::runtime_error when a key is
/// missing or malformed.
[[noreturn]] void fail(const std::string& message);
const std::string& req(const KeyValues& kv, std::string_view key);
std::uint64_t reqDec(const KeyValues& kv, std::string_view key);
std::uint32_t reqU32(const KeyValues& kv, std::string_view key);
std::uint64_t reqHex(const KeyValues& kv, std::string_view key);
Bytes reqBytes(const KeyValues& kv, std::string_view key);
/// Comma-separated decimal list ("-" = empty).
std::string listU32(std::span<const std::uint32_t> values);
std::vector<std::uint32_t> parseListU32(const std::string& text);

} // namespace fuse::relight::hash::test
