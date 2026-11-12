// FUSE Remaster W0.1-W0.3: header-only content digests (plan §1.3, §2.1).
//
//   SHA-256 (FIPS 180-4)  blob addresses (<store>/blobs/sha256/<ab>/<sha256>) and canonical `hash_key` values;
//   SHA-1   (FIPS 180-4)  only for RFC 4122 name-based UUIDv5, the `oaid` of an original asset.
// Plain implementations of the published algorithms; no third-party code.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace fuse::remaster {

namespace detail {
inline constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline constexpr std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
} // namespace detail

/// Shared Merkle-Damgard padding / buffering for the two 64-byte-block hashes.
template <class Derived>
class BlockHash {
public:
    void update(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        m_bytes += size;
        while (size > 0) {
            const std::size_t n = std::min<std::size_t>(64 - m_used, size);
            std::memcpy(m_buf.data() + m_used, p, n);
            m_used += n;
            p += n;
            size -= n;
            if (m_used == 64) {
                static_cast<Derived*>(this)->block(m_buf.data());
                m_used = 0;
            }
        }
    }
    void update(std::string_view s) { update(s.data(), s.size()); }

protected:
    void finish() {
        const std::uint64_t bits = m_bytes * 8;
        const std::uint8_t one = 0x80;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (m_used != 56) {
            update(&zero, 1);
        }
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) {
            len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        }
        update(len, 8);
    }
    std::array<std::uint8_t, 64> m_buf{};
    std::size_t m_used = 0;
    std::uint64_t m_bytes = 0;
};

class Sha256 : public BlockHash<Sha256> {
public:
    std::array<std::uint8_t, 32> digest() {
        finish();
        std::array<std::uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i) {
            for (int b = 0; b < 4; ++b) {
                out[std::size_t(i * 4 + b)] = static_cast<std::uint8_t>(m_h[std::size_t(i)] >> (24 - 8 * b));
            }
        }
        return out;
    }
    void block(const std::uint8_t* p) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        using detail::rotr;
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = detail::be32(p + 4 * i);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        m_h[0] += a, m_h[1] += b, m_h[2] += c, m_h[3] += d, m_h[4] += e, m_h[5] += f, m_h[6] += g, m_h[7] += h;
    }

private:
    std::array<std::uint32_t, 8> m_h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
};

class Sha1 : public BlockHash<Sha1> {
public:
    std::array<std::uint8_t, 20> digest() {
        finish();
        std::array<std::uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i) {
            for (int b = 0; b < 4; ++b) {
                out[std::size_t(i * 4 + b)] = static_cast<std::uint8_t>(m_h[std::size_t(i)] >> (24 - 8 * b));
            }
        }
        return out;
    }
    void block(const std::uint8_t* p) {
        using detail::rotl;
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = detail::be32(p + 4 * i);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d), k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d, k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d, k = 0xCA62C1D6;
            }
            const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        m_h[0] += a, m_h[1] += b, m_h[2] += c, m_h[3] += d, m_h[4] += e;
    }

private:
    std::array<std::uint32_t, 5> m_h{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
};

inline std::string toHex(const std::uint8_t* data, std::size_t size) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        s += kHex[data[i] >> 4];
        s += kHex[data[i] & 15];
    }
    return s;
}

/// Lower-case hex SHA-256 of `data`.
inline std::string sha256Hex(const void* data, std::size_t size) {
    Sha256 h;
    h.update(data, size);
    const auto d = h.digest();
    return toHex(d.data(), d.size());
}
inline std::string sha256Hex(std::string_view s) { return sha256Hex(s.data(), s.size()); }

inline bool isSha256Hex(std::string_view s) {
    if (s.size() != 64) {
        return false;
    }
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

using Uuid = std::array<std::uint8_t, 16>;

/// RFC 4122 Appendix C NameSpace_URL (6ba7b811-9dad-11d1-80b4-00c04fd430c8).
inline Uuid uuidNamespaceUrl() {
    return {0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
}

/// RFC 4122 §4.3 name-based UUID, version 5 (SHA-1), of `name` in `ns`.
inline Uuid uuidV5(const Uuid& ns, std::string_view name) {
    Sha1 h;
    h.update(ns.data(), ns.size());
    h.update(name);
    const auto d = h.digest();
    Uuid u{};
    std::memcpy(u.data(), d.data(), 16);
    u[6] = static_cast<std::uint8_t>((u[6] & 0x0F) | 0x50);
    u[8] = static_cast<std::uint8_t>((u[8] & 0x3F) | 0x80);
    return u;
}

/// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (lower case).
inline std::string uuidString(const Uuid& u) {
    std::string hex = toHex(u.data(), u.size());
    return hex.substr(0, 8) + '-' + hex.substr(8, 4) + '-' + hex.substr(12, 4) + '-' + hex.substr(16, 4) + '-' + hex.substr(20);
}

} // namespace fuse::remaster
