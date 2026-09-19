#include <fuse/cook/bc7_encoder.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace fuse::cook {

namespace {

u64 fnv1a64(const u8* data, std::size_t size) {
    u64 hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

u8 quantize7(u8 value) {
    return static_cast<u8>((static_cast<u32>(value) * 127u + 127u) / 255u);
}

u8 expand7(u8 value) {
    return static_cast<u8>((static_cast<u32>(value) * 255u + 63u) / 127u);
}

void write_bits(u8* block, u32 bitOffset, u32 bitCount, u64 value) {
    for (u32 bit = 0; bit < bitCount; ++bit) {
        const u32 absolute = bitOffset + bit;
        const u32 byteIndex = absolute / 8u;
        const u32 bitIndex = absolute % 8u;
        if ((value >> bit) & 1ull) {
            block[byteIndex] |= static_cast<u8>(1u << bitIndex);
        } else {
            block[byteIndex] &= static_cast<u8>(~(1u << bitIndex));
        }
    }
}

u64 read_bits(const u8* block, u32 bitOffset, u32 bitCount) {
    u64 value = 0;
    for (u32 bit = 0; bit < bitCount; ++bit) {
        const u32 absolute = bitOffset + bit;
        const u32 byteIndex = absolute / 8u;
        const u32 bitIndex = absolute % 8u;
        if ((block[byteIndex] >> bitIndex) & 1u) {
            value |= (1ull << bit);
        }
    }
    return value;
}

void average_rgba4x4(const u8* rgba, u32 width, u32 height, u32 blockX, u32 blockY, u8& r, u8& g,
                     u8& b, u8& a) {
    u32 sumR = 0;
    u32 sumG = 0;
    u32 sumB = 0;
    u32 sumA = 0;
    u32 count = 0;

    for (u32 y = 0; y < 4u; ++y) {
        for (u32 x = 0; x < 4u; ++x) {
            const u32 px = blockX * 4u + x;
            const u32 py = blockY * 4u + y;
            const u32 sx = px < width ? px : (width > 0 ? width - 1u : 0u);
            const u32 sy = py < height ? py : (height > 0 ? height - 1u : 0u);
            const u32 index = (sy * width + sx) * 4u;
            sumR += rgba[index + 0];
            sumG += rgba[index + 1];
            sumB += rgba[index + 2];
            sumA += rgba[index + 3];
            ++count;
        }
    }

    r = static_cast<u8>(sumR / count);
    g = static_cast<u8>(sumG / count);
    b = static_cast<u8>(sumB / count);
    a = static_cast<u8>(sumA / count);
}

} // namespace

u32 bc7_padded_dimension(u32 value) {
    if (value == 0) {
        return 4u;
    }
    return ((value + 3u) / 4u) * 4u;
}

void bc7_encode_solid_block(u8 block[16], u8 r, u8 g, u8 b, u8 a) {
    for (u32 i = 0; i < 16u; ++i) {
        block[i] = 0;
    }

    // Mode 6 marker: bit 6 set, bits 0-5 clear.
    write_bits(block, 6, 1, 1);

    const u8 r0 = quantize7(r);
    const u8 g0 = quantize7(g);
    const u8 b0 = quantize7(b);
    const u8 a0 = quantize7(a);
    const u8 r1 = r0;
    const u8 g1 = g0;
    const u8 b1 = b0;
    const u8 a1 = a0;

    u32 bit = 7;
    write_bits(block, bit, 7, r0);
    bit += 7;
    write_bits(block, bit, 7, r1);
    bit += 7;
    write_bits(block, bit, 7, g0);
    bit += 7;
    write_bits(block, bit, 7, g1);
    bit += 7;
    write_bits(block, bit, 7, b0);
    bit += 7;
    write_bits(block, bit, 7, b1);
    bit += 7;
    write_bits(block, bit, 7, a0);
    bit += 7;
    write_bits(block, bit, 7, a1);
    bit += 7;

    for (u32 index = 0; index < 16u; ++index) {
        write_bits(block, bit, 2, 0);
        bit += 2;
    }
}

bool bc7_decode_solid_block(const u8 block[16], u8& r, u8& g, u8& b, u8& a) {
    if ((block[0] & 0x7Fu) != 0x40u) {
        return false;
    }

    u32 bit = 7;
    const u8 r0 = static_cast<u8>(read_bits(block, bit, 7));
    bit += 7;
    (void)read_bits(block, bit, 7);
    bit += 7;
    const u8 g0 = static_cast<u8>(read_bits(block, bit, 7));
    bit += 7;
    (void)read_bits(block, bit, 7);
    bit += 7;
    const u8 b0 = static_cast<u8>(read_bits(block, bit, 7));
    bit += 7;
    (void)read_bits(block, bit, 7);
    bit += 7;
    const u8 a0 = static_cast<u8>(read_bits(block, bit, 7));

    r = expand7(r0);
    g = expand7(g0);
    b = expand7(b0);
    a = expand7(a0);
    return true;
}

Bc7RgbaImage synthesize_rgba_from_source(const std::string& source_path) {
    Bc7RgbaImage image;

    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        return image;
    }

    std::vector<u8> bytes;
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size > 0) {
        bytes.resize(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), size);
    }

    const u64 hash = fnv1a64(bytes.data(), bytes.size());
    image.width = bc7_padded_dimension(static_cast<u32>(32u + (hash & 0x3Fu)));
    image.height = bc7_padded_dimension(static_cast<u32>(32u + ((hash >> 8) & 0x3Fu)));
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4u);

    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            const u32 index = (y * image.width + x) * 4u;
            image.rgba[index + 0] = static_cast<u8>((hash + x * 17u + y * 3u) & 0xFFu);
            image.rgba[index + 1] = static_cast<u8>((hash >> 16) + x) & 0xFFu;
            image.rgba[index + 2] = static_cast<u8>((hash >> 32) + y) & 0xFFu;
            image.rgba[index + 3] = 255u;
        }
    }

    return image;
}

Bc7EncodeResult encode_bc7_rgba8(const u8* rgba, u32 width, u32 height, std::vector<u8>& outBlocks) {
    Bc7EncodeResult result;
    if (rgba == nullptr || width == 0 || height == 0) {
        result.note = "invalid rgba input";
        return result;
    }

    const u32 paddedWidth = bc7_padded_dimension(width);
    const u32 paddedHeight = bc7_padded_dimension(height);
    const u32 blocksX = paddedWidth / 4u;
    const u32 blocksY = paddedHeight / 4u;
    result.width = paddedWidth;
    result.height = paddedHeight;
    result.blockCount = blocksX * blocksY;
    result.byteCount = result.blockCount * 16u;

    outBlocks.resize(static_cast<std::size_t>(result.blockCount) * 16u);
    for (u32 by = 0; by < blocksY; ++by) {
        for (u32 bx = 0; bx < blocksX; ++bx) {
            u8 r = 0;
            u8 g = 0;
            u8 b = 0;
            u8 a = 255;
            average_rgba4x4(rgba, width, height, bx, by, r, g, b, a);

            u8 block[16];
            bc7_encode_solid_block(block, r, g, b, a);
            const std::size_t offset = static_cast<std::size_t>((by * blocksX + bx) * 16u);
            for (u32 i = 0; i < 16u; ++i) {
                outBlocks[offset + i] = block[i];
            }
        }
    }

    result.ok = true;
    result.note = "bc7 mode-6 block encoding (honest stub)";
    return result;
}

} // namespace fuse::cook
