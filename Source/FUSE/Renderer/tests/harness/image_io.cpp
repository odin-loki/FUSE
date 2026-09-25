#include "image_io.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fuse::renderer::harness {

namespace {

// ---- checksums ---------------------------------------------------------------------------------

const std::array<u32, 256>& crcTable() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 n = 0; n < 256u; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    return table;
}

u32 adler32(const u8* data, usize size) {
    u32 a = 1u;
    u32 b = 0u;
    for (usize i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void putBe32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v >> 24));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

u32 getBe32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

// ---- deflate (LZ77 + fixed Huffman) ------------------------------------------------------------

constexpr u16 kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class BitWriter {
public:
    explicit BitWriter(std::vector<u8>& out) : m_out(out) {}

    void bits(u32 value, u32 count) {
        m_acc |= static_cast<u64>(value) << m_count;
        m_count += count;
        while (m_count >= 8u) {
            m_out.push_back(static_cast<u8>(m_acc));
            m_acc >>= 8;
            m_count -= 8u;
        }
    }
    /// Huffman codes are stored most-significant bit first.
    void code(u32 code, u32 length) {
        u32 reversed = 0;
        for (u32 i = 0; i < length; ++i) {
            reversed = (reversed << 1) | ((code >> i) & 1u);
        }
        bits(reversed, length);
    }
    void flush() {
        if (m_count > 0u) {
            m_out.push_back(static_cast<u8>(m_acc));
        }
        m_acc = 0;
        m_count = 0;
    }

private:
    std::vector<u8>& m_out;
    u64 m_acc = 0;
    u32 m_count = 0;
};

void fixedLiteral(BitWriter& w, u32 symbol) {
    if (symbol < 144u) {
        w.code(0x30u + symbol, 8);
    } else if (symbol < 256u) {
        w.code(0x190u + (symbol - 144u), 9);
    } else if (symbol < 280u) {
        w.code(symbol - 256u, 7);
    } else {
        w.code(0xC0u + (symbol - 280u), 8);
    }
}

void fixedMatch(BitWriter& w, u32 length, u32 distance) {
    u32 li = 28;
    while (kLengthBase[li] > length) {
        --li;
    }
    fixedLiteral(w, 257u + li);
    if (kLengthExtra[li] != 0u) {
        w.bits(length - kLengthBase[li], kLengthExtra[li]);
    }
    u32 di = 29;
    while (kDistBase[di] > distance) {
        --di;
    }
    w.code(di, 5);
    if (kDistExtra[di] != 0u) {
        w.bits(distance - kDistBase[di], kDistExtra[di]);
    }
}

void zlibCompress(const std::vector<u8>& in, std::vector<u8>& out) {
    out.push_back(0x78);
    out.push_back(0x01);
    BitWriter w(out);
    w.bits(1u, 1u); // BFINAL
    w.bits(1u, 2u); // BTYPE = fixed Huffman

    constexpr u32 kWindow = 32768u;
    constexpr u32 kHashSize = 1u << 15;
    constexpr u32 kMaxChain = 64u;
    constexpr u32 kMaxMatch = 258u;
    std::vector<s32> head(kHashSize, -1);
    std::vector<s32> prev(in.size(), -1);
    const usize n = in.size();
    auto hashAt = [&in](usize i) {
        return ((static_cast<u32>(in[i]) << 10) ^ (static_cast<u32>(in[i + 1]) << 5) ^ in[i + 2]) & (kHashSize - 1u);
    };
    auto insert = [&](usize i) {
        if (i + 2u < n) {
            const u32 h = hashAt(i);
            prev[i] = head[h];
            head[h] = static_cast<s32>(i);
        }
    };

    usize i = 0;
    while (i < n) {
        u32 bestLen = 0;
        u32 bestDist = 0;
        if (i + 2u < n) {
            s32 candidate = head[hashAt(i)];
            u32 chain = 0;
            const u32 maxLen = static_cast<u32>(std::min<usize>(kMaxMatch, n - i));
            while (candidate >= 0 && chain < kMaxChain && i - static_cast<usize>(candidate) <= kWindow - 1u) {
                const usize c = static_cast<usize>(candidate);
                u32 len = 0;
                while (len < maxLen && in[c + len] == in[i + len]) {
                    ++len;
                }
                if (len > bestLen) {
                    bestLen = len;
                    bestDist = static_cast<u32>(i - c);
                    if (len == maxLen) {
                        break;
                    }
                }
                candidate = prev[c];
                ++chain;
            }
        }
        if (bestLen >= 3u) {
            fixedMatch(w, bestLen, bestDist);
            for (u32 k = 0; k < bestLen; ++k) {
                insert(i + k);
            }
            i += bestLen;
        } else {
            fixedLiteral(w, in[i]);
            insert(i);
            ++i;
        }
    }
    fixedLiteral(w, 256u);
    w.flush();
    putBe32(out, adler32(in.data(), in.size()));
}

// ---- inflate -----------------------------------------------------------------------------------

struct Huffman {
    u16 counts[16]{};
    u16 symbols[320]{};
};

class Inflater {
public:
    Inflater(const u8* data, usize size, std::vector<u8>& out) : m_in(data), m_size(size), m_out(out) {}

    bool run(std::string& error) {
        u32 last = 0;
        do {
            last = bits(1);
            const u32 type = bits(2);
            bool ok = false;
            if (type == 0u) {
                ok = stored();
            } else if (type == 1u) {
                ok = fixed();
            } else if (type == 2u) {
                ok = dynamic();
            }
            if (!ok || m_overrun) {
                error = m_overrun ? "deflate stream truncated" : "invalid deflate block";
                return false;
            }
        } while (last == 0u);
        return true;
    }

private:
    u32 bits(u32 need) {
        u32 value = m_bitBuf;
        while (m_bitCount < need) {
            if (m_pos >= m_size) {
                m_overrun = true;
                return 0;
            }
            value |= static_cast<u32>(m_in[m_pos++]) << m_bitCount;
            m_bitCount += 8u;
        }
        m_bitBuf = need >= 32u ? 0u : value >> need;
        m_bitCount -= need;
        return need >= 32u ? value : value & ((1u << need) - 1u);
    }

    bool stored() {
        m_bitBuf = 0;
        m_bitCount = 0;
        if (m_pos + 4u > m_size) {
            m_overrun = true;
            return false;
        }
        const u32 len = m_in[m_pos] | (static_cast<u32>(m_in[m_pos + 1]) << 8);
        const u32 nlen = m_in[m_pos + 2] | (static_cast<u32>(m_in[m_pos + 3]) << 8);
        m_pos += 4u;
        if (len != (~nlen & 0xFFFFu) || m_pos + len > m_size) {
            return false;
        }
        m_out.insert(m_out.end(), m_in + m_pos, m_in + m_pos + len);
        m_pos += len;
        return true;
    }

    static bool build(Huffman& h, const u8* lengths, u32 n) {
        std::memset(h.counts, 0, sizeof(h.counts));
        for (u32 s = 0; s < n; ++s) {
            ++h.counts[lengths[s]];
        }
        if (h.counts[0] == n) {
            return true; // empty code (distance-free block)
        }
        s32 left = 1;
        for (u32 len = 1; len < 16u; ++len) {
            left <<= 1;
            left -= h.counts[len];
            if (left < 0) {
                return false; // over-subscribed
            }
        }
        u16 offs[16]{};
        for (u32 len = 1; len < 15u; ++len) {
            offs[len + 1] = static_cast<u16>(offs[len] + h.counts[len]);
        }
        for (u32 s = 0; s < n; ++s) {
            if (lengths[s] != 0u) {
                h.symbols[offs[lengths[s]]++] = static_cast<u16>(s);
            }
        }
        return true;
    }

    s32 decode(const Huffman& h) {
        s32 code = 0;
        s32 first = 0;
        s32 index = 0;
        for (u32 len = 1; len < 16u; ++len) {
            code |= static_cast<s32>(bits(1));
            if (m_overrun) {
                return -1;
            }
            const s32 count = h.counts[len];
            if (code - count < first) {
                return h.symbols[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    bool codes(const Huffman& lit, const Huffman& dist) {
        for (;;) {
            const s32 symbol = decode(lit);
            if (symbol < 0) {
                return false;
            }
            if (symbol < 256) {
                m_out.push_back(static_cast<u8>(symbol));
            } else if (symbol == 256) {
                return true;
            } else {
                const u32 li = static_cast<u32>(symbol - 257);
                if (li >= 29u) {
                    return false;
                }
                const u32 len = kLengthBase[li] + bits(kLengthExtra[li]);
                const s32 ds = decode(dist);
                if (ds < 0 || ds >= 30) {
                    return false;
                }
                const u32 d = kDistBase[ds] + bits(kDistExtra[ds]);
                if (d > m_out.size()) {
                    return false;
                }
                const usize from = m_out.size() - d;
                for (u32 k = 0; k < len; ++k) {
                    m_out.push_back(m_out[from + k]);
                }
            }
        }
    }

    bool fixed() {
        u8 lengths[288];
        u32 s = 0;
        for (; s < 144u; ++s) lengths[s] = 8;
        for (; s < 256u; ++s) lengths[s] = 9;
        for (; s < 280u; ++s) lengths[s] = 7;
        for (; s < 288u; ++s) lengths[s] = 8;
        Huffman lit;
        build(lit, lengths, 288);
        u8 dl[30];
        std::memset(dl, 5, sizeof(dl));
        Huffman dist;
        build(dist, dl, 30);
        return codes(lit, dist);
    }

    bool dynamic() {
        static constexpr u8 kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const u32 nlen = bits(5) + 257u;
        const u32 ndist = bits(5) + 1u;
        const u32 ncode = bits(4) + 4u;
        if (nlen > 286u || ndist > 30u) {
            return false;
        }
        u8 lengths[320]{};
        for (u32 i = 0; i < ncode; ++i) {
            lengths[kOrder[i]] = static_cast<u8>(bits(3));
        }
        Huffman lencode;
        if (!build(lencode, lengths, 19)) {
            return false;
        }
        std::memset(lengths, 0, sizeof(lengths));
        u32 index = 0;
        while (index < nlen + ndist) {
            s32 symbol = decode(lencode);
            if (symbol < 0) {
                return false;
            }
            if (symbol < 16) {
                lengths[index++] = static_cast<u8>(symbol);
            } else {
                u8 value = 0;
                u32 repeat = 0;
                if (symbol == 16) {
                    if (index == 0u) {
                        return false;
                    }
                    value = lengths[index - 1u];
                    repeat = 3u + bits(2);
                } else if (symbol == 17) {
                    repeat = 3u + bits(3);
                } else {
                    repeat = 11u + bits(7);
                }
                if (index + repeat > nlen + ndist) {
                    return false;
                }
                while (repeat-- > 0u) {
                    lengths[index++] = value;
                }
            }
        }
        Huffman lit;
        Huffman dist;
        if (!build(lit, lengths, nlen) || !build(dist, lengths + nlen, ndist)) {
            return false;
        }
        return codes(lit, dist);
    }

    const u8* m_in;
    usize m_size;
    usize m_pos = 0;
    u32 m_bitBuf = 0;
    u32 m_bitCount = 0;
    bool m_overrun = false;
    std::vector<u8>& m_out;
};

bool zlibDecompress(const std::vector<u8>& in, std::vector<u8>& out, std::string& error) {
    if (in.size() < 6u || (in[0] & 0x0Fu) != 8u || ((static_cast<u32>(in[0]) << 8) | in[1]) % 31u != 0u ||
        (in[1] & 0x20u) != 0u) {
        error = "invalid zlib header";
        return false;
    }
    Inflater inflater(in.data() + 2, in.size() - 2u, out);
    if (!inflater.run(error)) {
        return false;
    }
    // The Adler-32 trailer is checked when present (some writers pad IDAT after it).
    return true;
}

// ---- PNG filters -------------------------------------------------------------------------------

u8 paeth(u8 a, u8 b, u8 c) {
    const int p = static_cast<int>(a) + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

void appendChunk(std::vector<u8>& out, const char type[4], const std::vector<u8>& data) {
    putBe32(out, static_cast<u32>(data.size()));
    const usize start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    putBe32(out, crc32(out.data() + start, out.size() - start));
}

u32 floatBits(f32 v) {
    u32 b = 0;
    std::memcpy(&b, &v, 4);
    return b;
}

f32 halfToFloat(u16 h) {
    const u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    u32 exponent = (h >> 10) & 0x1Fu;
    u32 mantissa = h & 0x3FFu;
    u32 bits = 0;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;
        } else {
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x400u) == 0u) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
    }
    f32 out = 0.f;
    std::memcpy(&out, &bits, 4);
    return out;
}

void putLe32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

void putLe64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

u32 getLe32(const u8* p) {
    return p[0] | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

u64 getLe64(const u8* p) {
    return static_cast<u64>(getLe32(p)) | (static_cast<u64>(getLe32(p + 4)) << 32);
}

void putAttribute(std::vector<u8>& out, const char* name, const char* type, const std::vector<u8>& value) {
    out.insert(out.end(), name, name + std::strlen(name) + 1u);
    out.insert(out.end(), type, type + std::strlen(type) + 1u);
    putLe32(out, static_cast<u32>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

} // namespace

u32 crc32(const u8* data, usize size, u32 crc) {
    const auto& table = crcTable();
    crc = ~crc;
    for (usize i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

bool encodePng(const ImageRgba8& image, std::vector<u8>& out) {
    if (!image.valid()) {
        return false;
    }
    bool opaque = true;
    for (usize i = 3; i < image.pixels.size(); i += 4u) {
        opaque = opaque && image.pixels[i] == 255u;
    }
    const u32 channels = opaque ? 3u : 4u;
    const usize stride = static_cast<usize>(image.width) * channels;

    std::vector<u8> raw(stride);
    std::vector<u8> prior(stride, 0u);
    std::vector<u8> filtered;
    filtered.reserve((stride + 1u) * image.height);
    std::vector<u8> candidate(stride);
    std::vector<u8> best(stride);
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            const u8* p = image.at(x, y);
            std::memcpy(raw.data() + static_cast<usize>(x) * channels, p, channels);
        }
        u64 bestScore = ~u64{0};
        u8 bestFilter = 0;
        for (u8 f = 0; f < 5u; ++f) {
            u64 score = 0;
            for (usize i = 0; i < stride; ++i) {
                const u8 a = i >= channels ? raw[i - channels] : 0u;
                const u8 b = prior[i];
                const u8 c = i >= channels ? prior[i - channels] : 0u;
                u8 predictor = 0;
                switch (f) {
                case 1: predictor = a; break;
                case 2: predictor = b; break;
                case 3: predictor = static_cast<u8>((static_cast<u32>(a) + b) / 2u); break;
                case 4: predictor = paeth(a, b, c); break;
                default: break;
                }
                candidate[i] = static_cast<u8>(raw[i] - predictor);
                score += static_cast<u64>(std::abs(static_cast<int>(static_cast<std::int8_t>(candidate[i]))));
            }
            if (score < bestScore) {
                bestScore = score;
                bestFilter = f;
                best.swap(candidate);
            }
        }
        filtered.push_back(bestFilter);
        filtered.insert(filtered.end(), best.begin(), best.end());
        prior.swap(raw);
    }

    out.clear();
    static constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), kSignature, kSignature + 8);
    std::vector<u8> ihdr;
    putBe32(ihdr, image.width);
    putBe32(ihdr, image.height);
    ihdr.push_back(8);                        // bit depth
    ihdr.push_back(opaque ? 2u : 6u);         // colour type RGB / RGBA
    ihdr.push_back(0);                        // compression
    ihdr.push_back(0);                        // filter method
    ihdr.push_back(0);                        // interlace
    appendChunk(out, "IHDR", ihdr);
    std::vector<u8> idat;
    zlibCompress(filtered, idat);
    appendChunk(out, "IDAT", idat);
    appendChunk(out, "IEND", {});
    return true;
}

bool decodePng(const u8* data, usize size, ImageRgba8& out, std::string* error) {
    std::string localError;
    std::string& err = error != nullptr ? *error : localError;
    static constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < 8u || std::memcmp(data, kSignature, 8) != 0) {
        err = "not a PNG file";
        return false;
    }
    usize pos = 8;
    u32 width = 0;
    u32 height = 0;
    u8 colorType = 0;
    std::vector<u8> idat;
    bool sawIhdr = false;
    bool sawEnd = false;
    while (pos + 12u <= size && !sawEnd) {
        const u32 length = getBe32(data + pos);
        if (pos + 12u + length > size) {
            err = "truncated chunk";
            return false;
        }
        const u8* type = data + pos + 4;
        const u8* body = data + pos + 8;
        if (crc32(type, length + 4u) != getBe32(body + length)) {
            err = "chunk CRC mismatch";
            return false;
        }
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (length != 13u) {
                err = "bad IHDR";
                return false;
            }
            width = getBe32(body);
            height = getBe32(body + 4);
            colorType = body[9];
            if (body[8] != 8u || (colorType != 0u && colorType != 2u && colorType != 4u && colorType != 6u) ||
                body[12] != 0u) {
                err = "unsupported PNG (need 8-bit grey/RGB/RGBA, non-interlaced)";
                return false;
            }
            sawIhdr = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            sawEnd = true;
        } else if ((type[0] & 0x20u) == 0u) {
            err = "unsupported critical chunk";
            return false;
        }
        pos += 12u + length;
    }
    if (!sawIhdr || width == 0u || height == 0u || width > 16384u || height > 16384u) {
        err = "missing or invalid IHDR";
        return false;
    }
    std::vector<u8> filtered;
    if (!zlibDecompress(idat, filtered, err)) {
        return false;
    }
    const u32 channels = colorType == 0u ? 1u : colorType == 4u ? 2u : colorType == 2u ? 3u : 4u;
    const usize stride = static_cast<usize>(width) * channels;
    if (filtered.size() < (stride + 1u) * height) {
        err = "image data too short";
        return false;
    }
    std::vector<u8> prior(stride, 0u);
    std::vector<u8> row(stride);
    out = ImageRgba8(width, height);
    for (u32 y = 0; y < height; ++y) {
        const u8* src = filtered.data() + static_cast<usize>(y) * (stride + 1u);
        const u8 filter = src[0];
        ++src;
        for (usize i = 0; i < stride; ++i) {
            const u8 a = i >= channels ? row[i - channels] : 0u;
            const u8 b = prior[i];
            const u8 c = i >= channels ? prior[i - channels] : 0u;
            u8 predictor = 0;
            switch (filter) {
            case 0: break;
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = static_cast<u8>((static_cast<u32>(a) + b) / 2u); break;
            case 4: predictor = paeth(a, b, c); break;
            default: err = "invalid filter type"; return false;
            }
            row[i] = static_cast<u8>(src[i] + predictor);
        }
        for (u32 x = 0; x < width; ++x) {
            const u8* s = row.data() + static_cast<usize>(x) * channels;
            u8* d = out.at(x, y);
            switch (channels) {
            case 1: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
            case 2: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            case 3: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
            default: std::memcpy(d, s, 4); break;
            }
        }
        prior.swap(row);
    }
    return true;
}

bool readFileBytes(const std::string& path, std::vector<u8>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool writeFileBytes(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

bool ensureDirectory(const std::string& path) {
    if (path.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return std::filesystem::is_directory(path, ec);
}

bool writePng(const std::string& path, const ImageRgba8& image) {
    std::vector<u8> bytes;
    return encodePng(image, bytes) && writeFileBytes(path, bytes);
}

bool readPng(const std::string& path, ImageRgba8& out, std::string* error) {
    std::vector<u8> bytes;
    if (!readFileBytes(path, bytes)) {
        if (error != nullptr) {
            *error = "cannot read " + path;
        }
        return false;
    }
    return decodePng(bytes.data(), bytes.size(), out, error);
}

bool writeExr(const std::string& path, u32 width, u32 height, std::vector<ExrChannel> channels) {
    const usize pixels = static_cast<usize>(width) * height;
    if (width == 0u || height == 0u || channels.empty()) {
        return false;
    }
    for (const ExrChannel& c : channels) {
        if (c.name.empty() || c.data.size() != pixels) {
            return false;
        }
    }
    std::sort(channels.begin(), channels.end(), [](const ExrChannel& a, const ExrChannel& b) { return a.name < b.name; });

    std::vector<u8> out = {0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0};
    std::vector<u8> chlist;
    for (const ExrChannel& c : channels) {
        chlist.insert(chlist.end(), c.name.begin(), c.name.end());
        chlist.push_back(0);
        putLe32(chlist, 2u);                         // FLOAT
        chlist.insert(chlist.end(), {0, 0, 0, 0});   // pLinear + reserved
        putLe32(chlist, 1u);                         // xSampling
        putLe32(chlist, 1u);                         // ySampling
    }
    chlist.push_back(0);
    putAttribute(out, "channels", "chlist", chlist);
    putAttribute(out, "compression", "compression", {0});
    std::vector<u8> box;
    putLe32(box, 0u);
    putLe32(box, 0u);
    putLe32(box, width - 1u);
    putLe32(box, height - 1u);
    putAttribute(out, "dataWindow", "box2i", box);
    putAttribute(out, "displayWindow", "box2i", box);
    putAttribute(out, "lineOrder", "lineOrder", {0});
    std::vector<u8> one;
    putLe32(one, floatBits(1.f));
    putAttribute(out, "pixelAspectRatio", "float", one);
    std::vector<u8> center;
    putLe32(center, floatBits(0.f));
    putLe32(center, floatBits(0.f));
    putAttribute(out, "screenWindowCenter", "v2f", center);
    putAttribute(out, "screenWindowWidth", "float", one);
    out.push_back(0); // end of header

    const u32 lineBytes = width * static_cast<u32>(channels.size()) * 4u;
    const u64 tableStart = out.size();
    const u64 firstChunk = tableStart + static_cast<u64>(height) * 8u;
    for (u32 y = 0; y < height; ++y) {
        putLe64(out, firstChunk + static_cast<u64>(y) * (8u + lineBytes));
    }
    for (u32 y = 0; y < height; ++y) {
        putLe32(out, y);
        putLe32(out, lineBytes);
        for (const ExrChannel& c : channels) {
            for (u32 x = 0; x < width; ++x) {
                putLe32(out, floatBits(c.data[static_cast<usize>(y) * width + x]));
            }
        }
    }
    return writeFileBytes(path, out);
}

bool readExr(const std::string& path, u32& width, u32& height, std::vector<ExrChannel>& channels,
             std::string* error) {
    std::string localError;
    std::string& err = error != nullptr ? *error : localError;
    std::vector<u8> bytes;
    if (!readFileBytes(path, bytes)) {
        err = "cannot read " + path;
        return false;
    }
    if (bytes.size() < 8u || getLe32(bytes.data()) != 20000630u || bytes[4] != 2u || (bytes[5] & 0x06u) != 0u) {
        err = "not a single-part scanline OpenEXR file";
        return false;
    }
    usize pos = 8;
    auto readString = [&](std::string& s) {
        s.clear();
        while (pos < bytes.size() && bytes[pos] != 0u) {
            s.push_back(static_cast<char>(bytes[pos++]));
        }
        return pos++ < bytes.size();
    };
    std::vector<u32> types;
    channels.clear();
    s32 box[4] = {0, 0, -1, -1};
    int compression = -1;
    for (;;) {
        std::string name;
        if (!readString(name)) {
            err = "truncated header";
            return false;
        }
        if (name.empty()) {
            break;
        }
        std::string type;
        if (!readString(type) || pos + 4u > bytes.size()) {
            err = "truncated attribute";
            return false;
        }
        const u32 size = getLe32(bytes.data() + pos);
        pos += 4u;
        if (pos + size > bytes.size()) {
            err = "truncated attribute value";
            return false;
        }
        const u8* value = bytes.data() + pos;
        if (name == "channels") {
            usize p = 0;
            while (p < size && value[p] != 0u) {
                ExrChannel c;
                while (p < size && value[p] != 0u) {
                    c.name.push_back(static_cast<char>(value[p++]));
                }
                ++p;
                if (p + 16u > size) {
                    err = "bad chlist";
                    return false;
                }
                types.push_back(getLe32(value + p));
                if (getLe32(value + p + 8) != 1u || getLe32(value + p + 12) != 1u) {
                    err = "subsampled channels unsupported";
                    return false;
                }
                p += 16u;
                channels.push_back(std::move(c));
            }
        } else if (name == "compression" && size == 1u) {
            compression = value[0];
        } else if (name == "dataWindow" && size == 16u) {
            for (int i = 0; i < 4; ++i) {
                box[i] = static_cast<s32>(getLe32(value + 4 * i));
            }
        }
        pos += size;
    }
    if (compression != 0 || channels.empty() || box[2] < box[0] || box[3] < box[1]) {
        err = "only uncompressed EXR with a valid dataWindow is supported";
        return false;
    }
    width = static_cast<u32>(box[2] - box[0] + 1);
    height = static_cast<u32>(box[3] - box[1] + 1);
    for (usize c = 0; c < channels.size(); ++c) {
        if (types[c] != 1u && types[c] != 2u) {
            err = "only HALF / FLOAT channels are supported";
            return false;
        }
        channels[c].data.assign(static_cast<usize>(width) * height, 0.f);
    }
    for (u32 line = 0; line < height; ++line) {
        if (pos + 8u > bytes.size()) {
            err = "truncated offset table";
            return false;
        }
        usize chunk = static_cast<usize>(getLe64(bytes.data() + pos));
        pos += 8u;
        if (chunk + 8u > bytes.size()) {
            err = "bad chunk offset";
            return false;
        }
        const s32 y = static_cast<s32>(getLe32(bytes.data() + chunk)) - box[1];
        chunk += 8u;
        if (y < 0 || static_cast<u32>(y) >= height) {
            err = "bad scanline index";
            return false;
        }
        for (usize c = 0; c < channels.size(); ++c) {
            const usize bytesPer = types[c] == 1u ? 2u : 4u;
            if (chunk + bytesPer * width > bytes.size()) {
                err = "truncated scanline";
                return false;
            }
            for (u32 x = 0; x < width; ++x) {
                const u8* p = bytes.data() + chunk + bytesPer * x;
                f32 v = 0.f;
                if (bytesPer == 2u) {
                    v = halfToFloat(static_cast<u16>(p[0] | (p[1] << 8)));
                } else {
                    const u32 b = getLe32(p);
                    std::memcpy(&v, &b, 4);
                }
                channels[c].data[static_cast<usize>(y) * width + x] = v;
            }
            chunk += bytesPer * width;
        }
    }
    return true;
}

} // namespace fuse::renderer::harness
