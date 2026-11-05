// FUSE Remaster W1.5 gate (remaster_dif): a synthetic interior (floor slab, wall, pillar) written with the
// fuse_legacy_dif writer and read back with the reader:
//   * brush count and triangle area match the analytic values (relative error <= 1e-4);
//   * lightmaps are ignored: the same model written with and without dummy lightmaps (and a preview bitmap)
//     reads back to the same geometry;
//   * the writer is byte-deterministic (two writes identical; the fixture regenerates byte-identically);
//   * every interior version 0..14 round-trips (the version-dependent field widths);
//   * robustness: every truncation, bad resource / interior versions, out-of-range indices and 1,000 seeded
//     random byte mutations never crash; truncations and bad versions are errors; whatever is accepted validates.
// The fixture is generated in a temp directory at test time (no binary fixture is committed).
#include "../../tests/check.hpp"

#include <fuse/remaster/dif/dif.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace dif = fuse::remaster::dif;
namespace fs = std::filesystem;

namespace {

double boxArea(dif::Vec3 a, dif::Vec3 b) {
    const double x = double(b.x) - a.x, y = double(b.y) - a.y, z = double(b.z) - a.z;
    return 2.0 * (x * y + y * z + z * x);
}

dif::InteriorResource fixture(std::uint32_t version = dif::kMaxInteriorVersion) {
    dif::Interior it;
    it.version = version;
    addBoxBrush(it, {-5.f, -5.f, -0.5f}, {5.f, 5.f, 0.f}, "fuse/fixture/floor");     // floor slab 10 x 10 x 0.5
    addBoxBrush(it, {-5.f, 4.5f, 0.f}, {5.f, 5.f, 3.f}, "fuse/fixture/wall");        // wall 10 x 0.5 x 3
    addBoxBrush(it, {-0.5f, -0.5f, 0.f}, {0.5f, 0.5f, 3.f}, "fuse/fixture/wall");    // pillar 1 x 1 x 3
    dif::InteriorResource r;
    r.detailLevels.push_back(it);
    dif::Interior lod; // a coarser detail level: one block
    lod.version = version;
    lod.detailLevel = 1;
    lod.minPixels = 64;
    addBoxBrush(lod, {-5.f, -5.f, -0.5f}, {5.f, 5.f, 3.f}, "fuse/fixture/floor");
    r.detailLevels.push_back(lod);
    return r;
}

double expectedArea0() {
    return boxArea({-5, -5, -0.5f}, {5, 5, 0}) + boxArea({-5, 4.5f, 0}, {5, 5, 3}) + boxArea({-0.5f, -0.5f, 0}, {0.5f, 0.5f, 3});
}

dif::Interior geometryOnly(dif::Interior it) {
    it.lightmapsSkipped = 0;
    return it;
}

void testRoundTrip(const fs::path& dir) {
    const dif::InteriorResource src = fixture();
    const std::vector<std::uint8_t> bytes = dif::writeDif(src);
    CHECK(!bytes.empty());
    CHECK(bytes == dif::writeDif(fixture())); // byte-deterministic regeneration
    {
        std::ofstream out(dir / "fixture_interior.dif", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::ifstream in(dir / "fixture_interior.dif", std::ios::binary);
    const std::vector<std::uint8_t> onDisk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(onDisk == bytes);

    const dif::ReadResult r = dif::readDif(onDisk);
    CHECK(r.ok);
    if (!r.ok) {
        std::fprintf(stderr, "read error: %s\n", r.error.c_str());
        return;
    }
    CHECK(r.warnings.empty());
    CHECK(r.resource.version == dif::kResourceVersion);
    CHECK(r.resource.detailLevels.size() == 2u);
    const dif::Interior& l0 = r.resource.detailLevels[0];
    CHECK(l0 == src.detailLevels[0]);
    CHECK(dif::brushCount(l0) == 3u);
    CHECK(l0.surfaces.size() == 18u && l0.materials.size() == 2u);
    CHECK(dif::triangulate(l0).size() == 36u);
    const double a0 = dif::totalTriangleArea(l0);
    CHECK(std::fabs(a0 - expectedArea0()) <= 1e-4 * expectedArea0());
    CHECK(std::fabs(expectedArea0() - 307.0) < 1e-9);
    const dif::Interior& l1 = r.resource.detailLevels[1];
    CHECK(dif::brushCount(l1) == 1u && l1.detailLevel == 1u && l1.minPixels == 64u);
    CHECK(std::fabs(dif::totalTriangleArea(l1) - boxArea({-5, -5, -0.5f}, {5, 5, 3})) <= 1e-4 * 340.0);
    // Planes face outwards: every surface's plane holds its own points (n . p + d == 0).
    for (const dif::Surface& s : l0.surfaces) {
        const dif::Plane& p = l0.planes[s.planeIndex];
        const dif::Vec3& n = l0.normals[p.normalIndex];
        for (std::uint32_t k = 0; k < s.windingCount; ++k) {
            const dif::Vec3& q = l0.points[l0.windings[s.windingStart + k]];
            CHECK(std::fabs(n.x * q.x + n.y * q.y + n.z * q.z + p.distance) < 1e-5f);
        }
    }
    const std::string summary = dif::summaryJson(r.resource);
    CHECK(summary.find("\"brushes\": 3") != std::string::npos);
    CHECK(summary.find("\"area\": 307.000000") != std::string::npos);
    CHECK(summary == dif::summaryJson(r.resource));
    std::printf("%s", summary.c_str());
}

void testLightmapsIgnored() {
    const std::vector<std::uint8_t> plain = dif::writeDif(fixture());
    dif::WriteOptions opt;
    opt.dummyLightmaps = 3;
    opt.preview = true;
    const std::vector<std::uint8_t> lit = dif::writeDif(fixture(), opt);
    CHECK(lit.size() > plain.size());
    CHECK(lit == dif::writeDif(fixture(), opt));
    const dif::ReadResult a = dif::readDif(plain), b = dif::readDif(lit);
    CHECK(a.ok && b.ok);
    if (!a.ok || !b.ok) {
        std::fprintf(stderr, "lightmap read: %s / %s\n", a.error.c_str(), b.error.c_str());
        return;
    }
    CHECK(b.resource.detailLevels[0].lightmapsSkipped == 3u && a.resource.detailLevels[0].lightmapsSkipped == 0u);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(geometryOnly(a.resource.detailLevels[i]) == geometryOnly(b.resource.detailLevels[i]));
    }
    CHECK(dif::totalTriangleArea(b.resource.detailLevels[0]) == dif::totalTriangleArea(a.resource.detailLevels[0]));
}

void testVersions() {
    for (std::uint32_t v = 0; v <= dif::kMaxInteriorVersion; ++v) {
        dif::WriteOptions opt;
        opt.dummyLightmaps = 2;
        const std::vector<std::uint8_t> bytes = dif::writeDif(fixture(v), opt);
        const dif::ReadResult r = dif::readDif(bytes);
        CHECK(r.ok && r.resource.detailLevels.size() == 2u);
        if (r.ok) {
            CHECK(geometryOnly(r.resource.detailLevels[0]) == fixture(v).detailLevels[0]);
            CHECK(dif::brushCount(r.resource.detailLevels[0]) == 3u);
        } else {
            std::fprintf(stderr, "version %u: %s\n", v, r.error.c_str());
        }
    }
}

void testRobustness() {
    dif::WriteOptions opt;
    opt.dummyLightmaps = 2;
    const std::vector<std::uint8_t> good = dif::writeDif(fixture(), opt);
    // Every truncation inside the detail levels (strided past the first 512 bytes) is an error, never a crash.
    // The 5 trailing game-object counts (20 bytes) are read leniently: cutting into them keeps the geometry.
    const std::size_t geometryEnd = good.size() - 20u;
    std::size_t truncations = 0;
    for (std::size_t n = 0; n < geometryEnd; n += (n < 512 ? 1 : 7)) {
        const dif::ReadResult r = dif::readDif(std::span(good.data(), n));
        CHECK(!r.ok && !r.error.empty());
        ++truncations;
    }
    for (std::size_t n = geometryEnd; n < good.size(); ++n) {
        const dif::ReadResult r = dif::readDif(std::span(good.data(), n));
        CHECK(r.ok && r.resource.detailLevels.size() == 2u && dif::brushCount(r.resource.detailLevels[0]) == 3u);
    }
    // Bad resource version / interior version.
    std::vector<std::uint8_t> bad = good;
    bad[0] = 45;
    CHECK(!dif::readDif(bad).ok);
    bad = good;
    bad[9] = 15; // first interior version (offset 4 version + 1 preview flag + 4 level count)
    const dif::ReadResult badInterior = dif::readDif(bad);
    CHECK(!badInterior.ok && badInterior.error.find("interior version") != std::string::npos);
    // Out-of-range indices are refused by the writer and by the reader.
    dif::InteriorResource broken = fixture();
    broken.detailLevels[0].windings[0] = 999;
    CHECK(dif::writeDif(broken).empty());
    CHECK(!dif::validate(broken.detailLevels[0]).empty());
    // Seeded random mutations (xorshift32): never crash; anything accepted validates and triangulates.
    std::uint32_t state = 0x2545F491u;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    std::size_t accepted = 0, rejected = 0;
    for (int i = 0; i < 1000; ++i) {
        std::vector<std::uint8_t> m = good;
        const int flips = 1 + static_cast<int>(next() % 4u);
        for (int f = 0; f < flips; ++f) {
            m[next() % m.size()] ^= static_cast<std::uint8_t>(1u + next() % 255u);
        }
        const dif::ReadResult r = dif::readDif(m);
        if (r.ok) {
            ++accepted;
            for (const dif::Interior& it : r.resource.detailLevels) {
                CHECK(dif::validate(it).empty());
                (void)dif::totalTriangleArea(it);
            }
            (void)dif::summaryJson(r.resource);
        } else {
            ++rejected;
            CHECK(!r.error.empty());
        }
    }
    std::printf("robustness: %zu truncations rejected; 1000 mutations: %zu rejected, %zu accepted (validated)\n",
                truncations, rejected, accepted);
}

} // namespace

int main(int argc, char** argv) {
    // `--write-fixture <path>`: writes the synthetic fixture (with dummy lightmaps) for the CLI gate and exits.
    if (argc == 3 && std::string(argv[1]) == "--write-fixture") {
        dif::WriteOptions opt;
        opt.dummyLightmaps = 3;
        const std::vector<std::uint8_t> bytes = dif::writeDif(fixture(), opt);
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return out && !bytes.empty() ? 0 : 1;
    }
    const fs::path dir = fs::temp_directory_path() / "fuse_remaster_dif_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    testRoundTrip(dir);
    testLightmapsIgnored();
    testVersions();
    testRobustness();
    fs::remove_all(dir, ec);
    return remaster_test::finish("remaster_dif");
}
