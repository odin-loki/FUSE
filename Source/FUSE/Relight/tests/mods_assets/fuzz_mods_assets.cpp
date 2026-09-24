// FUSE Relight RL-3.3: malformed-file fuzz of the three readers (ctest rl_mods_assets_fuzz and, natively,
// rl_mods_assets_fuzz_asan under AddressSanitizer + UBSan).
//
// Deterministic (fixed seeds): for each reader, 1000 cases (override with argv[1]) derived from valid seed
// files by bit flips, byte stores of boundary values, 32-bit header-field overwrites with hostile values,
// truncation, extension and splices. Every case goes through the reader and, when it is accepted, through
// everything a consumer would do next (decode every subresource of a DDS file; read, decode and load every
// blob, image and buffer of a package; decode a GDeflate stream into a buffer sized by its own header).
// Pass = no crash, no sanitizer report, no exception; the accept/reject counts are printed.
#include "bc_oracle.hpp"

#include <fuse/relight/mods/assets/asset_package.hpp>
#include <fuse/relight/mods/assets/dds.hpp>
#include <fuse/relight/mods/assets/gdeflate.hpp>
#include <fuse/relight/mods/assets/texture_decode.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace ma = fuse::relight::mods::assets;
namespace gd = fuse::relight::mods::assets::gdeflate;
using Bytes = std::vector<std::uint8_t>;
using ma::TexFormat;

struct Rng {
    rl_bc_oracle::Lcg g;
    std::uint32_t next() { return g.next(); }
    std::uint32_t below(std::uint32_t n) { return n ? next() % n : 0; }
};

Bytes randomBytes(std::size_t n, std::uint64_t seed) {
    rl_bc_oracle::Lcg g{seed};
    Bytes b(n);
    for (auto& x : b) {
        x = static_cast<std::uint8_t>(g.next() >> 11);
    }
    return b;
}

ma::TextureImage makeImage(TexFormat f, ma::TexDimension dim, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint32_t mips,
                           std::uint32_t layers, std::uint64_t seed) {
    ma::TextureImage img;
    img.format = f;
    img.dimension = dim;
    img.width = w;
    img.height = h;
    img.depth = d;
    img.mipLevels = mips;
    img.arraySize = layers;
    img.faces = dim == ma::TexDimension::Cube ? 6 : 1;
    img.data = randomBytes(static_cast<std::size_t>(ma::layoutSubresources(img)), seed);
    return img;
}

/// A DX9 DDS header + payload (bits: 32-bit A8R8G8B8, or a FourCC).
Bytes legacyDds(std::uint32_t fourCC, std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t caps2, const Bytes& payload) {
    Bytes o;
    auto put32 = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            o.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    };
    put32(0x20534444);
    put32(124);
    put32(0x21007);
    put32(h);
    put32(w);
    put32(0);
    put32(0);
    put32(mips);
    for (int k = 0; k < 11; ++k) {
        put32(0);
    }
    put32(32);
    put32(fourCC ? 0x4 : 0x41);
    put32(fourCC);
    put32(fourCC ? 0 : 32);
    put32(fourCC ? 0 : 0xff0000);
    put32(fourCC ? 0 : 0xff00);
    put32(fourCC ? 0 : 0xff);
    put32(fourCC ? 0 : 0xff000000);
    put32(0x1000);
    put32(caps2);
    put32(0);
    put32(0);
    put32(0);
    o.insert(o.end(), payload.begin(), payload.end());
    return o;
}

/// Offsets worth hitting with whole-field overwrites (header fields of each format).
Bytes mutate(const Bytes& seed, Rng& r, const std::vector<std::size_t>& fields) {
    Bytes b = seed;
    static const std::uint32_t kHostile[] = {0,          1,          2,          3,          4,          6,          7,
                                             0xff,       0x100,      0xffff,     0x10000,    0x7fffffff, 0x80000000, 0xfffffffe,
                                             0xffffffff, 0x20534444, 0xbaadd00d, 0x30315844, 0x31545844, 0xfb04,     16384};
    const std::uint32_t ops = 1 + r.below(4);
    for (std::uint32_t op = 0; op < ops && !b.empty(); ++op) {
        switch (r.below(8)) {
        case 0: // bit flips
        case 1:
            for (std::uint32_t k = 1 + r.below(8); k > 0; --k) {
                b[r.below(static_cast<std::uint32_t>(b.size()))] ^= static_cast<std::uint8_t>(1u << r.below(8));
            }
            break;
        case 2: // boundary byte
            b[r.below(static_cast<std::uint32_t>(b.size()))] = static_cast<std::uint8_t>(kHostile[r.below(sizeof(kHostile) / 4)]);
            break;
        case 3:
        case 4: { // hostile 32-bit (or 16-bit) value into a header field
            if (fields.empty()) {
                break;
            }
            const std::size_t at = fields[r.below(static_cast<std::uint32_t>(fields.size()))];
            const std::uint32_t v = kHostile[r.below(sizeof(kHostile) / 4)] + (r.below(4) == 0 ? r.below(64) : 0);
            const int width = r.below(3) == 0 ? 2 : 4;
            for (int k = 0; k < width && at + static_cast<std::size_t>(k) < b.size(); ++k) {
                b[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v >> (8 * k));
            }
            break;
        }
        case 5: // truncate
            b.resize(r.below(static_cast<std::uint32_t>(b.size())));
            break;
        case 6: { // extend with junk
            const std::uint32_t n = 1 + r.below(256);
            for (std::uint32_t k = 0; k < n; ++k) {
                b.push_back(static_cast<std::uint8_t>(r.next()));
            }
            break;
        }
        case 7: { // splice a random window over another
            const std::uint32_t n = 1 + r.below(64);
            if (b.size() > n) {
                const std::uint32_t from = r.below(static_cast<std::uint32_t>(b.size() - n));
                const std::uint32_t to = r.below(static_cast<std::uint32_t>(b.size() - n));
                for (std::uint32_t k = 0; k < n; ++k) {
                    b[to + k] = b[from + k];
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return b;
}

struct Tally {
    int accepted = 0, rejected = 0, deep = 0;
};

volatile float g_sink = 0.0f; // keeps decode results observable

void consumeDds(const Bytes& file, Tally& t) {
    std::string err;
    auto tex = ma::readDds(file, &err);
    if (!tex) {
        ++t.rejected;
        return;
    }
    ++t.accepted;
    for (const ma::Subresource& s : tex->image.subresources) {
        if (std::uint64_t(s.width) * s.height * s.depth > (1u << 16)) {
            continue;
        }
        auto px = ma::decodeSubresource(tex->image, s);
        if (px && !px->empty()) {
            ma::applySwizzle(*px, tex->image.swizzle);
            g_sink = g_sink + (*px)[0];
            ++t.deep;
        }
    }
}

void consumePackage(const Bytes& file, Tally& t) {
    std::string err;
    auto p = ma::AssetPackage::fromBytes(file, &err);
    if (!p) {
        ++t.rejected;
        return;
    }
    ++t.accepted;
    for (std::uint32_t i = 0; i < p->blobCount() && i < 256; ++i) {
        auto b = p->readBlob(i, &err, 1u << 24);
        (void)p->blobCrcMatches(i);
        t.deep += b ? 1 : 0;
    }
    for (std::uint32_t i = 0; i < p->assetCount() && i < 64; ++i) {
        (void)p->assetName(i);
        (void)p->findAsset(p->assetName(i));
        const ma::PackageAssetDesc* a = p->asset(i);
        if (a->type == ma::PackageAssetType::Buffer) {
            t.deep += p->loadBuffer(i, &err) ? 1 : 0;
            continue;
        }
        (void)p->blobIndex(i, 0, 0, 0);
        auto img = p->loadImage(i, &err);
        if (img && !img->subresources.empty()) {
            const ma::Subresource& s = img->subresources.back();
            if (std::uint64_t(s.width) * s.height * s.depth <= (1u << 16)) {
                auto px = ma::decodeSubresource(*img, s);
                g_sink = g_sink + (px && !px->empty() ? (*px)[0] : 0.0f);
            }
            ++t.deep;
        }
    }
}

void consumeGdeflate(const Bytes& stream, Tally& t) {
    std::string err;
    auto info = gd::inspect(stream, &err);
    if (!info) {
        ++t.rejected;
        return;
    }
    ++t.accepted;
    if (info->uncompressedSize > (16u << 20)) {
        return;
    }
    Bytes out(static_cast<std::size_t>(info->uncompressedSize));
    t.deep += gd::decompress(stream, out, &err) == gd::Status::Ok ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    const int cases = argc > 1 ? std::atoi(argv[1]) : 1000;

    // ---- seeds ----
    std::vector<Bytes> ddsSeeds;
    ddsSeeds.push_back(ma::writeDds(makeImage(TexFormat::BC7_UNORM_BLOCK, ma::TexDimension::Tex2D, 16, 8, 1, 3, 1, 1)));
    ddsSeeds.push_back(ma::writeDds(makeImage(TexFormat::BC6H_SFLOAT_BLOCK, ma::TexDimension::Cube, 8, 8, 1, 2, 1, 2)));
    ddsSeeds.push_back(ma::writeDds(makeImage(TexFormat::R16G16B16A16_SFLOAT, ma::TexDimension::Tex3D, 4, 4, 4, 3, 1, 3)));
    ddsSeeds.push_back(ma::writeDds(makeImage(TexFormat::BC1_RGBA_UNORM_BLOCK, ma::TexDimension::Tex2D, 12, 12, 1, 4, 3, 4)));
    ddsSeeds.push_back(ma::writeDds(makeImage(TexFormat::R8_UNORM, ma::TexDimension::Tex1D, 17, 1, 1, 2, 2, 5)));
    ddsSeeds.push_back(legacyDds(0x35545844 /* DXT5 */, 8, 8, 4, 0, randomBytes(64 + 16 + 16 + 16, 6)));
    ddsSeeds.push_back(legacyDds(0, 4, 4, 1, 0x200 | 0xfc00, randomBytes(6 * 64, 7)));
    ddsSeeds.push_back(legacyDds(113 /* A16B16G16R16F */, 3, 3, 2, 0, randomBytes(9 * 8 + 8, 8)));
    const std::vector<std::size_t> ddsFields = {4,  8,  12, 16, 20, 24, 28, 80, 84, 88, 92,  96,  100, 104,
                                                108, 112, 116, 128, 132, 136, 140, 144, 148};

    std::vector<Bytes> pkgSeeds;
    for (std::uint32_t level : {0u, 6u}) {
        ma::AssetPackageWriter w;
        w.addImage("a.dds", makeImage(TexFormat::BC7_SRGB_BLOCK, ma::TexDimension::Tex2D, 32, 16, 1, 6, 1, 10), 3, level);
        w.addImage("cube.dds", makeImage(TexFormat::R8G8B8A8_UNORM, ma::TexDimension::Cube, 4, 4, 1, 3, 1, 11), 1, level);
        w.addImage("vol.dds", makeImage(TexFormat::BC4_UNORM_BLOCK, ma::TexDimension::Tex3D, 8, 8, 2, 2, 1, 12), 0, level);
        ma::PackageAssetDesc buf;
        buf.type = ma::PackageAssetType::Buffer;
        const Bytes data = randomBytes(3000, 13);
        buf.size = static_cast<std::uint32_t>(data.size());
        buf.baseBlobIdx = static_cast<std::uint16_t>(w.addBlob(data, level));
        w.addAsset("buf.bin", buf);
        pkgSeeds.push_back(w.finish());
    }
    std::vector<std::vector<std::size_t>> pkgFields;
    for (const Bytes& s : pkgSeeds) {
        std::vector<std::size_t> f = {0, 4, 8, 12};
        std::uint64_t dict = 0;
        for (int k = 0; k < 8; ++k) {
            dict |= std::uint64_t(s[8 + static_cast<std::size_t>(k)]) << (8 * k);
        }
        for (std::size_t at = static_cast<std::size_t>(dict); at + 4 <= s.size() && at < dict + 4 + 4 * 20 + 64 * 16; at += 2) {
            f.push_back(at);
        }
        f.push_back(16);
        f.push_back(24);
        pkgFields.push_back(f);
    }

    std::vector<Bytes> gdSeeds;
    gdSeeds.push_back(*gd::compress(randomBytes(70000, 20), 6));
    {
        std::string text;
        while (text.size() < 140000) {
            text += "material mat_1234 diffuse_texture textures/abc.dds emissive_mask_texture ";
        }
        gdSeeds.push_back(*gd::compress(Bytes(text.begin(), text.end()), 12));
    }
    gdSeeds.push_back(*gd::compress(Bytes(1000, 0), 1));
    std::vector<std::size_t> gdFields = {0, 2, 4, 6, 8, 12, 16, 20};

    Rng r{{0x5eed}};
    Tally dds, pkg, gdt;
    for (int i = 0; i < cases; ++i) {
        consumeDds(mutate(ddsSeeds[static_cast<std::size_t>(i) % ddsSeeds.size()], r, ddsFields), dds);
    }
    for (int i = 0; i < cases; ++i) {
        const std::size_t s = static_cast<std::size_t>(i) % pkgSeeds.size();
        consumePackage(mutate(pkgSeeds[s], r, pkgFields[s]), pkg);
    }
    for (int i = 0; i < cases; ++i) {
        consumeGdeflate(mutate(gdSeeds[static_cast<std::size_t>(i) % gdSeeds.size()], r, gdFields), gdt);
    }
    // Pure noise, too.
    Tally noise;
    for (int i = 0; i < 100; ++i) {
        const Bytes junk = randomBytes(1 + r.below(4096), 1000 + static_cast<std::uint64_t>(i));
        consumeDds(junk, noise);
        consumePackage(junk, noise);
        consumeGdeflate(junk, noise);
    }
    std::printf("rl_mods_assets_fuzz: %d cases per reader\n", cases);
    std::printf("  dds       accepted %4d rejected %4d decoded subresources %d\n", dds.accepted, dds.rejected, dds.deep);
    std::printf("  package   accepted %4d rejected %4d blobs/assets loaded %d\n", pkg.accepted, pkg.rejected, pkg.deep);
    std::printf("  gdeflate  accepted %4d rejected %4d streams decoded %d\n", gdt.accepted, gdt.rejected, gdt.deep);
    std::printf("  noise     accepted %4d rejected %4d\n", noise.accepted, noise.rejected);
    // Both outcomes must actually occur, or the mutator is not exercising the reader.
    const bool exercised = dds.accepted > 0 && dds.rejected > 0 && pkg.accepted > 0 && pkg.rejected > 0 && gdt.accepted > 0 &&
                           gdt.rejected > 0 && dds.accepted + dds.rejected == cases && pkg.accepted + pkg.rejected == cases &&
                           gdt.accepted + gdt.rejected == cases;
    if (!exercised) {
        std::printf("FAIL: a reader was not exercised on both paths\n");
        return 1;
    }
    return 0;
}
