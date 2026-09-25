// FUSE Relight RL-5.6 terrain baking gates (terrain_baker.hpp). Suites (argv[1]; "all" runs every one):
//
//   golden      a synthetic terrain (hilly grid with a checker base texture, an alpha-blended road decal, a MODULATE2X
//               lightmap, an additive highlight and an alpha-tested grass layer) baked to 96 x 64: the RGBA8-quantised
//               content hash equals the golden (the same value in every tree and on Wine; float operations only
//               + - * / floor, no contraction), plus spot checks of the layers' effects.
//   blend       every blend mode against its closed form over a flat textured quad.
//   raster      a quad split into two triangles covers every texel centre exactly once (no gap, no double blend of an
//               alpha layer); the top-down depth test keeps the highest surface of a self-overlapping layer; the world
//               lookup at texel centres returns the texels.
//   cache       unchanged inputs are not re-baked; a changed texel / transform / vertex re-bakes.
//   zero_alloc  steady-state bakes (same sizes) make no heap allocation.
#include "terrain_test_scene.hpp"

#include <fuse/relight/terrain/terrain_baker.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<bool> g_count{false};
std::atomic<unsigned long long> g_allocations{0};
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (g_count.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace terrain_test;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}


void suiteGolden() {
    Terrain t;
    makeTerrain(t);
    tb::TerrainBaker baker;
    std::vector<float> out;
    check(baker.bake(t.desc, t.layers, out), "golden: bake");
    const u64 h = hashRgba8(out, std::size_t(t.desc.width) * t.desc.height);
    std::printf("golden: 96 x 64 bake of 5 layers: RGBA8 hash 0x%016llx (golden 0x%016llx), %llu texels written\n",
                (unsigned long long)h, (unsigned long long)kGolden, (unsigned long long)baker.stats().texelsWritten);
    check(h == kGolden, "golden: baked texture == golden");
    // Spot checks: the road darkens the centre line, the highlight brightens its square, the lightmap scales.
    float road[4], off[4], hi[4], ref[4];
    tb::terrainLookup(t.desc, out, 2.2f, 0.05f, road);
    tb::terrainLookup(t.desc, out, 2.2f, -2.5f, off);
    tb::terrainLookup(t.desc, out, 2.f, 2.f, hi);
    tb::terrainLookup(t.desc, out, 4.f, 2.f, ref);
    std::printf("golden: road %.3f %.3f %.3f, grass %.3f %.3f %.3f, highlight %.3f vs %.3f\n", road[0], road[1],
                road[2], off[0], off[1], off[2], hi[0], ref[0]);
    check(std::fabs(road[0] - road[1]) < std::fabs(off[0] - off[1]), "golden: the road decal is visible (grey)");
    check(hi[0] > ref[0] + 0.5f, "golden: the additive highlight is visible");
}

void suiteBlend() {
    const float c0[4] = {0.8f, 0.4f, 0.2f, 0.6f};
    std::vector<float> tex(4u * 4u * 4u);
    for (u32 i = 0; i < 16u; ++i) {
        std::memcpy(&tex[i * 4u], c0, sizeof(c0));
    }
    const tb::TerrainTexture t{4, 4, tex.data()};
    auto flat = [](float, float) { return 0.f; };
    const Mesh quad = grid(0.f, 0.f, 1.f, 1.f, 1, 1, 1.f, flat, false);
    tb::TerrainBakeDesc d;
    d.width = 8;
    d.height = 8;
    d.clear[0] = 0.5f;
    d.clear[1] = 0.25f;
    d.clear[2] = 1.f;
    d.clear[3] = 1.f;
    struct Case {
        tb::TerrainBlend mode;
        float expect[3];
    };
    const float* c = d.clear;
    const Case cases[] = {
        {tb::TerrainBlend::Opaque, {c0[0], c0[1], c0[2]}},
        {tb::TerrainBlend::Alpha,
         {c0[0] * c0[3] + c[0] * (1.f - c0[3]), c0[1] * c0[3] + c[1] * (1.f - c0[3]), c0[2] * c0[3] + c[2] * (1.f - c0[3])}},
        {tb::TerrainBlend::Additive, {c[0] + c0[0] * c0[3], c[1] + c0[1] * c0[3], c[2] + c0[2] * c0[3]}},
        {tb::TerrainBlend::Multiply, {c[0] * c0[0], c[1] * c0[1], c[2] * c0[2]}},
        {tb::TerrainBlend::Multiply2x, {2.f * c[0] * c0[0], 2.f * c[1] * c0[1], 2.f * c[2] * c0[2]}}};
    double worst = 0.0;
    tb::TerrainBaker baker;
    std::vector<float> out;
    for (const Case& k : cases) {
        const tb::TerrainLayer L = layerOf(quad, &t, k.mode);
        baker.bake(d, std::span<const tb::TerrainLayer>(&L, 1u), out);
        for (u32 i = 0; i < 64u; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                worst = std::max(worst, double(std::fabs(out[i * 4u + ch] - k.expect[ch])));
            }
        }
    }
    std::printf("blend: 5 modes x 64 texels: max error vs closed form %.2e\n", worst);
    check(worst <= 1e-6, "blend: every mode == closed form");
}

void suiteRaster() {
    auto flat = [](float, float) { return 0.f; };
    const Mesh quad = grid(-1.f, -1.f, 1.f, 1.f, 3, 5, 1.f, flat, false); // 30 triangles, diagonal edges
    tb::TerrainBakeDesc d;
    d.minX = -1.f;
    d.minZ = -1.f;
    d.maxX = 1.f;
    d.maxZ = 1.f;
    d.width = 60;
    d.height = 60;
    tb::TerrainLayer L = layerOf(quad, nullptr, tb::TerrainBlend::Additive);
    L.colors = nullptr;
    tb::TerrainBaker baker;
    std::vector<float> out;
    baker.bake(d, std::span<const tb::TerrainLayer>(&L, 1u), out);
    u32 once = 0;
    for (u32 i = 0; i < 3600u; ++i) {
        once += out[i * 4u] == 1.f ? 1u : 0u;
    }
    std::printf("raster: additive white over a 30-triangle split quad: %u / 3600 texels exactly once\n", once);
    check(once == 3600u, "raster: every texel centre covered exactly once (no gap, no double blend)");
    // Depth test: two overlapping sheets in one layer, the upper one (red) wins.
    Mesh sheets;
    const Mesh lo = grid(-1.f, -1.f, 1.f, 1.f, 1, 1, 1.f, [](float, float) { return 0.f; }, false);
    const Mesh up = grid(-0.5f, -0.5f, 0.5f, 0.5f, 1, 1, 1.f, [](float, float) { return 1.f; }, false);
    sheets.positions = lo.positions;
    sheets.positions.insert(sheets.positions.end(), up.positions.begin(), up.positions.end());
    sheets.uvs = lo.uvs;
    sheets.uvs.insert(sheets.uvs.end(), up.uvs.begin(), up.uvs.end());
    sheets.indices = up.indices;
    for (u32& i : sheets.indices) {
        i += 4u;
    }
    sheets.indices.insert(sheets.indices.end(), lo.indices.begin(), lo.indices.end()); // upper sheet first
    for (u32 v = 0; v < 8u; ++v) {
        const float col[4] = {v >= 4u ? 1.f : 0.f, v >= 4u ? 0.f : 1.f, 0.f, 1.f};
        sheets.colors.insert(sheets.colors.end(), col, col + 4);
    }
    const tb::TerrainLayer S = layerOf(sheets, nullptr, tb::TerrainBlend::Opaque);
    baker.bake(d, std::span<const tb::TerrainLayer>(&S, 1u), out);
    float centre[4], corner[4];
    tb::terrainLookup(d, out, 0.f, 0.f, centre);
    tb::terrainLookup(d, out, -0.9f, -0.9f, corner);
    check(centre[0] == 1.f && centre[1] == 0.f && corner[0] == 0.f && corner[1] == 1.f,
          "raster: top-down depth test keeps the highest surface");
    // Lookup at texel centres == texels.
    bool lookup = true;
    for (u32 z = 0; z < d.height; z += 7u) {
        for (u32 x = 0; x < d.width; x += 5u) {
            float rgba[4];
            tb::terrainLookup(d, out, -1.f + (x + 0.5f) * 2.f / d.width, -1.f + (z + 0.5f) * 2.f / d.height, rgba);
            for (int k = 0; k < 4; ++k) {
                lookup = lookup && std::fabs(rgba[k] - out[(z * d.width + x) * 4u + k]) <= 1e-6f;
            }
        }
    }
    std::printf("raster: depth test centre %.1f/%.1f corner %.1f/%.1f; lookup at texel centres %s\n", centre[0],
                centre[1], corner[0], corner[1], lookup ? "== texels" : "DIFFERS");
    check(lookup, "raster: lookup at texel centres == the texels");
}

void suiteCache() {
    Terrain t;
    makeTerrain(t);
    tb::TerrainBaker baker;
    std::vector<float> out;
    const bool first = baker.bakeCached(t.desc, t.layers, out);
    const bool second = baker.bakeCached(t.desc, t.layers, out);
    t.texRoad[5] += 0.25f;
    const bool texel = baker.bakeCached(t.desc, t.layers, out);
    t.layers[3].uvTransform[2] = 0.25f;
    const bool transform = baker.bakeCached(t.desc, t.layers, out);
    t.ground.positions[1] += 0.1f;
    const bool vertex = baker.bakeCached(t.desc, t.layers, out);
    const bool again = baker.bakeCached(t.desc, t.layers, out);
    std::printf("cache: first %d, unchanged %d, texel %d, transform %d, vertex %d, unchanged %d (bakes %u, hits %u)\n",
                first, second, texel, transform, vertex, again, baker.stats().bakes, baker.stats().cacheHits);
    check(first && !second && texel && transform && vertex && !again, "cache: re-bakes exactly when the inputs change");
}

void suiteZeroAlloc() {
    Terrain t;
    makeTerrain(t);
    tb::TerrainBaker baker;
    std::vector<float> out;
    baker.bake(t.desc, t.layers, out);
    g_allocations = 0;
    g_count = true;
    for (u32 k = 0; k < 16u; ++k) {
        t.layers[3].uvTransform[2] = 0.01f * float(k);
        baker.bakeCached(t.desc, t.layers, out);
    }
    g_count = false;
    std::printf("zero_alloc: 16 steady-state re-bakes: %llu operator-new calls\n",
                (unsigned long long)g_allocations.load());
    check(g_allocations.load() == 0u, "zero_alloc: no heap allocation");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"golden", suiteGolden}, {"blend", suiteBlend},           {"raster", suiteRaster},
                            {"cache", suiteCache},   {"zero_alloc", suiteZeroAlloc}};
    bool ran = false;
    for (const Suite& s : suites) {
        if (suite == "all" || suite == s.name) {
            s.fn();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s) (%s)\n", g_failures, suite.c_str());
        return 1;
    }
    std::printf("PASS: rl_terrain %s\n", suite.c_str());
    return 0;
}
