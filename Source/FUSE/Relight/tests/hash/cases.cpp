// FUSE Relight RL-0.5 tests: case generators and fuse_relight_hash evaluation (see cases.hpp and the
// line formats documented in Tools/FUSE/Relight/remix_hash_ref.py).
#include "cases.hpp"

#include <fuse/relight/hash/remix_hash.hpp>

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace fuse::relight::hash::test {

namespace {

std::string dec(std::uint64_t v) {
    return std::to_string(v);
}

std::uint64_t pickSeed(Rng& rng) {
    const std::uint32_t k = rng.below(10);
    if (k < 4) {
        return 0;
    }
    if (k < 6) {
        return rng.below(1000);
    }
    return rng.u64();
}

std::size_t pickLength(Rng& rng) {
    const std::uint32_t k = rng.below(10);
    if (k < 3) {
        return rng.range(0, 16);
    }
    if (k < 8) {
        return rng.range(17, 300);
    }
    return rng.range(300, 2200);
}

const std::uint32_t kSceneScales[] = {
    0x3f800000u, // 1
    0x3f000000u, // 0.5
    0x40000000u, // 2
    0x3e99999au, // 0.3
    0x3dcccccdu, // 0.1
    0x40228f5cu, // 2.54
    0x3cd013a9u, // 0.0254
    0x421d7ae1u, // 39.37
    0x3f400000u, // 0.75
    0x3fc00000u, // 1.5
    0x41200000u, // 10
    0x3c23d70au, // 0.01
    0x3f2aaaabu, // 0.6666667
};

std::uint32_t pickSceneScale(Rng& rng) {
    if (rng.chance(70)) {
        return kSceneScales[rng.below(std::uint32_t(std::size(kSceneScales)))];
    }
    // A "game-like" positive scale in [2^-7, 2^7).
    return ((120u + rng.below(14)) << 23) | (rng.u32() >> 9);
}

// ---- xxh64 / xxh3 ---------------------------------------------------------------------------------

KeyValues genXxh64(Rng& rng) {
    return {{"d", specGen(rng.u64(), pickLength(rng)).spec}, {"seed", hex64(pickSeed(rng))}};
}
KeyValues computeXxh64(const KeyValues& in) {
    const Bytes d = reqBytes(in, "d");
    return {{"out", hex64(xxh64(d.data(), d.size(), reqHex(in, "seed")))}};
}

KeyValues genXxh3(Rng& rng) {
    const std::string seed = rng.chance(30) ? std::string("none") : hex64(pickSeed(rng));
    return {{"d", specGen(rng.u64(), pickLength(rng)).spec}, {"seed", seed}};
}
KeyValues computeXxh3(const KeyValues& in) {
    const Bytes d = reqBytes(in, "d");
    const Hash64 h = req(in, "seed") == "none" ? xxh3_64(d.data(), d.size()) : xxh3_64(d.data(), d.size(), reqHex(in, "seed"));
    return {{"out", hex64(h)}};
}

// ---- geometry components ----------------------------------------------------------------------------

KeyValues genGeomDesc(Rng& rng) {
    const std::uint32_t indexTypes[] = {0u, 1u, 1000165000u, rng.u32()};
    return {{"ic", dec(rng.chance(80) ? rng.below(100000) : rng.u32())},
            {"vc", dec(rng.chance(80) ? rng.below(65536) : rng.u32())},
            {"it", dec(indexTypes[rng.below(4)])},
            {"topo", dec(rng.chance(90) ? rng.below(6) : rng.u32())}};
}
KeyValues computeGeomDesc(const KeyValues& in) {
    return {{"out", hex64(hashGeometryDescriptor(reqU32(in, "ic"), reqU32(in, "vc"), reqU32(in, "it"), reqU32(in, "topo")))}};
}

const std::uint32_t kLayoutFormats[] = {
    vk_format::kR32Sfloat,         vk_format::kR32G32Sfloat,        vk_format::kR32G32B32Sfloat,  vk_format::kR32G32B32A32Sfloat,
    vk_format::kB8G8R8A8Unorm,     vk_format::kR8G8B8A8Uscaled,     vk_format::kR16G16Sscaled,    vk_format::kR16G16B16A16Sfloat,
    vk_format::kR32Uint,           vk_format::kA2B10G10R10SnormPack32, vk_format::kUndefined,
};

std::string layoutElement(Rng& rng, bool forceDefined) {
    const bool defined = forceDefined || rng.chance(55);
    const std::uint32_t strides[] = {8, 12, 16, 20, 24, 28, 32, 36, 40, 44};
    const std::uint32_t stride = rng.chance(85) ? strides[rng.below(10)] : rng.below(65);
    const std::uint32_t fmt = rng.chance(95) ? kLayoutFormats[rng.below(std::uint32_t(std::size(kLayoutFormats)))] : rng.u32();
    return std::string(defined ? "1" : "0") + ":" + dec(rng.below(3)) + ":" + dec(stride) + ":" + dec(fmt);
}

VertexLayoutElement parseLayoutElement(const std::string& text) {
    const auto parts = splitList(text, ':');
    if (parts.size() != 4) {
        fail("bad layout element");
    }
    VertexLayoutElement e;
    const auto d = parseU64(parts[0], 10), s = parseU64(parts[1], 10), st = parseU64(parts[2], 10), f = parseU64(parts[3], 10);
    if (!d || !s || !st || !f) {
        fail("bad layout element value");
    }
    e.defined = *d != 0;
    e.stream = std::uint32_t(*s);
    e.stride = std::uint32_t(*st);
    e.vkFormat = std::uint32_t(*f);
    return e;
}

KeyValues genVLayout(Rng& rng) {
    // Bias towards shared streams and strides so the interleaved path is exercised.
    KeyValues kv{{"p", layoutElement(rng, true)}, {"n", layoutElement(rng, false)}, {"t", layoutElement(rng, false)},
                 {"c", layoutElement(rng, false)}};
    if (rng.chance(50)) {
        const auto p = splitList(kv[0].second, ':');
        for (std::size_t i = 1; i < 4; ++i) {
            auto e = splitList(kv[i].second, ':');
            e[1] = p[1];
            e[2] = p[2];
            kv[i].second = e[0] + ":" + e[1] + ":" + e[2] + ":" + e[3];
        }
    }
    return kv;
}
KeyValues computeVLayout(const KeyValues& in) {
    const VertexLayoutInput layout{parseLayoutElement(req(in, "p")), parseLayoutElement(req(in, "n")),
                                   parseLayoutElement(req(in, "t")), parseLayoutElement(req(in, "c"))};
    const std::uint64_t stride = vertexLayoutStride(layout);
    return {{"stride", dec(stride)}, {"out", hex64(hashVertexLayoutStride(stride))}};
}

KeyValues genRegion(Rng& rng) {
    const std::uint32_t strides[] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 13};
    const std::uint32_t stride = strides[rng.below(std::uint32_t(std::size(strides)))];
    const std::uint32_t esizes[] = {0, 4, 8, 12, 16};
    std::uint32_t esize = esizes[rng.below(5)];
    const std::uint32_t vcount = rng.below(49);
    std::vector<std::uint32_t> uniq;
    if (rng.chance(50)) {
        const std::uint32_t n = std::max(1u, vcount);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (rng.chance(60)) {
                uniq.push_back(i);
            }
        }
        if (uniq.empty()) {
            uniq.push_back(rng.below(n));
        }
    }
    const std::uint32_t maxIdx = uniq.empty() ? 0 : uniq.back();
    std::size_t len = std::max<std::size_t>(std::size_t(vcount) * stride, std::size_t(maxIdx + 1) * stride) + esize + 4;
    std::string data;
    if (stride == 0 && esize == 0 && rng.chance(50)) {
        data = "hex:"; // an undefined stream: null base, nothing read
        len = 0;
    } else {
        data = specGen(rng.u64(), len).spec;
    }
    return {{"d", data}, {"stride", dec(stride)}, {"esize", dec(esize)}, {"size", dec(std::size_t(vcount) * stride)},
            {"uniq", listU32(uniq)}};
}
KeyValues computeRegion(const KeyValues& in) {
    const Bytes d = reqBytes(in, "d");
    const std::vector<std::uint32_t> uniq = parseListU32(req(in, "uniq"));
    const std::uint64_t stride = reqDec(in, "stride"), esize = reqDec(in, "esize"), size = reqDec(in, "size");
    const std::uint64_t maxIdx = uniq.empty() ? 0 : *std::max_element(uniq.begin(), uniq.end());
    const std::uint64_t need = std::max<std::uint64_t>(uniq.empty() ? (size ? size - stride + esize : 0) : maxIdx * stride + esize, 0);
    if (need > d.size() && !(d.empty() && esize == 0 && stride == 0)) {
        fail("region data too short");
    }
    const Hash64 h = hashVertexRegion(d.empty() ? nullptr : d.data(), std::size_t(size), std::size_t(stride), std::size_t(esize), uniq);
    return {{"out", hex64(h)}};
}

KeyValues genUniq(Rng& rng) {
    const std::uint32_t isize = rng.chance(50) ? 2 : 4;
    const std::uint32_t count = rng.below(101);
    const std::uint32_t maxIndex = rng.below(81);
    Bytes idx(std::size_t(count) * isize);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t v = rng.below(maxIndex + 1);
        for (std::uint32_t b = 0; b < isize; ++b) {
            idx[std::size_t(i) * isize + b] = std::uint8_t(v >> (8 * b));
        }
    }
    return {{"d", specHex(idx).spec}, {"isize", dec(isize)}, {"count", dec(count)}, {"max", dec(maxIndex)}};
}
KeyValues computeUniq(const KeyValues& in) {
    const Bytes d = reqBytes(in, "d");
    const std::uint32_t isize = reqU32(in, "isize"), count = reqU32(in, "count");
    if ((isize != 2 && isize != 4) || d.size() < std::size_t(count) * isize) {
        fail("bad uniq input");
    }
    const auto u = sortedUniqueIndices(d.data(), count, isize, reqU32(in, "max"));
    return {{"out", listU32(u)}};
}

KeyValues genLegIdx(Rng& rng) {
    const std::uint32_t isize = rng.chance(50) ? 2 : 4;
    const std::uint32_t count = rng.chance(50) ? rng.below(600) : rng.below(3000);
    return {{"d", specGen(rng.u64(), std::size_t(count) * isize).spec}, {"isize", dec(isize)}, {"count", dec(count)}};
}
KeyValues computeLegIdx(const KeyValues& in) {
    const Bytes d = reqBytes(in, "d");
    const std::uint32_t isize = reqU32(in, "isize"), count = reqU32(in, "count");
    if ((isize != 2 && isize != 4) || d.size() < std::size_t(count) * isize) {
        fail("bad legidx input");
    }
    return {{"out", hex64(hashIndicesLegacy(d.data(), count, isize))}};
}

std::uint32_t floatBits(float f) {
    return std::bit_cast<std::uint32_t>(f);
}

KeyValues genDisc(Rng& rng) {
    const std::uint32_t scale = pickSceneScale(rng);
    const float step = legacyDiscreteStepSize(std::bit_cast<float>(scale));
    const std::uint32_t stepBits = floatBits(step);
    std::uint32_t v = 0;
    switch (rng.below(6)) {
    case 0: v = kSpecialFloats[rng.below(16)]; break;
    case 1: v = rng.u32(); break;
    case 2: v = ((rng.u32() & 1u) << 31) | ((110u + rng.below(40)) << 23) | (rng.u32() >> 9); break;
    default: {
        // Near an exact multiple of the step: k * step, nudged by -3..+3 ulps (or a subnormal).
        const std::int32_t k = std::int32_t(rng.below(20001)) - 10000;
        std::uint32_t m = mulF32Bits(floatBits(float(k)), stepBits);
        const std::int32_t nudge = std::int32_t(rng.below(7)) - 3;
        if ((m & 0x7fffffffu) != 0 && (m & 0x7f800000u) != 0x7f800000u) {
            m = std::uint32_t(std::int64_t(m) + nudge);
        }
        v = rng.chance(5) ? (rng.u32() & 0x807fffffu) : m;
        break;
    }
    }
    return {{"v", hex32(v)}, {"scale", hex32(scale)}};
}
KeyValues computeDisc(const KeyValues& in) {
    const auto scale = std::bit_cast<float>(std::uint32_t(reqHex(in, "scale")));
    const float step = legacyDiscreteStepSize(scale);
    const float inv = 1.f / step;
    const std::uint32_t out = discretizeLegacyBits(std::uint32_t(reqHex(in, "v")), floatBits(step), floatBits(inv));
    return {{"step", hex32(floatBits(step))}, {"inv", hex32(floatBits(inv))}, {"out", hex32(out)}};
}

KeyValues genLegPos(Rng& rng) {
    const std::uint32_t strides[] = {12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 14, 13};
    const std::uint32_t stride = strides[rng.below(std::uint32_t(std::size(strides)))];
    const std::uint32_t vcount = rng.chance(20) ? rng.range(18, 23) : rng.below(46);
    const std::size_t words = (std::size_t(vcount) * stride + 12 + 3) / 4;
    const std::uint64_t h0 = rng.chance(90) ? 0 : rng.u64();
    const std::uint64_t h1 = rng.chance(90) ? 0 : rng.u64();
    return {{"d", specFgen(rng.u64(), words).spec}, {"stride", dec(stride)}, {"size", dec(std::size_t(vcount) * stride)},
            {"scale", hex32(pickSceneScale(rng))},       {"h0", hex64(h0)},       {"h1", hex64(h1)}};
}
KeyValues computeLegPos(const KeyValues& in) {
    const Bytes d = reqBytes(in, "d");
    const std::uint64_t stride = reqDec(in, "stride"), size = reqDec(in, "size");
    if (size != 0 && (stride == 0 || size - stride + 12 > d.size())) {
        fail("legpos data too short");
    }
    Hash64 h0 = reqHex(in, "h0"), h1 = reqHex(in, "h1");
    hashPositionsLegacy(d.data(), std::size_t(size), std::size_t(stride),
                        legacyDiscreteStepSize(std::bit_cast<float>(std::uint32_t(reqHex(in, "scale")))), h0, h1);
    return {{"out", hex64(h0) + "," + hex64(h1)}};
}

KeyValues genVShader(Rng& rng) {
    const std::uint32_t nf = rng.below(9), ni = rng.below(5), nb = rng.below(97);
    return {{"bc", specGen(rng.u64(), rng.below(301)).spec},
            {"f", specGen(rng.u64(), std::size_t(nf) * 16).spec},
            {"nf", dec(nf)},
            {"i", specGen(rng.u64(), std::size_t(ni) * 16).spec},
            {"ni", dec(ni)},
            {"b", specGen(rng.u64(), std::size_t(nb) * 4 / 32).spec},
            {"nb", dec(nb)}};
}
KeyValues computeVShader(const KeyValues& in) {
    const Bytes bc = reqBytes(in, "bc"), f = reqBytes(in, "f"), i = reqBytes(in, "i"), b = reqBytes(in, "b");
    const std::uint32_t nf = reqU32(in, "nf"), ni = reqU32(in, "ni"), nb = reqU32(in, "nb");
    if (f.size() < std::size_t(nf) * 16 || i.size() < std::size_t(ni) * 16 || b.size() < std::size_t(nb) * 4 / 32) {
        fail("vshader constants too short");
    }
    return {{"out", hex64(hashVertexShader(bc, f.data(), nf, i.data(), ni, b.data(), nb))}};
}

KeyValues genRule(Rng& rng) {
    static const std::string_view kTokens[] = {
        "positions", "legacypositions0", "legacypositions1", "texcoords", "indices", "legacyindices",
        "geometrydescriptor", "vertexlayout", "vertexshader", "Positions", "position", "foo", "", "indices;",
    };
    std::string s;
    const std::uint32_t n = rng.below(8);
    for (std::uint32_t k = 0; k < n; ++k) {
        if (k) {
            s += rng.chance(80) ? "," : ",,";
        }
        if (rng.chance(20)) {
            s += " ";
        }
        s += kTokens[rng.below(std::uint32_t(std::size(kTokens)))];
        if (rng.chance(20)) {
            s += "  ";
        }
    }
    if (rng.chance(5)) {
        s = " ";
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
    return {{"s", specHex(std::span(p, s.size())).spec}};
}
KeyValues computeRule(const KeyValues& in) {
    const Bytes s = reqBytes(in, "s");
    const HashRule rule = parseHashRule(std::string_view(reinterpret_cast<const char*>(s.data()), s.size()));
    const std::string canon = formatHashRule(rule);
    const auto* p = reinterpret_cast<const std::uint8_t*>(canon.data());
    return {{"out", dec(rule.bits)}, {"fmt", specHex(std::span(p, canon.size())).spec}, {"id", hex64(hashRuleId(rule))}};
}

KeyValues genCombine(Rng& rng) {
    std::string f;
    for (std::uint32_t i = 0; i < kHashComponentCount; ++i) {
        if (i) {
            f.push_back(',');
        }
        f += hex64(rng.chance(25) ? 0 : (rng.chance(10) ? rng.below(4) : rng.u64()));
    }
    const std::uint32_t named[] = {rules::kTopological.bits, rules::kVertexData.bits, rules::kFullGeometry.bits,
                                   rules::kLegacyAsset0.bits, rules::kLegacyAsset1.bits};
    const std::uint32_t rule = rng.chance(40) ? named[rng.below(5)] : rng.below(512);
    return {{"f", f}, {"rule", dec(rule)}};
}
KeyValues computeCombine(const KeyValues& in) {
    const auto parts = splitList(req(in, "f"), ',');
    if (parts.size() != kHashComponentCount) {
        fail("combine needs 9 fields");
    }
    GeometryHashes g;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto v = parseU64(parts[i], 16);
        if (!v) {
            fail("bad field");
        }
        g.fields[i] = *v;
    }
    const HashRule rule{reqU32(in, "rule")};
    return {{"out", hex64(g.hashForRule(rule))}, {"def", g.isRuleHashDefinedUpstream(rule) ? "1" : "0"}};
}

// ---- draw ------------------------------------------------------------------------------------------

std::string elementSpec(std::uint32_t off, std::uint32_t stride, D3DDeclType type, std::uint32_t stream) {
    return dec(off) + ":" + dec(stride) + ":" + dec(std::uint32_t(type)) + ":" + dec(stream);
}

KeyValues genDraw(Rng& rng) {
    static const D3DPrimitiveType kPrims[] = {
        D3DPrimitiveType::TriangleList,  D3DPrimitiveType::TriangleList, D3DPrimitiveType::TriangleList,
        D3DPrimitiveType::TriangleStrip, D3DPrimitiveType::TriangleFan,  D3DPrimitiveType::PointList,
        D3DPrimitiveType::LineList,      D3DPrimitiveType::LineStrip,    D3DPrimitiveType(7),
    };
    const D3DPrimitiveType prim = kPrims[rng.below(std::uint32_t(std::size(kPrims)))];
    const std::uint32_t pc = rng.range(1, 20);
    const std::uint32_t itypeSel = rng.below(10);
    const IndexType itype = itypeSel < 4 ? IndexType::Uint16 : (itypeSel < 7 ? IndexType::Uint32 : IndexType::NoneKhr);
    const bool indexed = itype != IndexType::NoneKhr;
    const std::uint32_t isize = itype == IndexType::Uint16 ? 2 : 4;

    Bytes idx;
    std::uint32_t vertices = 0;
    if (indexed) {
        const std::uint32_t ic = d3dVertexCount(prim, pc);
        const std::uint32_t lo = rng.below(31);
        const std::uint32_t span = rng.chance(5) ? 0 : rng.range(1, 40);
        idx.resize(std::size_t(ic) * isize);
        for (std::uint32_t i = 0; i < ic; ++i) {
            const std::uint32_t v = lo + rng.below(span + 1);
            for (std::uint32_t b = 0; b < isize; ++b) {
                idx[std::size_t(i) * isize + b] = std::uint8_t(v >> (8 * b));
            }
        }
        vertices = lo + span + 1;
    } else {
        vertices = d3dVertexCount(prim, pc);
    }

    // Stream 0 holds the position; texcoord/normal/color live in stream 0 or in streams 1/2.
    const std::uint32_t strides0[] = {12, 16, 20, 24, 28, 32, 36, 40};
    const std::uint32_t stride0 = strides0[rng.below(8)];
    static const D3DDeclType kPosTypes[] = {D3DDeclType::Float3, D3DDeclType::Float3, D3DDeclType::Float3,
                                            D3DDeclType::Float3, D3DDeclType::Float4, D3DDeclType::Float2,
                                            D3DDeclType::Short4, D3DDeclType::Float16_4};
    D3DDeclType posType = kPosTypes[rng.below(8)];
    if (declTypeElementSize(posType) > stride0) {
        posType = D3DDeclType::Float3;
    }
    const std::uint32_t posOff = 4 * rng.below((stride0 - declTypeElementSize(posType)) / 4 + 1);
    const std::uint32_t stride1 = 8 + 4 * rng.below(3);
    const std::uint32_t stride2 = 12 + 4 * rng.below(2);
    const std::uint32_t len0 = vertices * stride0 + 16, len1 = vertices * stride1 + 16, len2 = vertices * stride2 + 16;
    const std::uint32_t base1 = len0, base2 = len0 + len1;

    const auto sideElement = [&](std::uint32_t presentPercent, D3DDeclType preferred, std::uint32_t sideStream) -> std::string {
        if (!rng.chance(presentPercent)) {
            return "-";
        }
        D3DDeclType type = rng.chance(80) ? preferred : D3DDeclType(rng.below(17));
        if (rng.chance(60)) {
            if (declTypeElementSize(type) > stride0) {
                type = D3DDeclType::D3DColor;
            }
            const std::uint32_t off = 4 * rng.below((stride0 - declTypeElementSize(type)) / 4 + 1);
            return elementSpec(off, stride0, type, 0);
        }
        const std::uint32_t stride = sideStream == 1 ? stride1 : stride2;
        if (declTypeElementSize(type) > stride) {
            type = D3DDeclType::Float2;
        }
        return elementSpec(sideStream == 1 ? base1 : base2, stride, type, sideStream);
    };

    KeyValues kv{{"prim", dec(std::uint32_t(prim))},
                 {"pc", dec(pc)},
                 {"itype", dec(std::uint32_t(itype))},
                 {"idx", specHex(idx).spec},
                 {"vb", specFgen(rng.u64(), (len0 + len1 + len2) / 4).spec},
                 {"pos", elementSpec(posOff, stride0, posType, 0)},
                 {"tc", sideElement(80, D3DDeclType::Float2, 1)},
                 {"n", sideElement(50, D3DDeclType::Float3, 2)},
                 {"c", sideElement(30, D3DDeclType::D3DColor, 2)}};
    if (rng.chance(12)) {
        const std::uint32_t nf = rng.below(9), ni = rng.below(5), nb = rng.below(65);
        kv.emplace_back("vs", "1");
        kv.emplace_back("vsbc", specGen(rng.u64(), rng.range(1, 200)).spec);
        kv.emplace_back("vsf", specGen(rng.u64(), std::size_t(nf) * 16).spec);
        kv.emplace_back("vsnf", dec(nf));
        kv.emplace_back("vsi", specGen(rng.u64(), std::size_t(ni) * 16).spec);
        kv.emplace_back("vsni", dec(ni));
        kv.emplace_back("vsb", specGen(rng.u64(), std::size_t(nb) * 4 / 32).spec);
        kv.emplace_back("vsnb", dec(nb));
    } else {
        kv.emplace_back("vs", "0");
    }
    const std::uint32_t legacyBits = rules::kLegacyAsset0.bits | rules::kLegacyAsset1.bits;
    const HashRule generation = parseHashRule(rules::kDefaultGenerationRuleString);
    const std::uint32_t ruleSel = rng.below(10);
    const std::uint32_t rule = ruleSel < 4 ? generation.bits : (ruleSel < 6 ? (generation.bits | legacyBits) : rng.below(512));
    const std::uint32_t assetSel = rng.below(4);
    const std::uint32_t asset = assetSel < 2 ? parseHashRule(rules::kDefaultAssetRuleString).bits
                                             : (assetSel == 2 ? rules::kLegacyAsset0.bits : rng.below(512));
    kv.emplace_back("rule", dec(rule));
    kv.emplace_back("scale", hex32(pickSceneScale(rng)));
    kv.emplace_back("asset", dec(asset));
    kv.emplace_back("mat", hex64(rng.chance(30) ? 0 : rng.u64()));
    return kv;
}

DrawVertexElement parseDrawElement(const std::string& text, const Bytes& vb) {
    DrawVertexElement e;
    if (text == "-") {
        return e;
    }
    const auto parts = splitList(text, ':');
    if (parts.size() != 4) {
        fail("bad draw element");
    }
    const auto off = parseU64(parts[0], 10), stride = parseU64(parts[1], 10), type = parseU64(parts[2], 10),
               stream = parseU64(parts[3], 10);
    if (!off || !stride || !type || !stream || *off > vb.size()) {
        fail("bad draw element value");
    }
    e.data = vb.data() + *off;
    e.stride = std::uint32_t(*stride);
    e.type = D3DDeclType(std::uint32_t(*type));
    e.stream = std::uint32_t(*stream);
    return e;
}

KeyValues computeDraw(const KeyValues& in) {
    const Bytes vb = reqBytes(in, "vb");
    const Bytes idx = reqBytes(in, "idx");
    DrawGeometryInput draw;
    draw.primitiveType = D3DPrimitiveType(reqU32(in, "prim"));
    draw.primitiveCount = reqU32(in, "pc");
    draw.indexType = IndexType(reqU32(in, "itype"));
    const bool indexed = draw.indexType == IndexType::Uint16 || draw.indexType == IndexType::Uint32;
    if (indexed && idx.size() < std::size_t(d3dVertexCount(draw.primitiveType, draw.primitiveCount)) *
                                    (draw.indexType == IndexType::Uint16 ? 2u : 4u)) {
        fail("draw index data too short");
    }
    draw.indexData = idx.empty() ? nullptr : idx.data();
    draw.position = parseDrawElement(req(in, "pos"), vb);
    draw.texcoord = parseDrawElement(req(in, "tc"), vb);
    draw.normal = parseDrawElement(req(in, "n"), vb);
    draw.color0 = parseDrawElement(req(in, "c"), vb);
    Bytes vsbc, vsf, vsi, vsb;
    if (req(in, "vs") == "1") {
        vsbc = reqBytes(in, "vsbc");
        vsf = reqBytes(in, "vsf");
        vsi = reqBytes(in, "vsi");
        vsb = reqBytes(in, "vsb");
        draw.programmableVsWithCapture = true;
        draw.vertexShader.bytecode = vsbc;
        draw.vertexShader.floatConstants = vsf.data();
        draw.vertexShader.maxConstIndexF = reqU32(in, "vsnf");
        draw.vertexShader.intConstants = vsi.data();
        draw.vertexShader.maxConstIndexI = reqU32(in, "vsni");
        draw.vertexShader.boolConstants = vsb.data();
        draw.vertexShader.maxConstIndexB = reqU32(in, "vsnb");
    }
    // Every element must stay inside vb for the vertices the draw touches (+12 bytes for legacy).
    const std::uint32_t vertexSpan = [&] {
        if (!indexed) {
            return d3dVertexCount(draw.primitiveType, draw.primitiveCount);
        }
        const std::uint32_t isz = draw.indexType == IndexType::Uint16 ? 2u : 4u;
        std::uint32_t mx = 0;
        for (std::size_t i = 0; i + isz <= idx.size(); i += isz) {
            std::uint32_t v = 0;
            for (std::uint32_t b = 0; b < isz; ++b) {
                v |= std::uint32_t(idx[i + b]) << (8 * b);
            }
            mx = std::max(mx, v);
        }
        return mx + 1;
    }();
    for (const DrawVertexElement* e : {&draw.position, &draw.texcoord, &draw.normal, &draw.color0}) {
        if (e->defined() && std::size_t(e->data - vb.data()) + std::size_t(vertexSpan) * e->stride + 16 > vb.size() + e->stride) {
            fail("draw vertex data too short");
        }
    }

    const auto result = computeDrawGeometryHashes(draw, HashRule{reqU32(in, "rule")},
                                                  std::bit_cast<float>(std::uint32_t(reqHex(in, "scale"))));
    if (!result) {
        return {{"ok", "0"}};
    }
    std::string f;
    for (std::uint32_t i = 0; i < kHashComponentCount; ++i) {
        if (i) {
            f.push_back(',');
        }
        f += hex64(result->hashes.fields[i]);
    }
    const Hash64 mat = reqHex(in, "mat");
    return {{"ok", "1"},
            {"f", f},
            {"ic", dec(result->indexCount)},
            {"vc", dec(result->vertexCount)},
            {"min", dec(result->minIndex)},
            {"max", dec(result->maxIndex)},
            {"topo", dec(result->topology)},
            {"it", dec(result->indexType)},
            {"ps", dec(result->positionStride)},
            {"key", hex64(meshReplacementHash(result->hashes, HashRule{reqU32(in, "asset")}, mat))},
            {"leg0", hex64(meshReplacementHashLegacy(*result, rules::kLegacyAsset0, mat))},
            {"leg1", hex64(meshReplacementHashLegacy(*result, rules::kLegacyAsset1, mat))},
            {"def0", result->hashes.isRuleHashDefinedUpstream(rules::kLegacyAsset0) ? "1" : "0"},
            {"def1", result->hashes.isRuleHashDefinedUpstream(rules::kLegacyAsset1) ? "1" : "0"}};
}

// ---- textures ----------------------------------------------------------------------------------------

KeyValues genTexLayout(Rng& rng) {
    const auto formats = allD3DFormats();
    std::uint32_t fmt;
    const std::uint32_t k = rng.below(20);
    if (k < 17) {
        fmt = std::uint32_t(formats[rng.below(std::uint32_t(formats.size()))]);
    } else if (k < 19) {
        fmt = rng.below(256);
    } else {
        fmt = rng.u32();
    }
    const std::uint32_t w = rng.chance(90) ? rng.below(70) : rng.range(70, 300);
    const std::uint32_t h = rng.chance(90) ? rng.below(70) : rng.range(70, 300);
    const std::uint32_t d = rng.chance(85) ? 1 : rng.below(4);
    const std::uint32_t opt = rng.chance(75) ? 15 : rng.below(16);
    const TextureMip0Layout l = textureMip0Layout(D3DFormat(fmt), w, h, d, FormatTableOptions{(opt & 1) != 0, (opt & 2) != 0, (opt & 4) != 0, (opt & 8) != 0});
    const std::uint32_t pitch = std::uint32_t(l.rowBytes) + (rng.chance(70) ? 0 : rng.below(9)) - (rng.chance(5) && l.rowBytes > 4 ? 4 : 0);
    const std::size_t srcLen = std::size_t(l.rowCount) * pitch;
    return {{"fmt", dec(fmt)}, {"w", dec(w)}, {"h", dec(h)}, {"d", dec(d)}, {"opt", dec(opt)}, {"pitch", dec(pitch)},
            {"src", specGen(rng.u64(), srcLen).spec}};
}
KeyValues computeTexLayout(const KeyValues& in) {
    const std::uint32_t opt = reqU32(in, "opt");
    const FormatTableOptions options{(opt & 1) != 0, (opt & 2) != 0, (opt & 4) != 0, (opt & 8) != 0};
    const auto fmt = D3DFormat(reqU32(in, "fmt"));
    const TextureFormatInfo info = textureFormatInfo(fmt, options);
    const TextureMip0Layout l = textureMip0Layout(fmt, reqU32(in, "w"), reqU32(in, "h"), reqU32(in, "d"), options);
    const Bytes src = reqBytes(in, "src");
    const std::uint32_t pitch = reqU32(in, "pitch");
    if (src.size() < std::size_t(l.rowCount) * pitch) {
        fail("texture source too short");
    }
    const auto packed = packTextureMip0(l, src.data(), pitch);
    return {{"info", dec(info.mapped) + "," + dec(info.vkFormat) + "," + dec(info.elementSize) + "," + dec(info.blockWidth) + "," +
                         dec(info.blockHeight) + "," + dec(info.planeCount)},
            {"layout", dec(l.blocksWide) + "," + dec(l.blocksHigh) + "," + dec(l.depth) + "," + dec(l.planes) + "," +
                           dec(l.rowBytes) + "," + dec(l.rowCount) + "," + dec(l.size)},
            {"hash", hex64(hashTextureMip0(packed.data(), packed.size()))},
            {"obs", hex64(hashTextureMip0Obsolete(packed.data(), packed.size()))}};
}

KeyValues genTexDesc(Rng& rng) {
    std::string w;
    for (int i = 0; i < 10; ++i) {
        if (i) {
            w.push_back(',');
        }
        w += dec(rng.chance(70) ? rng.below(4097) : rng.u32());
    }
    return {{"w", w}, {"flags", dec(rng.below(8))}};
}
KeyValues computeTexDesc(const KeyValues& in) {
    const auto parts = splitList(req(in, "w"), ',');
    if (parts.size() != 10) {
        fail("texdesc needs 10 words");
    }
    std::uint32_t words[10];
    for (int i = 0; i < 10; ++i) {
        const auto v = parseU64(parts[std::size_t(i)], 10);
        if (!v || *v > 0xffffffffull) {
            fail("bad texdesc word");
        }
        words[i] = std::uint32_t(*v);
    }
    const std::uint32_t flags = reqU32(in, "flags");
    const TextureDescriptor d{words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9],
                              (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0};
    return {{"out", hex64(hashTextureDescriptor(d))}};
}

// ---- lights ------------------------------------------------------------------------------------------

std::uint32_t lightFloatCount(std::uint32_t type) {
    switch (type) {
    case 0: return 3 + 1 + 6;
    case 1:
    case 2: return 3 + 2 + 3 + 3 + 3 + 6;
    case 3: return 3 + 1 + 3 + 1;
    case 4: return 3 + 1;
    default: return 0;
    }
}

KeyValues genLight(Rng& rng) {
    const std::uint32_t type = rng.below(5);
    const Bytes floats = fgenBytes(rng.u64(), lightFloatCount(type));
    std::string f;
    for (std::size_t i = 0; i < floats.size(); i += 4) {
        if (i) {
            f.push_back(',');
        }
        f += hex32(std::uint32_t(floats[i]) | (std::uint32_t(floats[i + 1]) << 8) | (std::uint32_t(floats[i + 2]) << 16) |
                   (std::uint32_t(floats[i + 3]) << 24));
    }
    return {{"type", dec(type)}, {"en", rng.chance(50) ? "1" : "0"}, {"f", f}};
}
KeyValues computeLight(const KeyValues& in) {
    const std::uint32_t type = reqU32(in, "type");
    const auto parts = splitList(req(in, "f"), ',');
    if (parts.size() != lightFloatCount(type) || parts.empty()) {
        fail("bad light float count");
    }
    std::vector<float> v;
    for (const auto& p : parts) {
        const auto b = parseU64(p, 16);
        if (!b || *b > 0xffffffffull) {
            fail("bad light float");
        }
        v.push_back(std::bit_cast<float>(std::uint32_t(*b)));
    }
    const auto f3 = [&](std::size_t i) { return Float3{v[i], v[i + 1], v[i + 2]}; };
    const auto shaping = [&](std::size_t i) {
        return LightShaping{req(in, "en") == "1", f3(i), v[i + 3], v[i + 4], v[i + 5]};
    };
    Hash64 h = 0;
    switch (type) {
    case 0: h = hashSphereLight(f3(0), v[3], shaping(4)); break;
    case 1: h = hashRectLight(f3(0), Float2{v[3], v[4]}, f3(5), f3(8), f3(11), shaping(14)); break;
    case 2: h = hashDiskLight(f3(0), Float2{v[3], v[4]}, f3(5), f3(8), f3(11), shaping(14)); break;
    case 3: h = hashCylinderLight(f3(0), v[3], f3(4), v[7]); break;
    default: h = hashDistantLight(f3(0), v[3]); break;
    }
    return {{"out", hex64(h)}};
}

// ---- strings -----------------------------------------------------------------------------------------

KeyValues genHexFmt(Rng& rng) {
    return {{"h", hex64(rng.chance(10) ? rng.below(256) : rng.u64())}};
}
KeyValues computeHexFmt(const KeyValues& in) {
    const Hash64 h = reqHex(in, "h");
    return {{"out", hashToString(h)}, {"opt", hashToOptionString(h)}, {"mesh", primName(prim_prefix::kMesh, h)}};
}

KeyValues genParse(Rng& rng) {
    std::string s;
    const Hash64 h = rng.u64();
    switch (rng.below(12)) {
    case 0: s = primName(prim_prefix::kMesh, h); break;
    case 1: s = "mesh_" + hex64(h); break;
    case 2: s = "mesh_0x" + hashToString(h); break;
    case 3: s = "mesh_" + hashToString(h) + "_ref"; break;
    case 4: s = "mesh_" + hashToString(h) + "F"; break; // 17 digits: overflow
    case 5: s = "mat_" + hashToString(h); break;
    case 6: s = hashToOptionString(h); break;
    case 7: s = "  \t0X" + hex64(h >> (4 * rng.below(16))); break;
    case 8: s = "-" + hex64(rng.below(4096)); break;
    case 9: s = std::string("mesh_") + (rng.chance(50) ? "" : "zz"); break;
    case 10: s = "0x"; break;
    default: {
        static const char kChars[] = "0123456789abcdefABCDEFxX+- _meshmat";
        const std::uint32_t n = rng.below(24);
        for (std::uint32_t i = 0; i < n; ++i) {
            s.push_back(kChars[rng.below(sizeof(kChars) - 1)]);
        }
        break;
    }
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
    return {{"s", specHex(std::span(p, s.size())).spec}};
}
KeyValues computeParse(const KeyValues& in) {
    const Bytes b = reqBytes(in, "s");
    const std::string_view s(reinterpret_cast<const char*>(b.data()), b.size());
    const auto opt = parseHashOption(s);
    return {{"opt", opt ? hex64(*opt) : std::string("none")}, {"prim", hex64(hashFromPrimName(s, prim_prefix::kMesh))}};
}

const CaseFunction kFunctions[] = {
    {"xxh64", {"out"}, genXxh64, computeXxh64},
    {"xxh3", {"out"}, genXxh3, computeXxh3},
    {"geomdesc", {"out"}, genGeomDesc, computeGeomDesc},
    {"vlayout", {"stride", "out"}, genVLayout, computeVLayout},
    {"region", {"out"}, genRegion, computeRegion},
    {"uniq", {"out"}, genUniq, computeUniq},
    {"legidx", {"out"}, genLegIdx, computeLegIdx},
    {"disc", {"step", "inv", "out"}, genDisc, computeDisc},
    {"legpos", {"out"}, genLegPos, computeLegPos},
    {"vshader", {"out"}, genVShader, computeVShader},
    {"rule", {"out", "fmt", "id"}, genRule, computeRule},
    {"combine", {"out", "def"}, genCombine, computeCombine},
    {"draw",
     {"ok", "f", "ic", "vc", "min", "max", "topo", "it", "ps", "key", "leg0", "leg1", "def0", "def1"},
     genDraw,
     computeDraw},
    {"texlayout", {"info", "layout", "hash", "obs"}, genTexLayout, computeTexLayout},
    {"texdesc", {"out"}, genTexDesc, computeTexDesc},
    {"light", {"out"}, genLight, computeLight},
    {"hexfmt", {"out", "opt", "mesh"}, genHexFmt, computeHexFmt},
    {"parse", {"opt", "prim"}, genParse, computeParse},
};

} // namespace

std::span<const CaseFunction> caseFunctions() {
    return kFunctions;
}

const CaseFunction* findCaseFunction(std::string_view name) {
    for (const CaseFunction& f : kFunctions) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

KeyValues caseInputs(const CaseFunction& fn, const KeyValues& kv) {
    KeyValues out;
    for (const auto& [k, v] : kv) {
        if (std::find(fn.outputKeys.begin(), fn.outputKeys.end(), k) == fn.outputKeys.end()) {
            out.emplace_back(k, v);
        }
    }
    return out;
}

std::string verifyCase(const CaseFunction& fn, const KeyValues& kv) {
    try {
        const KeyValues inputs = caseInputs(fn, kv);
        const KeyValues outputs = fn.compute(inputs);
        std::string diff;
        for (const auto& [k, v] : outputs) {
            const std::string* expected = findValue(kv, k);
            if (expected == nullptr) {
                diff += " missing expected '" + k + "'";
            } else if (*expected != v) {
                diff += " " + k + ": expected " + *expected + " got " + v;
            }
        }
        return diff;
    } catch (const std::exception& e) {
        return std::string(" malformed case: ") + e.what();
    }
}

void emitRandomCases(const CaseFunction& fn, std::uint64_t seed, std::size_t count,
                     const std::function<void(const std::string&)>& sink) {
    Rng rng(seed ^ (std::uint64_t(fn.name.size()) << 56) ^ hashContiguousMemory(fn.name.data(), fn.name.size()));
    for (std::size_t i = 0; i < count; ++i) {
        KeyValues kv = fn.generate(rng);
        const KeyValues outputs = fn.compute(kv);
        kv.insert(kv.end(), outputs.begin(), outputs.end());
        sink(formatCaseLine(fn.name, kv));
    }
}

} // namespace fuse::relight::hash::test
