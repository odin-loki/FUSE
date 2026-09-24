// FUSE Relight RL-3.3 tests: deterministic BC4/BC5/BC6H/BC7 block sets and the digests of their decoded
// texels, shared by rl_mods_assets_unit and the offline oracle that produced the expected digests.
//
// The expected values were generated once with bcdec (https://github.com/iOrange/bcdec @ 80859ed3,
// dual-licensed Unlicense / MIT, an independent decoder; used offline only, not vendored):
//   bcdec_bc7, bcdec_bc6h_half (unsigned and signed), bcdec_bc4_float with BCDEC_BC4BC5_PRECISE
//   (unorm and snorm)
// over exactly the blocks below (every BC7 mode + the invalid mode byte, every BC6H mode + the four
// reserved mode codes). BC1-BC3 are not in this set: their palette rounding follows capture/export's
// decodeRgba8 (RL-1.8), which the unit test compares against bit for bit instead.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rl_bc_oracle {

struct Lcg {
    std::uint64_t s;
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(s >> 33);
    }
};

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ b[i]) * 1099511628211ull;
        }
    }
    void u32(std::uint32_t v) { bytes(&v, 4); }
};

inline void randomBlock(Lcg& g, std::uint8_t* b, int n) {
    for (int i = 0; i < n; ++i) {
        b[i] = static_cast<std::uint8_t>(g.next() >> 7);
    }
}

constexpr int kBlocksPerMode = 256;

/// BC7: 256 blocks per mode 0..7 (mode bits forced), then 4 blocks with a zero mode byte.
inline std::vector<std::uint8_t> bc7Blocks() {
    Lcg g{0xbc7};
    std::vector<std::uint8_t> out;
    for (int mode = 0; mode <= 8; ++mode) {
        const int count = mode == 8 ? 4 : kBlocksPerMode;
        for (int i = 0; i < count; ++i) {
            std::uint8_t b[16];
            randomBlock(g, b, 16);
            if (mode == 8) {
                b[0] = 0;
            } else {
                b[0] = static_cast<std::uint8_t>((b[0] & ~((2u << mode) - 1u)) | (1u << mode));
            }
            out.insert(out.end(), b, b + 16);
        }
    }
    return out;
}

/// BC6H mode codes: the 14 valid ones, then the reserved 5-bit codes.
constexpr std::uint8_t kBc6Codes[18] = {0x00, 0x01, 0x02, 0x06, 0x0a, 0x0e, 0x12, 0x16, 0x1a,
                                        0x1e, 0x03, 0x07, 0x0b, 0x0f, 0x13, 0x17, 0x1b, 0x1f};

inline std::vector<std::uint8_t> bc6Blocks() {
    Lcg g{0xbc6};
    std::vector<std::uint8_t> out;
    for (std::uint8_t code : kBc6Codes) {
        for (int i = 0; i < kBlocksPerMode; ++i) {
            std::uint8_t b[16];
            randomBlock(g, b, 16);
            const std::uint8_t mask = code < 2 ? 0x3 : 0x1f;
            b[0] = static_cast<std::uint8_t>((b[0] & ~mask) | code);
            out.insert(out.end(), b, b + 16);
        }
    }
    return out;
}

/// BC4 blocks (8 bytes); 1024 random, with both endpoint orders well represented.
inline std::vector<std::uint8_t> bc4Blocks() {
    Lcg g{0xbc4};
    std::vector<std::uint8_t> out(1024 * 8);
    randomBlock(g, out.data(), static_cast<int>(out.size()));
    return out;
}

/// Float texels are digested quantised to 2^-20 so the digest does not depend on the last ulp.
inline std::int32_t quantise(float v) { return static_cast<std::int32_t>(std::lround(static_cast<double>(v) * 1048576.0)); }

// Expected digests (bcdec oracle, see above).
constexpr std::uint64_t kBc7Digest = 0xd7502b3fadd25a9dull;
constexpr std::uint64_t kBc6UnsignedDigest = 0x39534fe61b4031e5ull;
constexpr std::uint64_t kBc6SignedDigest = 0x64362859f11a083dull;
constexpr std::uint64_t kBc4UnormDigest = 0x1101c088cdf3c1d1ull;
constexpr std::uint64_t kBc4SnormDigest = 0x048583b2a6c1b946ull;

} // namespace rl_bc_oracle
