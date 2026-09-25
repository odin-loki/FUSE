// FUSE Relight RL-1.1: SHA-256 (FIPS 180-4).
#include "sha256.hpp"

#include <cstring>

namespace fuse::relight::tap::detail {

namespace {
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
} // namespace

Sha256::Sha256()
    : m_h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, m_buf{} {}

void Sha256::block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
               (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
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
    m_totalBytes += size;
    if (m_bufLen) {
        const std::size_t take = size < 64 - m_bufLen ? size : 64 - m_bufLen;
        std::memcpy(m_buf + m_bufLen, p, take);
        m_bufLen += take;
        p += take;
        size -= take;
        if (m_bufLen == 64) {
            block(m_buf);
            m_bufLen = 0;
        }
    }
    while (size >= 64) {
        block(p);
        p += 64;
        size -= 64;
    }
    if (size) {
        std::memcpy(m_buf, p, size);
        m_bufLen = size;
    }
}

std::string Sha256::hexDigest() {
    const std::uint64_t bits = m_totalBytes * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (m_bufLen != 56) {
        update(&zero, 1);
    }
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) {
        len[i] = std::uint8_t(bits >> (56 - 8 * i));
    }
    update(len, 8);
    static const char* kHex = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 4; ++b) {
            const std::uint8_t byte = std::uint8_t(m_h[i] >> (24 - 8 * b));
            out[std::size_t(8 * i + 2 * b)] = kHex[byte >> 4];
            out[std::size_t(8 * i + 2 * b + 1)] = kHex[byte & 15];
        }
    }
    return out;
}

std::string sha256Hex(const void* data, std::size_t size) {
    Sha256 s;
    s.update(data, size);
    return s.hexDigest();
}

} // namespace fuse::relight::tap::detail
