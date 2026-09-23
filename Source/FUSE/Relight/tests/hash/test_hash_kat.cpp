// FUSE Relight RL-0.5: known-answer, property and parity driver for fuse_relight_hash.
//
//   fuse_relight_hash_kat --vectors xxhash_vectors.txt --kat kat_vectors.txt --self --digest-check digest_golden.txt
//       (ctest rl_hash_kat) official xxHash vectors; the KAT table (derived from the upstream
//       oracle); property tests; library vs upstream oracle on random cases; the frozen digest of
//       the 10k-per-function random case set.
//   fuse_relight_hash_kat --emit <file> [--count N] [--seed S]
//       writes the random case set (library outputs) for Tools/FUSE/Relight/remix_hash_ref.py.
//   fuse_relight_hash_kat --digest [--count N] [--seed S]
//       prints one digest per function (native vs MinGW/Wine comparison, ctest rl_hash_wine).
//   fuse_relight_hash_kat --emit-kat <file>
//       regenerates kat_vectors.txt from the upstream oracle (x86-64 with SSE4.1 only).
//   fuse_relight_hash_kat --verify <file>
//       checks any case file against the library.
//   fuse_relight_hash_kat --skip <reason>
//       prints the reason and exits 77 (ctest skip), for tests whose prerequisites are missing.
// Exit status: 0 pass, 1 failure, 2 usage or I/O error.
#include "case_io.hpp"
#include "cases.hpp"
#include "upstream_oracle.hpp"

#include <fuse/relight/hash/remix_hash.hpp>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace fuse::relight::hash;
using namespace fuse::relight::hash::test;

namespace {

constexpr std::uint64_t kDefaultSeed = 0x52454C4947485431ull; // "RELIGHT1"
constexpr std::size_t kDefaultCount = 10000;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 50) {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        }
    }
}

bool readLines(const std::string& path, std::vector<std::string>& lines) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return true;
}

// ---- official xxHash vectors ------------------------------------------------------------------------

bool runVectors(const std::string& path) {
    std::vector<std::string> lines;
    if (!readLines(path, lines)) {
        return false;
    }
    // The xxHash sanity buffer: byteGen = PRIME32; byte = byteGen >> 56; byteGen *= PRIME64.
    std::vector<std::uint8_t> buffer(4096 + 64 + 1);
    std::uint64_t byteGen = 2654435761u;
    for (auto& b : buffer) {
        b = std::uint8_t(byteGen >> 56);
        byteGen *= 11400714785074694797ull;
    }
    int count = 0;
    for (const std::string& line : lines) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ss(line);
        std::string fn, len, seed, expected;
        ss >> fn >> len >> seed >> expected;
        const auto n = parseU64(len, 10);
        const auto s = parseU64(seed, 16);
        const auto e = parseU64(expected, 16);
        if (!n || !s || !e || *n > buffer.size()) {
            check(false, "malformed vector line: " + line);
            continue;
        }
        Hash64 got = 0;
        if (fn == "xxh64") {
            got = xxh64(buffer.data(), std::size_t(*n), *s);
        } else if (fn == "xxh3") {
            got = xxh3_64(buffer.data(), std::size_t(*n), *s);
            if (*s == 0) {
                check(xxh3_64(buffer.data(), std::size_t(*n)) == *e, "xxh3 unseeded len " + len);
            }
        } else {
            check(false, "unknown vector function: " + fn);
            continue;
        }
        check(got == *e, fn + " len " + len + " seed " + seed + ": expected " + expected + " got " + hex64(got));
        ++count;
    }
    std::printf("xxHash official vectors: %d checked\n", count);
    return count > 0;
}

// ---- case files -------------------------------------------------------------------------------------

bool runCaseFile(const std::string& path, bool alsoOracle) {
    std::vector<std::string> lines;
    if (!readLines(path, lines)) {
        return false;
    }
    std::map<std::string, int> perFunction;
    int oracleChecked = 0;
    for (const std::string& line : lines) {
        std::string fn;
        KeyValues kv;
        if (!parseCaseLine(line, fn, kv)) {
            continue;
        }
        const CaseFunction* f = findCaseFunction(fn);
        if (f == nullptr) {
            check(false, "unknown function in " + path + ": " + fn);
            continue;
        }
        const std::string diff = verifyCase(*f, kv);
        check(diff.empty(), fn + diff + "\n    " + line.substr(0, 300));
        ++perFunction[fn];
        if (alsoOracle) {
            const auto oracle = oracleCompute(fn, caseInputs(*f, kv));
            if (oracle) {
                for (const auto& [k, v] : *oracle) {
                    const std::string* expected = findValue(kv, k);
                    check(expected != nullptr && *expected == v, "oracle disagrees with KAT " + fn + " " + k);
                }
                ++oracleChecked;
            }
        }
    }
    std::printf("%s:", path.c_str());
    for (const auto& [fn, n] : perFunction) {
        std::printf(" %s=%d", fn.c_str(), n);
    }
    std::printf("%s\n", alsoOracle ? (" (oracle re-checked " + std::to_string(oracleChecked) + ")").c_str() : "");
    return !perFunction.empty();
}

// ---- digests ------------------------------------------------------------------------------------------

std::vector<std::pair<std::string, Hash64>> computeDigests(std::uint64_t seed, std::size_t count) {
    std::vector<std::pair<std::string, Hash64>> out;
    std::string all;
    for (const CaseFunction& f : caseFunctions()) {
        std::string text;
        emitRandomCases(f, seed, count, [&](const std::string& line) {
            text += line;
            text.push_back('\n');
        });
        const Hash64 d = xxh3_64(text.data(), text.size());
        out.emplace_back(std::string(f.name), d);
        all += hex64(d);
    }
    out.emplace_back("all", xxh3_64(all.data(), all.size()));
    return out;
}

std::string formatDigests(const std::vector<std::pair<std::string, Hash64>>& digests, std::uint64_t seed, std::size_t count) {
    std::string s = "# fuse_relight_hash random-case digests: seed " + hex64(seed) + ", " + std::to_string(count) +
                    " cases per function (XXH3 of the emitted case lines)\n";
    for (const auto& [name, d] : digests) {
        s += "digest " + name + " " + hex64(d) + "\n";
    }
    return s;
}

bool runDigestCheck(const std::string& path, std::uint64_t seed, std::size_t count) {
    std::vector<std::string> lines;
    if (!readLines(path, lines)) {
        return false;
    }
    std::map<std::string, std::string> expected;
    for (const std::string& line : lines) {
        std::istringstream ss(line);
        std::string tag, name, value;
        if (ss >> tag >> name >> value && tag == "digest") {
            expected[name] = value;
        }
    }
    for (const auto& [name, d] : computeDigests(seed, count)) {
        const auto it = expected.find(name);
        check(it != expected.end() && it->second == hex64(d),
              "digest " + name + ": golden " + (it == expected.end() ? std::string("<missing>") : it->second) + " got " + hex64(d));
    }
    std::printf("random-case digests (%zu per function) checked against %s\n", count, path.c_str());
    return true;
}

// ---- properties ---------------------------------------------------------------------------------------

std::uint32_t hwMul(std::uint32_t a, std::uint32_t b) {
    volatile float fa = std::bit_cast<float>(a);
    volatile float fb = std::bit_cast<float>(b);
    const float r = fa * fb;
    return std::bit_cast<std::uint32_t>(r);
}

std::uint32_t randomFloatBits(Rng& rng) {
    switch (rng.below(5)) {
    case 0: return kSpecialFloats[rng.below(16)];
    case 1: return rng.u32();
    case 2: return (rng.u32() & 0x807fffffu) | (rng.below(3) << 23); // subnormal / tiny
    case 3: return (rng.u32() & 0x80000000u) | ((110u + rng.below(40)) << 23) | (rng.u32() >> 9);
    default: return (rng.u32() & 0x80000000u) | ((230u + rng.below(25)) << 23) | (rng.u32() >> 9); // near overflow
    }
}

bool isNaNBits(std::uint32_t b) {
    return (b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu) != 0;
}

void runSoftFloatChecks() {
#if defined(__x86_64__) || defined(_M_X64)
    Rng rng(0xF10A7ull);
    int n = 0;
    for (int i = 0; i < 2000000; ++i) {
        const std::uint32_t a = randomFloatBits(rng);
        const std::uint32_t b = randomFloatBits(rng);
        if (isNaNBits(a) && isNaNBits(b)) {
            continue; // x86 returns the first *source* operand; the compiler may swap a and b
        }
        const std::uint32_t soft = mulF32Bits(a, b);
        const std::uint32_t hard = hwMul(a, b);
        if (soft != hard) {
            check(false, "mulF32Bits(" + hex32(a) + ", " + hex32(b) + ") = " + hex32(soft) + ", hardware " + hex32(hard));
        }
        ++n;
    }
    for (int i = 0; i < 1000000; ++i) {
        const std::uint32_t a = randomFloatBits(rng);
        const std::uint32_t soft = floorF32Bits(a);
        if (isNaNBits(a)) {
            check(soft == a, "floorF32Bits NaN pass-through " + hex32(a));
            continue;
        }
        volatile float fa = std::bit_cast<float>(a);
        const float hard = std::floor(float(fa));
        check(soft == std::bit_cast<std::uint32_t>(hard), "floorF32Bits(" + hex32(a) + ")");
    }
    std::printf("soft-float: %d multiplies and 1000000 floors match the hardware\n", n);
#else
    std::printf("soft-float hardware comparison skipped (not x86-64)\n");
#endif
}

void runOracleCrossCheck(std::size_t perFunction) {
    int compared = 0;
    for (const CaseFunction& f : caseFunctions()) {
        emitRandomCases(f, kDefaultSeed ^ 0x0AC1Eull, perFunction, [&](const std::string& line) {
            std::string fn;
            KeyValues kv;
            parseCaseLine(line, fn, kv);
            const auto oracle = oracleCompute(fn, caseInputs(f, kv));
            if (!oracle) {
                return;
            }
            for (const auto& [k, v] : *oracle) {
                const std::string* got = findValue(kv, k);
                check(got != nullptr && *got == v,
                      "library vs upstream oracle " + fn + " " + k + ": oracle " + v + " library " + (got ? *got : "<none>") +
                          "\n    " + line.substr(0, 300));
            }
            ++compared;
        });
    }
    std::printf("library vs upstream oracle: %d random cases compared%s\n", compared,
                oracleAvailable() ? "" : " (SSE4.1 oracle functions unavailable on this CPU)");
}

void runProperties() {
    check(xxhashVersionNumber() == 803, "vendored xxHash is 0.8.3");

    // Rules: parsing, canonical order, defaults and the named rules.
    const HashRule asset = parseHashRule(rules::kDefaultAssetRuleString);
    check(asset == HashRule{(1u << 0) | (1u << 4) | (1u << 6)}, "default asset rule bits");
    check(parseHashRule(rules::kDefaultGenerationRuleString) == HashRule{0x1D9u}, "default generation rule bits");
    check(parseHashRule("legacyindices, legacypositions0") == rules::kLegacyAsset0, "legacy rule 0 from text");
    check(parseHashRule("geometrydescriptor,indices,positions") == asset, "rule order does not matter");
    check(formatHashRule(parseHashRule("indices ,positions,geometrydescriptor")) == rules::kDefaultAssetRuleString,
          "canonical rule string");
    check(parseHashRule("").empty() && parseHashRule(" ").empty() && parseHashRule("Positions").empty(), "empty/unknown rules");
    for (std::uint32_t bits = 0; bits < 512; ++bits) {
        check(parseHashRule(formatHashRule(HashRule{bits})).bits == bits, "rule round trip " + std::to_string(bits));
    }

    // Combiner: empty rule -> 0; a single component is used as-is; a leading zero is skipped.
    GeometryHashes g;
    for (std::uint32_t i = 0; i < kHashComponentCount; ++i) {
        g.fields[i] = 0x1111111111111111ull * (i + 1);
    }
    check(g.hashForRule(HashRule{}) == 0, "empty rule hashes to 0");
    check(g.hashForRule(HashRule{}.set(HashComponent::Indices)) == g[HashComponent::Indices], "single component as-is");
    const Hash64 two = g.hashForRule(asset);
    check(two == xxh64(&g.fields[6], 8, xxh64(&g.fields[4], 8, g.fields[0])), "combiner chain order");
    GeometryHashes z = g;
    z[HashComponent::Positions] = 0;
    check(z.hashForRule(asset) == xxh64(&z.fields[6], 8, z.fields[4]), "leading zero component is skipped (upstream quirk)");
    z[HashComponent::LegacyPositions0] = 0;
    check(!z.isRuleHashDefinedUpstream(rules::kLegacyAsset0) && z.isRuleHashDefinedUpstream(asset), "legacy slot definedness");

    // Draw-level: components that are absent.
    std::vector<float> verts;
    for (int i = 0; i < 30; ++i) {
        verts.push_back(float(i));
        verts.push_back(float(i) * 0.5f);
        verts.push_back(-float(i));
    }
    DrawGeometryInput draw;
    draw.primitiveType = D3DPrimitiveType::TriangleList;
    draw.primitiveCount = 10;
    draw.position = {reinterpret_cast<const std::uint8_t*>(verts.data()), 12, D3DDeclType::Float3, 0};
    const HashRule all{0x1FFu};
    const auto nonIndexed = computeDrawGeometryHashes(draw, all, 1.f);
    check(nonIndexed && nonIndexed->hashes[HashComponent::Texcoords] == 0, "non-indexed draw without texcoords hashes 0");
    check(nonIndexed && nonIndexed->indexType == 0 && nonIndexed->indexCount == 0, "non-indexed draw: indexType 0, indexCount 0");
    check(nonIndexed && nonIndexed->hashes[HashComponent::LegacyPositions0] != 0, "30 vertices: legacypositions0 captured");
    check(nonIndexed && nonIndexed->hashes[HashComponent::VertexShader] == 0, "fixed function: vertexshader 0");
    std::vector<std::uint16_t> idx16;
    for (int i = 0; i < 30; ++i) {
        idx16.push_back(std::uint16_t(5 + (i * 7) % 12));
    }
    draw.indexType = IndexType::Uint16;
    draw.indexData = idx16.data();
    const auto indexed = computeDrawGeometryHashes(draw, all, 1.f);
    check(indexed && indexed->hashes[HashComponent::Texcoords] != 0, "indexed draw without texcoords still hashes texcoords");
    check(indexed && indexed->minIndex == 5 && indexed->vertexCount == 12, "index rebasing");
    check(indexed && indexed->hashes[HashComponent::LegacyPositions0] == 0, "12 vertices: legacypositions0 stays 0");
    idx16.assign(30, 3);
    check(!computeDrawGeometryHashes(draw, all, 1.f), "maxIndex == minIndex draws are skipped");

    // Strings.
    Rng rng(99);
    for (int i = 0; i < 1000; ++i) {
        const Hash64 h = rng.u64();
        check(parseHashOption(hashToOptionString(h)) == h, "option string round trip");
        check(hashFromPrimName(primName(prim_prefix::kMesh, h), prim_prefix::kMesh) == h, "prim name round trip");
        check(hashToString(h).size() == 16, "hashToString width");
    }
    check(hashToString(0xABCull) == "0000000000000ABC", "hashToString format");
    check(!parseHashOption("zz") && !parseHashOption("") && !parseHashOption("0x10000000000000000"), "stoull failures");
    check(hashFromPrimName("mat_00000000000000FF", prim_prefix::kMesh) == 0, "prefix mismatch");

    // Texture layouts.
    check(textureMip0Layout(D3DFormat::DXT1, 4, 4).size == 8, "DXT1 4x4 = one block");
    check(textureMip0Layout(D3DFormat::DXT5, 13, 7).size == 16 * 4 * 2, "DXT5 13x7 = 4x2 blocks");
    check(textureMip0Layout(D3DFormat::A8R8G8B8, 3, 3).rowBytes == 12, "A8R8G8B8 row bytes");
    check(textureMip0Layout(D3DFormat::R8G8B8, 5, 2).rowBytes == 16, "R8G8B8 (unsupported, 3 bytes) rows aligned to 4");
    check(textureMip0Layout(D3DFormat::L8, 5, 3).size == 8 * 3, "L8 rows aligned to 4");
    check(textureMip0Layout(D3DFormat::NV12, 8, 8).planes == 2 && textureMip0Layout(D3DFormat::YV12, 8, 8).planes == 2,
          "planar formats: min(planeCount, 2)");
    check(textureMip0Layout(D3DFormat::YUY2, 8, 2).rowBytes == 32, "YUY2 maps to a 4-byte format");
    check(textureMip0Layout(D3DFormat::R8G8_B8G8, 5, 1).rowBytes == 12, "RGBG: 2x1 blocks of 4 bytes");
    check(textureMip0Layout(D3DFormat::MULTI2_ARGB8, 8, 8).size == 0, "formats without a mapping or size hash 0 bytes");
    check(textureMip0Layout(D3DFormat::A8R8G8B8, 0, 0).size == 4, "zero extents clamp to 1");
    FormatTableOptions amd;
    amd.d24s8Supported = false;
    check(textureFormatInfo(D3DFormat::D24S8, amd).elementSize == 8, "D24S8 falls back to D32S8 (8 bytes)");
    FormatTableOptions noX4;
    noX4.supportX4R4G4B4 = false;
    check(!textureFormatInfo(D3DFormat::X4R4G4B4, noX4).mapped, "X4R4G4B4 unsupported by option");
    check(isTextureHashed(D3DResourceType::Texture, 0) && !isTextureHashed(D3DResourceType::Texture, kD3DUsageDepthStencil) &&
              !isTextureHashed(D3DResourceType::CubeTexture, 0),
          "only non-depth 2D textures are hashed");
    check(legacyMaterialHash(0x1234) == 0x1234, "material hash = colour texture hash");
    check(fullMipLevelCount(256, 64, 1) == 9 && fullMipLevelCount(1, 1, 1) == 1, "mip level count");
    for (D3DFormat f : allD3DFormats()) {
        check(!d3dFormatName(f).empty(), "every D3DFormat has a name");
    }
    std::printf("property tests done\n");
}

// ---- KAT generation ----------------------------------------------------------------------------------

KeyValues kv(std::initializer_list<std::pair<const char*, std::string>> list) {
    KeyValues out;
    for (const auto& [k, v] : list) {
        out.emplace_back(k, v);
    }
    return out;
}

std::string floatsHex(std::initializer_list<float> values) {
    std::vector<std::uint8_t> bytes;
    for (float f : values) {
        const auto b = std::bit_cast<std::uint32_t>(f);
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(std::uint8_t(b >> (8 * i)));
        }
    }
    return specHex(bytes).spec;
}

std::vector<std::pair<std::string, KeyValues>> curatedCases() {
    std::vector<std::pair<std::string, KeyValues>> c;
    const auto add = [&](const char* fn, KeyValues in) { c.emplace_back(fn, std::move(in)); };
    // Geometry descriptor: indexed list, the non-indexed quirk (indexType 0), strips and fans.
    add("geomdesc", kv({{"ic", "3"}, {"vc", "3"}, {"it", "0"}, {"topo", "3"}}));
    add("geomdesc", kv({{"ic", "0"}, {"vc", "36"}, {"it", "0"}, {"topo", "3"}}));
    add("geomdesc", kv({{"ic", "36"}, {"vc", "24"}, {"it", "1"}, {"topo", "3"}}));
    add("geomdesc", kv({{"ic", "0"}, {"vc", "6"}, {"it", "1000165000"}, {"topo", "4"}}));
    add("geomdesc", kv({{"ic", "12"}, {"vc", "8"}, {"it", "0"}, {"topo", "5"}}));
    // Vertex layout: interleaved GPU-friendly, split streams, unfriendly formats.
    add("vlayout", kv({{"p", "1:0:32:106"}, {"n", "1:0:32:106"}, {"t", "1:0:32:103"}, {"c", "0:0:0:0"}}));
    add("vlayout", kv({{"p", "1:0:36:106"}, {"n", "1:0:36:106"}, {"t", "1:0:36:103"}, {"c", "1:0:36:44"}}));
    add("vlayout", kv({{"p", "1:0:24:106"}, {"n", "0:0:0:0"}, {"t", "1:1:8:103"}, {"c", "0:0:0:0"}}));
    add("vlayout", kv({{"p", "1:0:16:94"}, {"n", "0:0:0:0"}, {"t", "0:0:0:0"}, {"c", "0:0:0:0"}}));
    add("vlayout", kv({{"p", "1:0:28:109"}, {"n", "1:0:28:98"}, {"t", "1:0:28:100"}, {"c", "0:0:0:0"}}));
    // Legacy discretisation: step 1 (sceneScale 1), step 2.54, step 0.3, specials.
    for (const char* scale : {"3f800000", "40228f5c", "3e99999a", "3cd013a9"}) {
        for (const char* v : {"00000000", "80000000", "3f7fffff", "3f800000", "3f800001", "bf000000", "bf800000", "40200000",
                              "c0200000", "7f800000", "ff800000", "7fc00000", "7f800001", "ffc12345", "00000001", "80000001",
                              "7f7fffff", "ff7fffff", "4b000001", "cb7fffff", "3c23d70a", "4479fff0"}) {
            add("disc", kv({{"v", v}, {"scale", scale}}));
        }
    }
    // Legacy positions: 19, 20, 21 and 25 vertices (h0 is captured before vertex 20).
    for (std::uint32_t verts : {19u, 20u, 21u, 25u}) {
        for (std::uint32_t stride : {12u, 16u, 20u}) {
            add("legpos", kv({{"d", specFgen(0x1000 + verts * 64 + stride, (verts * stride + 12) / 4 + 1).spec},
                              {"stride", std::to_string(stride)},
                              {"size", std::to_string(verts * stride)},
                              {"scale", stride == 16 ? "40228f5c" : "3f800000"},
                              {"h0", "0000000000000000"},
                              {"h1", "0000000000000000"}}));
        }
    }
    // Legacy indices: short buffers, exactly 1024 bytes, just over (sampled).
    for (std::uint32_t count : {3u, 511u, 512u, 513u, 1500u}) {
        add("legidx", kv({{"d", specGen(0x2000 + count, count * 2).spec}, {"isize", "2"}, {"count", std::to_string(count)}}));
        add("legidx", kv({{"d", specGen(0x3000 + count, count * 4).spec}, {"isize", "4"}, {"count", std::to_string(count)}}));
    }
    // Regions: a textured triangle's positions (non-indexed, indexed subset) and an undefined stream.
    const std::string tri = floatsHex({0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f});
    add("region", kv({{"d", tri}, {"stride", "12"}, {"esize", "12"}, {"size", "36"}, {"uniq", "-"}}));
    add("region", kv({{"d", tri}, {"stride", "12"}, {"esize", "12"}, {"size", "36"}, {"uniq", "0,2"}}));
    add("region", kv({{"d", "hex:"}, {"stride", "0"}, {"esize", "0"}, {"size", "0"}, {"uniq", "0,1,2"}}));
    add("region", kv({{"d", "hex:"}, {"stride", "0"}, {"esize", "0"}, {"size", "0"}, {"uniq", "-"}}));
    // Rules.
    for (std::string s : {std::string(rules::kDefaultAssetRuleString), std::string(rules::kDefaultGenerationRuleString),
                          std::string("legacypositions0,legacyindices"), std::string(" positions , indices ,, "),
                          std::string("Positions,INDICES"), std::string(""), std::string("vertexshader,positions")}) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
        add("rule", kv({{"s", specHex(std::span(p, s.size())).spec}}));
    }
    // Combiner quirks.
    const std::string f9 = "1111111111111111,2222222222222222,3333333333333333,4444444444444444,5555555555555555,"
                           "6666666666666666,7777777777777777,8888888888888888,9999999999999999";
    const std::string f9z = "0000000000000000,0000000000000000,3333333333333333,0000000000000000,5555555555555555,"
                            "6666666666666666,7777777777777777,0000000000000000,0000000000000000";
    for (const std::string& f : {f9, f9z}) {
        for (std::uint32_t rule : {0u, 0x51u, 0x1D9u, 0x22u, 0x24u, 0x50u, 0x189u, 0x1FFu, 0x100u}) {
            add("combine", kv({{"f", f}, {"rule", std::to_string(rule)}}));
        }
    }
    // Strings.
    for (const char* h : {"0000000000000000", "0000000000000abc", "ffffffffffffffff", "0123456789abcdef"}) {
        add("hexfmt", kv({{"h", h}}));
    }
    for (std::string s : {std::string("mesh_0123456789ABCDEF"), std::string("mesh_0123456789abcdef_1"), std::string("mesh_"),
                          std::string("mat_0123456789ABCDEF"), std::string("0x00000000000000FF"), std::string("  -1"),
                          std::string("0x"), std::string("mesh_10000000000000000"), std::string("mesh_0x1F")}) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(s.data());
        add("parse", kv({{"s", specHex(std::span(p, s.size())).spec}}));
    }
    // Every D3DFORMAT at an odd size (13x7) and 1x1, default options; option-dependent formats.
    std::uint64_t seed = 0x7E0000;
    for (D3DFormat f : allD3DFormats()) {
        for (auto [w, h] : {std::pair{13u, 7u}, std::pair{1u, 1u}}) {
            const TextureMip0Layout l = textureMip0Layout(f, w, h);
            add("texlayout", kv({{"fmt", std::to_string(std::uint32_t(f))},
                                 {"w", std::to_string(w)},
                                 {"h", std::to_string(h)},
                                 {"d", "1"},
                                 {"opt", "15"},
                                 {"pitch", std::to_string(l.rowBytes)},
                                 {"src", specGen(++seed, std::size_t(l.size)).spec}}));
        }
    }
    for (D3DFormat f : {D3DFormat::X4R4G4B4, D3DFormat::DF16, D3DFormat::DF24, D3DFormat::D32, D3DFormat::D24S8, D3DFormat::INTZ}) {
        for (std::uint32_t opt : {0u, 7u, 8u}) {
            const TextureMip0Layout l = textureMip0Layout(f, 4, 4, 1, FormatTableOptions{(opt & 1) != 0, (opt & 2) != 0, (opt & 4) != 0, (opt & 8) != 0});
            add("texlayout", kv({{"fmt", std::to_string(std::uint32_t(f))}, {"w", "4"}, {"h", "4"}, {"d", "1"},
                                 {"opt", std::to_string(opt)}, {"pitch", std::to_string(l.rowBytes)},
                                 {"src", specGen(++seed, std::size_t(l.size)).spec}}));
        }
    }
    // Render-target descriptors.
    add("texdesc", kv({{"w", "1024,768,1,1,1,1,21,0,0,0"}, {"flags", "0"}}));
    add("texdesc", kv({{"w", "256,256,1,1,9,0,894720068,1,0,0"}, {"flags", "0"}}));
    add("texdesc", kv({{"w", "640,480,1,1,1,1,22,0,0,0"}, {"flags", "2"}}));
    // Lights: a sphere with and without shaping, each other shape once.
    add("light", kv({{"type", "0"}, {"en", "0"}, {"f", "3f800000,40000000,40400000,40a00000,00000000,00000000,3f800000,3f000000,3e800000,3f800000"}}));
    add("light", kv({{"type", "0"}, {"en", "1"}, {"f", "3f800000,40000000,40400000,40a00000,00000000,00000000,3f800000,3f000000,3e800000,3f800000"}}));
    add("light", kv({{"type", "1"}, {"en", "1"}, {"f", "00000000,00000000,00000000,3f800000,40000000,3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,3f800000,3f400000,00000000,3f800000"}}));
    add("light", kv({{"type", "2"}, {"en", "0"}, {"f", "00000000,00000000,00000000,3f800000,40000000,3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,3f800000,3f400000,00000000,3f800000"}}));
    add("light", kv({{"type", "3"}, {"en", "0"}, {"f", "3f800000,40000000,40400000,3e800000,00000000,00000000,3f800000,41200000"}}));
    add("light", kv({{"type", "4"}, {"en", "0"}, {"f", "00000000,bf800000,00000000,3c8efa35"}}));
    return c;
}

bool emitKat(const std::string& path) {
    if (!oracleAvailable()) {
        std::fprintf(stderr, "--emit-kat needs the upstream oracle (x86-64 with SSE4.1)\n");
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    out << "# FUSE Relight RL-0.5 known-answer table (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.1).\n"
           "# Generated by `fuse_relight_hash_kat --emit-kat`: every output below was computed by the upstream\n"
           "# oracle (tests/hash/upstream_oracle.cpp, the dxvk-remix @0867d3c hashing code kept verbatim, with\n"
           "# the real SSE4.1 _mm_round_ps legacy path), except texlayout, whose D3DFORMAT table has no\n"
           "# standalone upstream form: those lines come from fuse_relight_hash and are checked\n"
           "# independently by Tools/FUSE/Relight/remix_hash_ref.py. Line format: see remix_hash_ref.py.\n";
    const auto write = [&](const std::string& fn, KeyValues in) {
        const CaseFunction* f = findCaseFunction(fn);
        auto outputs = oracleCompute(fn, in);
        if (!outputs) {
            outputs = f->compute(in);
        }
        in.insert(in.end(), outputs->begin(), outputs->end());
        out << formatCaseLine(fn, in) << "\n";
    };
    for (auto& [fn, in] : curatedCases()) {
        write(fn, in);
    }
    // Plus a small random sample of every function (fixed seed), including full draws.
    for (const CaseFunction& f : caseFunctions()) {
        const std::size_t n = f.name == "draw" ? 64 : 24;
        Rng rng(0x4B41540000ull ^ hashContiguousMemory(f.name.data(), f.name.size()));
        for (std::size_t i = 0; i < n; ++i) {
            write(std::string(f.name), f.generate(rng));
        }
    }
    return true;
}

bool emitCases(const std::string& path, std::uint64_t seed, std::size_t count) {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    std::fprintf(fp, "# fuse_relight_hash random cases: seed %s, %zu per function\n", hex64(seed).c_str(), count);
    for (const CaseFunction& f : caseFunctions()) {
        emitRandomCases(f, seed, count, [&](const std::string& line) {
            std::fwrite(line.data(), 1, line.size(), fp);
            std::fputc('\n', fp);
        });
    }
    return std::fclose(fp) == 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage: fuse_relight_hash_kat [--vectors F] [--kat F] [--self] [--digest-check F] [--digest]\n"
                 "                             [--emit F] [--emit-kat F] [--verify F] [--count N] [--seed S]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::size_t count = kDefaultCount;
    std::uint64_t seed = kDefaultSeed;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--count") {
            count = std::size_t(parseU64(args[i + 1], 10).value_or(kDefaultCount));
        } else if (args[i] == "--seed") {
            seed = parseU64(args[i + 1], 16).value_or(kDefaultSeed);
        }
    }
    if (args.empty()) {
        return usage();
    }
    if (args[0] == "--skip") {
        std::printf("SKIP: %s\n", args.size() > 1 ? args[1].c_str() : "not available");
        return 77;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--count" || a == "--seed") {
            ++i;
        } else if (a == "--vectors" && hasValue) {
            if (!runVectors(args[++i])) {
                return 2;
            }
        } else if ((a == "--kat" || a == "--verify") && hasValue) {
            if (!runCaseFile(args[++i], a == "--kat")) {
                return 2;
            }
        } else if (a == "--self") {
            runProperties();
            runSoftFloatChecks();
            runOracleCrossCheck(2000);
        } else if (a == "--digest-check" && hasValue) {
            if (!runDigestCheck(args[++i], seed, count)) {
                return 2;
            }
        } else if (a == "--digest") {
            std::fputs(formatDigests(computeDigests(seed, count), seed, count).c_str(), stdout);
        } else if (a == "--emit" && hasValue) {
            if (!emitCases(args[++i], seed, count)) {
                return 2;
            }
        } else if (a == "--emit-kat" && hasValue) {
            if (!emitKat(args[++i])) {
                return 2;
            }
        } else {
            return usage();
        }
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
