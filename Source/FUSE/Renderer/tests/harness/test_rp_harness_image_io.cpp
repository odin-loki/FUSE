// WP-0.7 renderer test harness, CPU half (no Vulkan; also runs in the stub backend):
//   * PNG: encoder -> decoder round trip (RGB and RGBA, noise and flat images, 1x1 .. 257x93), and a
//     zlib dynamic-Huffman PNG written by Python (all five row filters) decodes to the exact pixels;
//     corrupt input is rejected (bad CRC, truncated stream).
//     With the vendored stb_image available, it cross-checks the encoder as an independent decoder.
//   * EXR: FLOAT channels round trip bit-exactly through writeExr / readExr.
//   * Scene builders: deterministic (identical content hash on rebuild), expected sizes (the
//     instance grid scales 1k -> 100k at constant draw count), near-plane clipping keeps every
//     vertex in front of the camera.
//   * Golden gate failure path on CPU images: identical passes; a single changed pixel fails the
//     pixel budget even though PSNR stays above the threshold; tolerance and budget are honoured;
//     GoldenStore writes actual + diff artefacts on failure, reports a missing golden, and update mode
//     rewrites the golden and then passes.
#include "golden.hpp"
#include "image_io.hpp"
#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#if defined(FUSE_RP_HARNESS_HAS_STB)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#endif
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer::harness;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// Python zlib.compress(level 9) PNG, 16x8 RGBA, row filters y % 5 (dynamic Huffman block).
const u8 kPythonPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0xf0, 0x76, 0x7f,
    0x97, 0x00, 0x00, 0x00, 0xfe, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x8d, 0xd1, 0x21, 0x8e, 0xc3,
    0x30, 0x10, 0x05, 0xd0, 0x9f, 0xee, 0x02, 0x43, 0x43, 0x43, 0xc3, 0xc2, 0xc0, 0xc2, 0xc0, 0xc2,
    0xc0, 0xc2, 0xc0, 0xc0, 0xc0, 0xb2, 0x8c, 0x6f, 0xd0, 0x23, 0xcc, 0x11, 0x7a, 0x84, 0xc0, 0xc2,
    0x1e, 0xa1, 0xd0, 0xb0, 0xd0, 0x6c, 0xf6, 0x77, 0x93, 0x48, 0xd1, 0x4a, 0x55, 0x17, 0x44, 0xf3,
    0xf2, 0x35, 0x8a, 0xad, 0x1f, 0x00, 0x10, 0x0f, 0x67, 0x11, 0xde, 0x6a, 0x04, 0x69, 0x10, 0xad,
    0xc5, 0xde, 0x3a, 0xd4, 0x32, 0xe0, 0x60, 0x82, 0xc6, 0x2e, 0x38, 0x8a, 0xa2, 0xb5, 0x2b, 0x4e,
    0x36, 0xa1, 0x93, 0x3b, 0x7a, 0x7b, 0x60, 0xb0, 0x27, 0xce, 0x52, 0x21, 0x3a, 0xf3, 0x28, 0xf0,
    0x08, 0x89, 0x73, 0xe4, 0xa4, 0x0b, 0x1d, 0xc6, 0xff, 0xe4, 0x3b, 0x7e, 0x00, 0x88, 0x21, 0x21,
    0xe6, 0x11, 0xb1, 0xd0, 0x8e, 0x0e, 0x74, 0xa6, 0x0b, 0xed, 0xe8, 0x40, 0x67, 0xba, 0x8c, 0x7f,
    0xf7, 0xbf, 0xd0, 0xf8, 0xca, 0xf9, 0x32, 0x3a, 0x0f, 0x70, 0x26, 0xe7, 0x23, 0x5d, 0x68, 0xa4,
    0x39, 0x8f, 0x4b, 0x8e, 0x25, 0x8f, 0x69, 0xbb, 0xff, 0xfd, 0x7b, 0x02, 0x2f, 0xc4, 0xd7, 0xc4,
    0x87, 0xbe, 0x6d, 0xec, 0x16, 0xfb, 0x8d, 0x6f, 0x1b, 0xbf, 0x76, 0x74, 0x6f, 0x5e, 0xa3, 0x44,
    0x3d, 0x58, 0xad, 0xb5, 0x35, 0xea, 0xa4, 0x55, 0x58, 0xa7, 0xc1, 0x06, 0xf5, 0x22, 0xda, 0xdb,
    0x45, 0x3b, 0x53, 0x3d, 0xcb, 0x55, 0x07, 0x9b, 0xf4, 0x68, 0x77, 0x6d, 0xe4, 0xa1, 0x27, 0x7b,
    0x6a, 0x6b, 0x15, 0xa6, 0xfa, 0xf5, 0x17, 0x58, 0x4c, 0xe6, 0x3d, 0x1c, 0x8b, 0xc9, 0xb4, 0xa3,
    0x73, 0x9a, 0xf3, 0xfe, 0x4d, 0x3e, 0xef, 0xef, 0xe6, 0x92, 0xd6, 0xf2, 0xd6, 0x92, 0xd6, 0xf2,
    0x3e, 0xe7, 0x3f, 0x29, 0x5a, 0x8c, 0xf7, 0x27, 0x8e, 0x1f, 0xfa, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

ImageRgba8 noiseImage(u32 w, u32 h, u32 seed, bool alpha) {
    ImageRgba8 img(w, h);
    u32 state = seed * 747796405u + 2891336453u;
    for (usize i = 0; i < img.pixels.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        img.pixels[i] = static_cast<u8>(state >> 24);
        if (!alpha && i % 4u == 3u) {
            img.pixels[i] = 255u;
        }
    }
    return img;
}

void testPng() {
    for (const auto& dims : {std::pair<u32, u32>{1, 1}, {7, 3}, {64, 64}, {257, 93}}) {
        for (int alpha = 0; alpha < 2; ++alpha) {
            const ImageRgba8 img = noiseImage(dims.first, dims.second, dims.first * 31u + static_cast<u32>(alpha), alpha != 0);
            std::vector<u8> bytes;
            ImageRgba8 back;
            std::string error;
            const bool ok = encodePng(img, bytes) && decodePng(bytes.data(), bytes.size(), back, &error);
            expect(ok && back.width == img.width && back.height == img.height && back.pixels == img.pixels,
                   "PNG noise round trip is exact");
#if defined(FUSE_RP_HARNESS_HAS_STB)
            // Independent decoder: the vendored stb_image must read our encoder's output identically.
            int w = 0, h = 0, n = 0;
            unsigned char* stb = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
            expect(stb != nullptr && static_cast<u32>(w) == img.width && static_cast<u32>(h) == img.height &&
                       std::equal(img.pixels.begin(), img.pixels.end(), stb),
                   "stb_image decodes the harness PNG to the same pixels");
            stbi_image_free(stb);
#endif
        }
    }
    // Flat / gradient content must compress well (LZ77 + filters): 128x128 opaque gradient.
    ImageRgba8 flat(128, 128);
    for (u32 y = 0; y < 128u; ++y) {
        for (u32 x = 0; x < 128u; ++x) {
            u8* p = flat.at(x, y);
            p[0] = static_cast<u8>(x * 2u);
            p[1] = static_cast<u8>(y * 2u);
            p[2] = 40;
            p[3] = 255;
        }
    }
    std::vector<u8> bytes;
    ImageRgba8 back;
    expect(encodePng(flat, bytes) && decodePng(bytes.data(), bytes.size(), back) && back.pixels == flat.pixels,
           "PNG gradient round trip is exact");
    std::printf("PNG 128x128 gradient: %zu bytes (raw %u)\n", bytes.size(), 128u * 128u * 3u);
    expect(bytes.size() < 8192u, "gradient PNG compresses below 8 KiB");

    ImageRgba8 py;
    std::string error;
    const bool pyOk = decodePng(kPythonPng, sizeof(kPythonPng), py, &error);
    expect(pyOk, "Python zlib (dynamic Huffman) PNG decodes");
    bool match = pyOk && py.width == 16u && py.height == 8u;
    for (u32 y = 0; match && y < 8u; ++y) {
        for (u32 x = 0; x < 16u; ++x) {
            const u8* p = py.at(x, y);
            match = match && p[0] == static_cast<u8>(x * 16u) && p[1] == static_cast<u8>(y * 32u) &&
                    p[2] == static_cast<u8>((x ^ y) * 8u) && p[3] == ((x + y) % 3u != 0u ? 255u : 128u);
        }
    }
    expect(match, "Python PNG pixels (filters 0-4) match the generator");

    std::vector<u8> corrupt(kPythonPng, kPythonPng + sizeof(kPythonPng));
    corrupt[40] ^= 0x55u;
    expect(!decodePng(corrupt.data(), corrupt.size(), back), "PNG with a corrupted chunk is rejected (CRC)");
    expect(!decodePng(kPythonPng, 60, back), "truncated PNG is rejected");
}

void testExr(const std::string& dir) {
    std::vector<ExrChannel> channels(3);
    const char* names[] = {"Z", "B", "normal.X"};
    for (usize c = 0; c < 3u; ++c) {
        channels[c].name = names[c];
        channels[c].data.resize(5u * 3u);
        for (usize i = 0; i < channels[c].data.size(); ++i) {
            channels[c].data[i] = static_cast<float>(c) * 100.f + static_cast<float>(i) * 0.37f - 1.5f;
        }
    }
    channels[0].data[4] = 1e-30f;
    const std::string path = dir + "/roundtrip.exr";
    expect(writeExr(path, 5, 3, channels), "EXR written");
    u32 w = 0, h = 0;
    std::vector<ExrChannel> back;
    std::string error;
    const bool ok = readExr(path, w, h, back, &error);
    expect(ok && w == 5u && h == 3u && back.size() == 3u, "EXR read back");
    bool exact = ok && back.size() == 3u;
    for (const ExrChannel& orig : channels) {
        bool found = false;
        for (const ExrChannel& b : back) {
            if (b.name == orig.name) {
                found = true;
                exact = exact && b.data == orig.data;
            }
        }
        exact = exact && found;
    }
    expect(exact, "EXR FLOAT channels round trip bit-exactly (sorted on write)");
    expect(ok && back[0].name == "B" && back[1].name == "Z" && back[2].name == "normal.X",
           "EXR channel list sorted by name");
}

void testScenes() {
    for (const std::string& name : sceneNames()) {
        Scene a;
        Scene b;
        const bool ok = buildSceneByName(name, a) && buildSceneByName(name, b);
        expect(ok, "scene builds by name");
        expect(ok && a.contentHash() == b.contentHash(), "scene content hash is stable across rebuilds");
        expect(ok && a.triangleCount() > 0u, "scene has triangles");
        const ProjectedScene p = projectScene(a, 1.f);
        bool inFront = true;
        for (usize i = 2; i < p.vertices.size(); i += 3u) {
            inFront = inFront && p.vertices[i] >= 0.f && p.vertices[i] <= 1.f && std::isfinite(p.vertices[i - 1]) &&
                      std::isfinite(p.vertices[i - 2]);
        }
        expect(inFront, "projected vertices have finite x/y and NDC depth in [0, 1]");
        std::printf("scene %-20s batches %4zu triangles %8llu hash %016llx\n", name.c_str(), a.batches.size(),
                    static_cast<unsigned long long>(a.triangleCount()), static_cast<unsigned long long>(a.contentHash()));
    }
    const Scene g1 = buildInstanceGrid(1000);
    const Scene g100 = buildInstanceGrid(100000);
    expect(g1.instances == 1000u && g100.instances == 100000u, "instance grid instance counts");
    expect(g100.triangleCount() == 1u * 2u + 100000u * 10u, "100k grid: 5 faces x 2 triangles per cube + ground");
    expect(g1.batches.size() <= 21u && g100.batches.size() <= 21u, "instance grid draw count independent of N");
    Scene bad;
    expect(!buildSceneByName("instance_grid_", bad) && !buildSceneByName("nope", bad), "unknown scene names rejected");

    // Near-plane clipping: a quad straddling the camera plane keeps only the visible part.
    Scene clip;
    clip.camera.eye = {0.f, 0.f, 0.f};
    clip.camera.target = {0.f, 0.f, -1.f};
    clip.materials.push_back({});
    Batch batch;
    batch.positions = {{-1.f, -1.f, 1.f}, {1.f, -1.f, -3.f}, {-1.f, 1.f, -3.f}};
    clip.batches.push_back(batch);
    const ProjectedScene p = projectScene(clip, 1.f);
    expect(p.vertexCount[0] == 6u, "triangle crossing the near plane clips to a quad (2 triangles)");
}

void testGoldenGate(const std::string& dir) {
    ImageRgba8 golden = noiseImage(64, 48, 5u, false);
    GoldenSpec spec{};
    spec.metric = GoldenMetric::Psnr;
    spec.threshold = 40.0;

    const GoldenResult same = compareImages(golden, golden, spec);
    expect(same.passed && same.differingPixels == 0u && std::isinf(same.psnr), "identical image passes");

    ImageRgba8 onePixel = golden;
    onePixel.at(17, 9)[1] = static_cast<u8>(onePixel.at(17, 9)[1] ^ 0x80u);
    const GoldenResult changed = compareImages(onePixel, golden, spec);
    std::printf("one-pixel change: %s\n", changed.message.c_str());
    expect(changed.psnr > spec.threshold, "a one-pixel change keeps PSNR above the threshold (metric alone misses it)");
    expect(!changed.passed && changed.differingPixels == 1u, "one-pixel change fails the gate via the pixel budget");

    GoldenSpec lenient = spec;
    lenient.maxDifferingPixels = 1;
    expect(compareImages(onePixel, golden, lenient).passed, "pixel budget of 1 admits a one-pixel change");

    ImageRgba8 nudged = golden;
    nudged.at(3, 3)[0] = static_cast<u8>(nudged.at(3, 3)[0] < 128u ? nudged.at(3, 3)[0] + 2u : nudged.at(3, 3)[0] - 2u);
    expect(compareImages(nudged, golden, spec).passed, "a change within the per-channel tolerance passes");

    ImageRgba8 wrongSize = noiseImage(64, 47, 5u, false);
    expect(!compareImages(wrongSize, golden, spec).passed, "size mismatch fails");

    ImageRgba8 noisy = golden;
    for (usize i = 0; i < noisy.pixels.size(); i += 4u) {
        noisy.pixels[i] = static_cast<u8>(noisy.pixels[i] ^ 0x3Fu);
    }
    GoldenSpec anyPixels = spec;
    anyPixels.maxDifferingPixels = 1u << 30;
    const GoldenResult metricFail = compareImages(noisy, golden, anyPixels);
    expect(!metricFail.passed && metricFail.psnr < 40.0, "a large change fails on the metric alone");

    // GoldenStore: missing golden, update mode, pass, failure artefacts.
    const std::string goldenDir = dir + "/golden";
    const std::string artifactDir = dir + "/artifacts";
    std::filesystem::remove_all(goldenDir);
    std::filesystem::remove_all(artifactDir);
    GoldenStore store(goldenDir, artifactDir);
    store.setUpdateMode(false);
    const GoldenResult missing = store.check("unit", golden, spec);
    expect(!missing.passed && missing.goldenMissing, "missing golden fails outside update mode");
    store.setUpdateMode(true);
    const GoldenResult created = store.check("unit", golden, spec);
    expect(created.passed && created.updated && std::filesystem::exists(goldenDir + "/unit.png"),
           "update mode writes the golden");
    const GoldenResult unchanged = store.check("unit", golden, spec);
    expect(unchanged.passed && !unchanged.updated, "update mode leaves an identical golden untouched");
    store.setUpdateMode(false);
    expect(store.check("unit", golden, spec).passed, "stored golden round trips (PNG) and passes");
    const GoldenResult fail = store.check("unit", onePixel, spec);
    expect(!fail.passed && !fail.diffPath.empty() && std::filesystem::exists(fail.diffPath) &&
               std::filesystem::exists(fail.actualPath),
           "failed check writes actual + diff artefacts");
    ImageRgba8 diff;
    expect(readPng(fail.diffPath, diff) && diff.at(17, 9)[0] >= 128u && diff.at(17, 9)[1] == 0u,
           "diff image marks the changed pixel in red");
    store.printSummary();
}

} // namespace

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "rp_harness_image_io_out";
    ensureDirectory(dir);
    std::printf("quality metrics library: %s\n", goldenQualityMetricsAvailable() ? "linked (SSIM/FLIP)" : "absent (PSNR fallback)");
    testPng();
    testExr(dir);
    testScenes();
    testGoldenGate(dir);
    if (g_failures == 0) {
        std::printf("rp_harness_image_io: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "rp_harness_image_io: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
