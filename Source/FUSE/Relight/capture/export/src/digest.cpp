// FUSE Relight RL-1.8: SHA-256, SHA-1 and UUIDv5 (see digest.hpp). Straight from FIPS 180-4 and RFC 4122.
#include <fuse/relight/capture/export/digest.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::capture::exporter {

namespace {

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
constexpr std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/// Merkle-Damgard padding shared by both hashes (big-endian 64-bit bit length).
template <typename Hash>
void finishPadding(Hash& h, std::uint64_t bytes) {
    const std::uint64_t bits = bytes * 8;
    const std::uint8_t one = 0x80;
    h.update(&one, 1);
    const std::uint8_t zero = 0;
    while ((bytes + 1) % 64 != 56) {
        h.update(&zero, 1);
        ++bytes;
    }
    std::uint8_t len[8];
    for (int k = 0; k < 8; ++k) {
        len[k] = static_cast<std::uint8_t>(bits >> (56 - 8 * k));
    }
    h.update(len, 8);
}

} // namespace

// ---- SHA-256 --------------------------------------------------------------------------------------------

Sha256::Sha256() : m_h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int t = 0; t < 16; ++t) {
        w[t] = be32(p + 4 * t);
    }
    for (int t = 16; t < 64; ++t) {
        const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    std::uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];
    for (int t = 0; t < 64; ++t) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + S1 + ch + kSha256K[t] + w[t];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_h[0] += a;
    m_h[1] += b;
    m_h[2] += c;
    m_h[3] += d;
    m_h[4] += e;
    m_h[5] += f;
    m_h[6] += g;
    m_h[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    m_bytes += size;
    while (size > 0) {
        const std::size_t n = std::min(size, m_buf.size() - m_used);
        std::memcpy(m_buf.data() + m_used, p, n);
        m_used += n;
        p += n;
        size -= n;
        if (m_used == 64) {
            block(m_buf.data());
            m_used = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::digest() {
    finishPadding(*this, m_bytes);
    std::array<std::uint8_t, 32> out{};
    for (int k = 0; k < 8; ++k) {
        for (int j = 0; j < 4; ++j) {
            out[4 * k + j] = static_cast<std::uint8_t>(m_h[k] >> (24 - 8 * j));
        }
    }
    return out;
}

std::string Sha256::hexDigest() {
    const auto d = digest();
    return toHex(d.data(), d.size());
}

// ---- SHA-1 ----------------------------------------------------------------------------------------------

Sha1::Sha1() : m_h{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::block(const std::uint8_t* p) {
    std::uint32_t w[80];
    for (int t = 0; t < 16; ++t) {
        w[t] = be32(p + 4 * t);
    }
    for (int t = 16; t < 80; ++t) {
        w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }
    std::uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
    for (int t = 0; t < 80; ++t) {
        std::uint32_t f, k;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const std::uint32_t tmp = rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = tmp;
    }
    m_h[0] += a;
    m_h[1] += b;
    m_h[2] += c;
    m_h[3] += d;
    m_h[4] += e;
}

void Sha1::update(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    m_bytes += size;
    while (size > 0) {
        const std::size_t n = std::min(size, m_buf.size() - m_used);
        std::memcpy(m_buf.data() + m_used, p, n);
        m_used += n;
        p += n;
        size -= n;
        if (m_used == 64) {
            block(m_buf.data());
            m_used = 0;
        }
    }
}

std::array<std::uint8_t, 20> Sha1::digest() {
    finishPadding(*this, m_bytes);
    std::array<std::uint8_t, 20> out{};
    for (int k = 0; k < 5; ++k) {
        for (int j = 0; j < 4; ++j) {
            out[4 * k + j] = static_cast<std::uint8_t>(m_h[k] >> (24 - 8 * j));
        }
    }
    return out;
}

// ---- helpers --------------------------------------------------------------------------------------------

std::string toHex(const std::uint8_t* data, std::size_t size) {
    static const char* kDigits = "0123456789abcdef";
    std::string s;
    s.reserve(size * 2);
    for (std::size_t k = 0; k < size; ++k) {
        s += kDigits[data[k] >> 4];
        s += kDigits[data[k] & 15];
    }
    return s;
}

std::string sha256Hex(const void* data, std::size_t size) {
    Sha256 h;
    h.update(data, size);
    return h.hexDigest();
}

Uuid uuidV5(const Uuid& ns, std::string_view name) {
    Sha1 h;
    h.update(ns.data(), ns.size());
    h.update(name.data(), name.size());
    const auto d = h.digest();
    Uuid u{};
    std::memcpy(u.data(), d.data(), 16);
    u[6] = static_cast<std::uint8_t>((u[6] & 0x0F) | 0x50);
    u[8] = static_cast<std::uint8_t>((u[8] & 0x3F) | 0x80);
    return u;
}

std::string uuidString(const Uuid& u) {
    const std::string h = toHex(u.data(), u.size());
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

Uuid uuidNamespaceUrl() {
    return {0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
}

} // namespace fuse::relight::capture::exporter
