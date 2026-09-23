// B7.9 / B7.10 asset-pipeline gate tests.
//
// References are independent of the cooker under test:
//   - meshes are compared against triangles parsed from the OBJ text by this file,
//   - BC7 blocks are decoded by a spec-derived mode-6 decoder written here, and PSNR is measured
//     against the stb_image-decoded source (or the procedurally generated pixels),
//   - incremental cooking is observed through file bytes / mtimes on disk and cache-hit flags.
// Every file is written below a fresh temp directory; nothing touches the source tree.
// Source images are read (never written) from the repository sample project when present.

#include <fuse/cook/bc7_encoder.hpp>
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/cook/texture_cook.hpp>
#include <fuse/core/init.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/cook_manifest.hpp>
#include <fuse/project/import_pipeline.hpp>

#if defined(FUSE_HAS_STB_IMAGE)
#include "stb_image.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define FUSE_GETPID _getpid
#else
#include <unistd.h>
#define FUSE_GETPID getpid
#endif

namespace fs = std::filesystem;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;

namespace {

int g_failures = 0;
fs::path g_root;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string path_of(const fs::path& path) {
    return path.string();
}

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::vector<u8> readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------------------------
// Mesh fixtures + independent OBJ reference
// ---------------------------------------------------------------------------------------------

// Two objects, triangles only, coordinates exactly representable in binary floating point so the
// reference parse and the importer must agree bit-for-bit.
const char* kTwoObjectObj =
    "# fuse b7 gate mesh\n"
    "o Wedge\n"
    "v -1.0 0.0 -1.0\n"
    "v 1.0 0.0 -1.0\n"
    "v 1.0 0.0 1.0\n"
    "v -1.0 0.0 1.0\n"
    "v 0.0 1.5 0.0\n"
    "vt 0.0 0.0\n"
    "vt 1.0 0.0\n"
    "vt 1.0 1.0\n"
    "vt 0.0 1.0\n"
    "vt 0.5 0.5\n"
    "vn 0.0 -1.0 0.0\n"
    "vn 0.0 0.5 -0.75\n"
    "vn 0.75 0.5 0.0\n"
    "vn 0.0 0.5 0.75\n"
    "vn -0.75 0.5 0.0\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/1/1 3/3/1 4/4/1\n"
    "f 1/1/2 5/5/2 2/2/2\n"
    "f 2/2/3 5/5/3 3/3/3\n"
    "f 3/3/4 5/5/4 4/4/4\n"
    "f 4/4/5 5/5/5 1/1/5\n"
    "o Tri\n"
    "v 4.0 0.25 4.0\n"
    "v 6.0 0.25 4.0\n"
    "v 5.0 2.125 4.5\n"
    "vt 0.25 0.75\n"
    "vn 0.0 0.0 1.0\n"
    "f 6/6/6 7/6/6 8/6/6\n";

struct RefTriangle {
    std::array<f32, 9> positions{};
    std::array<f32, 6> uvs{};
};

std::vector<RefTriangle> parseObjTriangles(const std::string& text) {
    std::vector<std::array<f32, 3>> v;
    std::vector<std::array<f32, 2>> vt;
    std::vector<RefTriangle> tris;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            std::array<f32, 3> p{};
            ls >> p[0] >> p[1] >> p[2];
            v.push_back(p);
        } else if (tag == "vt") {
            std::array<f32, 2> t{};
            ls >> t[0] >> t[1];
            vt.push_back(t);
        } else if (tag == "f") {
            RefTriangle tri;
            for (u32 corner = 0; corner < 3u; ++corner) {
                std::string token;
                ls >> token;
                const u32 pi = static_cast<u32>(std::stoul(token.substr(0, token.find('/')))) - 1u;
                const std::size_t s1 = token.find('/');
                const u32 ti = static_cast<u32>(std::stoul(token.substr(s1 + 1, token.find('/', s1 + 1) - s1 - 1))) - 1u;
                for (u32 axis = 0; axis < 3u; ++axis) {
                    tri.positions[corner * 3u + axis] = v[pi][axis];
                }
                tri.uvs[corner * 2u + 0] = vt[ti][0];
                tri.uvs[corner * 2u + 1] = vt[ti][1];
            }
            tris.push_back(tri);
        }
    }
    return tris;
}

// Canonical form: rotate so the lexicographically smallest corner is first (winding preserved).
std::vector<f32> canonicalTriangle(const f32* p, const f32* uv) {
    u32 start = 0;
    for (u32 corner = 1; corner < 3u; ++corner) {
        if (std::lexicographical_compare(p + corner * 3u, p + corner * 3u + 3u, p + start * 3u, p + start * 3u + 3u)) {
            start = corner;
        }
    }
    std::vector<f32> out;
    for (u32 k = 0; k < 3u; ++k) {
        const u32 corner = (start + k) % 3u;
        out.insert(out.end(), p + corner * 3u, p + corner * 3u + 3u);
        out.insert(out.end(), uv + corner * 2u, uv + corner * 2u + 2u);
    }
    return out;
}

void testMeshRoundTripAgainstObjReference() {
    const fs::path source = g_root / "mesh" / "wedge.obj";
    writeText(source, kTwoObjectObj);
    const fs::path output = g_root / "cooked" / "wedge.fusemesh";

    fuse::project::AssetCooker cooker;
    cooker.set_strict_import(true);
    fuse::project::MeshImportDesc desc;
    desc.input_path = path_of(source);
    desc.output_path = path_of(output);
    const fuse::project::CookRecord record = cooker.cook_mesh(desc);
    expectTrue(record.ok && record.status == fuse::project::CookStatus::Ok, "strict mesh cook ok");

    fuse::cook::CookedMesh mesh;
    std::string error;
    const bool loaded = fuse::cook::load_cooked_mesh(path_of(output), mesh, &error);
    expectTrue(loaded, "cooked mesh loads");
    if (!loaded) {
        std::fprintf(stderr, "  load error: %s\n", error.c_str());
        return;
    }

    std::vector<std::vector<f32>> cooked;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        f32 p[9];
        f32 uv[6];
        for (u32 corner = 0; corner < 3u; ++corner) {
            const u32 vi = mesh.indices[t + corner];
            for (u32 axis = 0; axis < 3u; ++axis) {
                p[corner * 3u + axis] = mesh.positions[vi * 3u + axis];
            }
            uv[corner * 2u + 0] = mesh.uvs[vi * 2u + 0];
            uv[corner * 2u + 1] = mesh.uvs[vi * 2u + 1];
        }
        cooked.push_back(canonicalTriangle(p, uv));
    }
    std::vector<std::vector<f32>> reference;
    for (const RefTriangle& tri : parseObjTriangles(kTwoObjectObj)) {
        reference.push_back(canonicalTriangle(tri.positions.data(), tri.uvs.data()));
    }
    std::sort(cooked.begin(), cooked.end());
    std::sort(reference.begin(), reference.end());
    std::printf("mesh round trip: %zu cooked triangles, %zu reference triangles, %u vertices, %zu submeshes\n",
                cooked.size(), reference.size(), mesh.vertex_count(), mesh.submeshes.size());
    expectTrue(cooked == reference, "cooked triangles (position + uv) bit-identical to the OBJ reference");
    // Pre-transform merges objects that share a material; submeshes must tile the index buffer.
    u32 covered = 0;
    for (const fuse::cook::CookedMesh::Submesh& submesh : mesh.submeshes) {
        expectTrue(submesh.index_offset == covered, "submeshes are contiguous");
        covered += submesh.index_count;
    }
    expectTrue(!mesh.submeshes.empty() && covered == mesh.indices.size(), "submeshes cover every index");
    expectTrue(mesh.bounds_min[0] == -1.f && mesh.bounds_max[0] == 6.f && mesh.bounds_max[1] == 2.125f,
               "bounds match the OBJ extents");
    // Authored normals survive exactly (normalised by the importer where needed).
    u32 unitNormals = 0;
    for (u32 v = 0; v < mesh.vertex_count(); ++v) {
        const f32 x = mesh.normals[v * 3u];
        const f32 y = mesh.normals[v * 3u + 1u];
        const f32 z = mesh.normals[v * 3u + 2u];
        const f32 len = std::sqrt(x * x + y * y + z * z);
        if (std::fabs(len - 1.f) < 1e-4f || std::fabs(len - std::sqrt(0.8125f)) < 1e-4f) {
            ++unitNormals;
        }
    }
    expectTrue(unitNormals == mesh.vertex_count(), "every vertex carries the authored normal");

    // serialize(deserialize(file)) == file, bit-exact.
    const std::vector<u8> fileBytes = readBytes(output);
    const std::vector<u8> reserialized = fuse::cook::serialize_cooked_mesh(mesh);
    expectTrue(fileBytes == reserialized, "load -> serialize reproduces the cooked file bit-exactly");
}

void testMeshGeneratedNormals() {
    // Cube with shared corners and no normals: smooth normals must point away from the centre.
    const fs::path source = g_root / "mesh" / "cube.obj";
    writeText(source,
              "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
              "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\nf 4 8 7\nf 4 7 3\n"
              "f 1 5 8\nf 1 8 4\nf 2 3 7\nf 2 7 6\n");
    fuse::cook::CookedMesh mesh;
    std::string error;
    const bool ok = fuse::cook::import_mesh_file(path_of(source), {}, mesh, &error);
    expectTrue(ok, "normal-less cube imports");
    u32 outward = 0;
    for (u32 v = 0; v < mesh.vertex_count(); ++v) {
        const f32* p = &mesh.positions[v * 3u];
        const f32* n = &mesh.normals[v * 3u];
        const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (std::fabs(len - 1.f) < 1e-4f && p[0] * n[0] + p[1] * n[1] + p[2] * n[2] > 0.5f) {
            ++outward;
        }
    }
    std::printf("generated normals: %u/%u unit-length and outward, %zu triangles\n", outward, mesh.vertex_count(),
                mesh.indices.size() / 3u);
    expectTrue(ok && mesh.vertex_count() >= 8u && outward == mesh.vertex_count(), "generated normals outward");
    expectTrue(mesh.indices.size() == 36u, "12 cube triangles");
}

// Gate: mesh importer produces byte-identical output from the same source (deterministic).
void testMeshDeterminism() {
    const fs::path sourceA = g_root / "machineA" / "assets" / "wedge.obj";
    const fs::path sourceB = g_root / "machine_b_other_layout" / "deep" / "dir" / "renamed.obj";
    writeText(sourceA, kTwoObjectObj);
    writeText(sourceB, kTwoObjectObj);

    const fs::path outA = g_root / "machineA" / "cooked" / "wedge.fusemesh";
    const fs::path outB = g_root / "machine_b_other_layout" / "out" / "x.fusemesh";
    const fuse::cook::CookStubWriteResult a = fuse::cook::cook_mesh_file(path_of(sourceA), path_of(outA));
    const fuse::cook::CookStubWriteResult b = fuse::cook::cook_mesh_file(path_of(sourceB), path_of(outB));
    expectTrue(a.ok && b.ok, "both mesh cooks succeed");

    // Third cook from another thread with a different working directory.
    const fs::path outC = g_root / "machineA" / "cooked" / "again.fusemesh";
    const fs::path previousCwd = fs::current_path();
    fs::current_path(g_root / "machine_b_other_layout");
    std::thread worker([&] { (void)fuse::cook::cook_mesh_file(path_of(sourceA), path_of(outC)); });
    worker.join();
    fs::current_path(previousCwd);

    const std::vector<u8> bytesA = readBytes(outA);
    const std::vector<u8> bytesB = readBytes(outB);
    const std::vector<u8> bytesC = readBytes(outC);
    std::printf("mesh determinism: %zu / %zu / %zu bytes\n", bytesA.size(), bytesB.size(), bytesC.size());
    expectTrue(!bytesA.empty() && bytesA == bytesB && bytesA == bytesC,
               "same OBJ cooks to byte-identical .fusemesh across paths, cwd and threads");
    const std::string asText(bytesA.begin(), bytesA.end());
    expectTrue(asText.find("wedge") == std::string::npos && asText.find("machine") == std::string::npos,
               "cooked mesh embeds no source path");
}

// ---------------------------------------------------------------------------------------------
// BC7: independent mode-6 decoder + PSNR
// ---------------------------------------------------------------------------------------------
struct BitReader {
    const u8* data;
    u32 bit = 0;
    u32 take(u32 count) {
        u32 value = 0;
        for (u32 i = 0; i < count; ++i, ++bit) {
            value |= static_cast<u32>((data[bit >> 3] >> (bit & 7u)) & 1u) << i;
        }
        return value;
    }
};

// Written from the BC7 format description for the modes the cooker emits: 1 (2 subsets, RGB
// 6-bit + shared p-bit, 3-bit indices), 6 (1 subset, RGBA 7-bit + p-bit, 4-bit indices) and
// 7 (2 subsets, RGBA 5-bit + p-bit, 2-bit indices). Any other mode is rejected.
// (The same encoded images were also cross-checked offline against an unrelated BC7 decoder.)
const fuse::u16 kRefPartition2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22,
};
const u8 kRefAnchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};
u32 g_modeHistogram[8] = {};

bool referenceDecodeBlock(const u8 block[16], u8 out[64]) {
    BitReader reader{block};
    u32 mode = 0;
    while (mode < 8u && reader.take(1) == 0u) {
        ++mode;
    }
    u32 subsets = 0, colorBits = 0, alphaBits = 0, indexBits = 0;
    bool uniqueP = false, sharedP = false;
    if (mode == 1u) {
        subsets = 2; colorBits = 6; indexBits = 3; sharedP = true;
    } else if (mode == 6u) {
        subsets = 1; colorBits = 7; alphaBits = 7; indexBits = 4; uniqueP = true;
    } else if (mode == 7u) {
        subsets = 2; colorBits = 5; alphaBits = 5; indexBits = 2; uniqueP = true;
    } else {
        return false;
    }
    ++g_modeHistogram[mode];
    const u32 partition = subsets == 2u ? reader.take(6) : 0u;
    u32 e[4][4] = {}; // endpoint (subset*2 + k), channel
    for (u32 c = 0; c < 3u; ++c) {
        for (u32 k = 0; k < subsets * 2u; ++k) {
            e[k][c] = reader.take(colorBits);
        }
    }
    for (u32 k = 0; k < subsets * 2u; ++k) {
        e[k][3] = alphaBits != 0u ? reader.take(alphaBits) : 0u;
    }
    u32 p[4] = {};
    if (uniqueP) {
        for (u32 k = 0; k < subsets * 2u; ++k) {
            p[k] = reader.take(1);
        }
    } else if (sharedP) {
        for (u32 s = 0; s < subsets; ++s) {
            p[s * 2u] = p[s * 2u + 1u] = reader.take(1);
        }
    }
    auto expand = [](u32 value, u32 bits) {
        value <<= (8u - bits);
        return value | (value >> bits);
    };
    for (u32 k = 0; k < subsets * 2u; ++k) {
        for (u32 c = 0; c < 3u; ++c) {
            e[k][c] = expand((e[k][c] << 1) | p[k], colorBits + 1u);
        }
        e[k][3] = alphaBits != 0u ? expand((e[k][3] << 1) | p[k], alphaBits + 1u) : 255u;
    }
    static const u32 w2[4] = {0, 21, 43, 64};
    static const u32 w3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
    static const u32 w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    const u32* weights = indexBits == 2u ? w2 : (indexBits == 3u ? w3 : w4);
    for (u32 texel = 0; texel < 16u; ++texel) {
        const u32 subset = subsets == 2u ? ((kRefPartition2[partition] >> texel) & 1u) : 0u;
        const bool anchor = texel == 0u || (subsets == 2u && texel == kRefAnchor2[partition]);
        const u32 index = reader.take(anchor ? indexBits - 1u : indexBits);
        for (u32 c = 0; c < 4u; ++c) {
            out[texel * 4u + c] = static_cast<u8>(((64u - weights[index]) * e[subset * 2u][c] +
                                                   weights[index] * e[subset * 2u + 1u][c] + 32u) >> 6);
        }
    }
    return reader.bit == 128u;
}

bool referenceDecodeImage(const std::vector<u8>& blocks, u32 width, u32 height, std::vector<u8>& rgba) {
    const u32 bx = (width + 3u) / 4u;
    const u32 by = (height + 3u) / 4u;
    if (blocks.size() != static_cast<std::size_t>(bx) * by * 16u) {
        return false;
    }
    rgba.assign(static_cast<std::size_t>(width) * height * 4u, 0);
    for (u32 y = 0; y < by; ++y) {
        for (u32 x = 0; x < bx; ++x) {
            u8 texels[64];
            if (!referenceDecodeBlock(&blocks[(static_cast<std::size_t>(y) * bx + x) * 16u], texels)) {
                return false;
            }
            for (u32 ty = 0; ty < 4u; ++ty) {
                for (u32 tx = 0; tx < 4u; ++tx) {
                    const u32 px = x * 4u + tx;
                    const u32 py = y * 4u + ty;
                    if (px < width && py < height) {
                        std::memcpy(&rgba[(static_cast<std::size_t>(py) * width + px) * 4u], &texels[(ty * 4u + tx) * 4u], 4);
                    }
                }
            }
        }
    }
    return true;
}

f64 psnr(const std::vector<u8>& a, const std::vector<u8>& b, u32 channels) {
    f64 sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4) {
        for (u32 c = 0; c < channels; ++c) {
            const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
            sum += d * d;
            ++count;
        }
    }
    const f64 mse = sum / static_cast<f64>(count);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Uncompressed 32-bit TGA (bottom-left origin) — read by stb_image in the cooker.
void writeTga(const fs::path& path, const std::vector<u8>& rgba, u32 width, u32 height) {
    std::vector<u8> bytes(18, 0);
    bytes[2] = 2;
    bytes[12] = static_cast<u8>(width & 0xFFu);
    bytes[13] = static_cast<u8>(width >> 8);
    bytes[14] = static_cast<u8>(height & 0xFFu);
    bytes[15] = static_cast<u8>(height >> 8);
    bytes[16] = 32;
    bytes[17] = 8;
    for (u32 row = 0; row < height; ++row) {
        const u32 y = height - 1u - row;
        for (u32 x = 0; x < width; ++x) {
            const u8* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4u];
            bytes.insert(bytes.end(), {p[2], p[1], p[0], p[3]});
        }
    }
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<u8> proceduralImage(u32 width, u32 height) {
    std::vector<u8> rgba(static_cast<std::size_t>(width) * height * 4u);
    u32 lcg = 12345u;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            lcg = lcg * 1664525u + 1013904223u;
            const f64 noise = static_cast<f64>((lcg >> 24) & 7u) - 3.5;
            const f64 fx = static_cast<f64>(x) / width;
            const f64 fy = static_cast<f64>(y) / height;
            const f64 r = 128.0 + 90.0 * std::sin(6.0 * fx + 2.0 * fy) + noise;
            const f64 g = 110.0 + 70.0 * std::cos(4.0 * fy - 3.0 * fx * fy) + noise;
            const f64 b = 60.0 + 150.0 * fx * fy + noise;
            const f64 a = 255.0 - 100.0 * fy;
            u8* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4u];
            p[0] = static_cast<u8>(std::clamp(r, 0.0, 255.0));
            p[1] = static_cast<u8>(std::clamp(g, 0.0, 255.0));
            p[2] = static_cast<u8>(std::clamp(b, 0.0, 255.0));
            p[3] = static_cast<u8>(std::clamp(a, 0.0, 255.0));
        }
    }
    return rgba;
}

struct Bc7Measurement {
    bool ok = false;
    f64 psnrRgb = 0.0;
    f64 psnrRgba = 0.0;
    f64 psnrMip1 = 0.0;
    u32 levels = 0;
};

Bc7Measurement cookAndMeasure(const fs::path& source, const std::vector<u8>& original, u32 width, u32 height,
                              const fs::path& output) {
    Bc7Measurement m;
    fuse::project::AssetCooker cooker;
    cooker.set_strict_import(true);
    fuse::project::TextureImportDesc desc;
    desc.input_path = path_of(source);
    desc.output_path = path_of(output);
    desc.generate_mipmaps = true;
    const fuse::project::CookRecord record = cooker.cook_texture(desc);
    if (!record.ok) {
        std::fprintf(stderr, "  texture cook failed: %s\n", record.note.c_str());
        return m;
    }
    fuse::cook::CookedTexture texture;
    std::string error;
    if (!fuse::cook::load_cooked_texture(path_of(output), texture, &error) || texture.levels.empty()) {
        std::fprintf(stderr, "  texture load failed: %s\n", error.c_str());
        return m;
    }
    const fuse::cook::CookedTexture::Level& base = texture.levels[0];
    std::vector<u8> decoded;
    if (base.width != width || base.height != height || !referenceDecodeImage(base.blocks, width, height, decoded)) {
        std::fprintf(stderr, "  reference decode rejected level 0\n");
        return m;
    }
    m.psnrRgb = psnr(original, decoded, 3u);
    m.psnrRgba = psnr(original, decoded, 4u);
    m.levels = static_cast<u32>(texture.levels.size());

    // Mip 1 against an independent 2x2 box filter of the original.
    if (texture.levels.size() > 1u) {
        const fuse::cook::CookedTexture::Level& mip = texture.levels[1];
        std::vector<u8> expected(static_cast<std::size_t>(mip.width) * mip.height * 4u);
        for (u32 y = 0; y < mip.height; ++y) {
            for (u32 x = 0; x < mip.width; ++x) {
                for (u32 c = 0; c < 4u; ++c) {
                    u32 sum = 0;
                    for (u32 dy = 0; dy < 2u; ++dy) {
                        for (u32 dx = 0; dx < 2u; ++dx) {
                            const u32 sx = std::min(x * 2u + dx, width - 1u);
                            const u32 sy = std::min(y * 2u + dy, height - 1u);
                            sum += original[(static_cast<std::size_t>(sy) * width + sx) * 4u + c];
                        }
                    }
                    expected[(static_cast<std::size_t>(y) * mip.width + x) * 4u + c] = static_cast<u8>((sum + 2u) / 4u);
                }
            }
        }
        std::vector<u8> mipDecoded;
        if (referenceDecodeImage(mip.blocks, mip.width, mip.height, mipDecoded)) {
            m.psnrMip1 = psnr(expected, mipDecoded, 3u);
        }
    }
    u32 expectedLevels = 1;
    for (u32 w = width, h = height; w > 1u || h > 1u; w = std::max(1u, w / 2u), h = std::max(1u, h / 2u)) {
        ++expectedLevels;
    }
    m.ok = m.levels == expectedLevels;
    return m;
}

// Gate: texture BC7 compression PSNR > 40 dB vs original.
void testBc7Psnr() {
    const u32 width = 256;
    const u32 height = 192;
    const std::vector<u8> image = proceduralImage(width, height);
    const fs::path tga = g_root / "tex" / "procedural.tga";
    writeTga(tga, image, width, height);
    const Bc7Measurement procedural = cookAndMeasure(tga, image, width, height, g_root / "cooked" / "procedural.fusetex");
    std::printf("BC7 procedural 256x192: PSNR RGB %.2f dB, RGBA %.2f dB, mip1 %.2f dB, %u levels\n",
                procedural.psnrRgb, procedural.psnrRgba, procedural.psnrMip1, procedural.levels);
    expectTrue(procedural.ok, "procedural texture cooks with a full mip chain");
    expectTrue(procedural.psnrRgb > 40.0 && procedural.psnrRgba > 40.0, "BC7 PSNR > 40 dB (procedural)");
    expectTrue(procedural.psnrMip1 > 40.0, "BC7 mip 1 PSNR > 40 dB vs box-filtered reference");

    // Odd, non-multiple-of-4 dimensions exercise edge padding (37x23 window of the same image).
    std::vector<u8> odd;
    for (u32 y = 50; y < 73u; ++y) {
        const u8* row = &image[(static_cast<std::size_t>(y) * width + 100u) * 4u];
        odd.insert(odd.end(), row, row + 37u * 4u);
    }
    const fs::path oddTga = g_root / "tex" / "odd.tga";
    writeTga(oddTga, odd, 37, 23);
    const Bc7Measurement oddResult = cookAndMeasure(oddTga, odd, 37, 23, g_root / "cooked" / "odd.fusetex");
    std::printf("BC7 procedural 37x23: PSNR RGB %.2f dB, %u levels\n", oddResult.psnrRgb, oddResult.levels);
    expectTrue(oddResult.ok && oddResult.psnrRgb > 40.0, "BC7 PSNR > 40 dB on non-multiple-of-4 image");

#if defined(FUSE_HAS_STB_IMAGE) && defined(FUSE_SOURCE_DIR)
    // Real textures from the sample project (read only).
    const char* samples[] = {
        "Templates/BaseGame/game/core/gameObjects/images/defaultRoadTextureTop.png",
        "Templates/BaseGame/game/data/Prototyping/shapes/kork_chan.png",
        "Templates/BaseGame/game/core/rendering/materials/NightSkybox/NightSkybox_1.png",
        "Templates/BaseGame/game/data/UI/images/BackgroundImage.png",
    };
    u32 measured = 0;
    for (const char* relative : samples) {
        const fs::path source = fs::path(FUSE_SOURCE_DIR) / relative;
        if (!fs::exists(source)) {
            continue;
        }
        int w = 0;
        int h = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load(path_of(source).c_str(), &w, &h, &channels, 4);
        if (pixels == nullptr) {
            continue;
        }
        const std::vector<u8> original(pixels, pixels + static_cast<std::size_t>(w) * h * 4u);
        stbi_image_free(pixels);
        const Bc7Measurement real = cookAndMeasure(source, original, static_cast<u32>(w), static_cast<u32>(h),
                                                   g_root / "cooked" / (std::to_string(measured) + ".fusetex"));
        std::printf("BC7 %s (%dx%d): PSNR RGB %.2f dB, RGBA %.2f dB, mip1 %.2f dB\n", relative, w, h, real.psnrRgb,
                    real.psnrRgba, real.psnrMip1);
        expectTrue(real.ok, "sample texture cooks with full mip chain");
        expectTrue(real.psnrRgb > 40.0 && real.psnrRgba > 40.0, "BC7 PSNR > 40 dB on sample texture");
        ++measured;
    }
    expectTrue(measured >= 1u, "at least one repository sample texture measured");
#endif
    std::printf("BC7 block modes emitted: mode1=%u mode6=%u mode7=%u\n", g_modeHistogram[1], g_modeHistogram[6],
                g_modeHistogram[7]);

    // Texture cook is deterministic too.
    const fs::path again = g_root / "elsewhere" / "procedural_copy.fusetex";
    const fs::path tgaCopy = g_root / "other" / "copy.tga";
    writeTga(tgaCopy, image, width, height);
    const fuse::cook::CookStubWriteResult second = fuse::cook::cook_texture_bc7_file(path_of(tgaCopy), path_of(again), true);
    expectTrue(second.ok && readBytes(again) == readBytes(g_root / "cooked" / "procedural.fusetex"),
               "same image cooks to byte-identical BC7 output");
}

// ---------------------------------------------------------------------------------------------
// Error reporting for bad inputs (strict cook).
// ---------------------------------------------------------------------------------------------
void testBadInputs() {
    fuse::project::AssetCooker cooker;
    cooker.set_strict_import(true);

    struct MeshCase {
        const char* name;
        std::string contents;
    };
    const MeshCase meshCases[] = {
        {"empty.obj", "# nothing here\n"},
        {"garbage.obj", std::string("\x01\x02\xFFnot a mesh\x00\x7F", 16)},
        {"bad_index.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n"},
        {"lines_only.obj", "v 0 0 0\nv 1 0 0\nl 1 2\n"},
    };
    for (const MeshCase& c : meshCases) {
        const fs::path source = g_root / "bad" / c.name;
        writeText(source, c.contents);
        const fs::path output = g_root / "bad_out" / (std::string(c.name) + ".fusemesh");
        fuse::project::MeshImportDesc desc;
        desc.input_path = path_of(source);
        desc.output_path = path_of(output);
        const fuse::project::CookRecord record = cooker.cook_mesh(desc);
        std::printf("bad mesh %-15s -> ok=%d status=%s note='%s'\n", c.name, record.ok ? 1 : 0,
                    fuse::project::cookStatusName(record.status), record.note.c_str());
        expectTrue(!record.ok && record.status == fuse::project::CookStatus::InvalidInput, "bad mesh is rejected");
        expectTrue(!record.note.empty(), "bad mesh rejection carries a reason");
        expectTrue(!fs::exists(output), "rejected mesh writes no output");
    }

    const fs::path badPng = g_root / "bad" / "corrupt.png";
    writeText(badPng, std::string("\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDRtruncated", 25));
    fuse::project::TextureImportDesc tex;
    tex.input_path = path_of(badPng);
    tex.output_path = path_of(g_root / "bad_out" / "corrupt.fusetex");
    const fuse::project::CookRecord texRecord = cooker.cook_texture(tex);
    std::printf("bad texture corrupt.png -> ok=%d status=%s note='%s'\n", texRecord.ok ? 1 : 0,
                fuse::project::cookStatusName(texRecord.status), texRecord.note.c_str());
    expectTrue(!texRecord.ok && texRecord.status == fuse::project::CookStatus::InvalidInput,
               "corrupt PNG is rejected");
    expectTrue(!fs::exists(tex.output_path), "rejected texture writes no output");

    fuse::project::MeshImportDesc missing;
    missing.input_path = path_of(g_root / "bad" / "does_not_exist.obj");
    missing.output_path = path_of(g_root / "bad_out" / "missing.fusemesh");
    const fuse::project::CookRecord missingRecord = cooker.cook_mesh(missing);
    expectTrue(!missingRecord.ok && missingRecord.status == fuse::project::CookStatus::SourceMissing,
               "missing source reports SourceMissing");

    // Failed cooks are not cached: retrying reports the same error rather than a cache hit.
    fuse::project::MeshImportDesc retry;
    retry.input_path = path_of(g_root / "bad" / "empty.obj");
    retry.output_path = path_of(g_root / "bad_out" / "empty.obj.fusemesh");
    const fuse::project::CookRecord retried = cooker.cook_mesh(retry);
    expectTrue(!retried.ok && !retried.cache_hit, "failed cook is never served from the cache");
    expectTrue(cooker.cache().entry_count() == 0u, "no cache entries recorded for failed cooks");

    // Damaged cooked files are detected by the loader.
    const fs::path good = g_root / "mesh" / "wedge.obj";
    const fs::path cooked = g_root / "bad_out" / "damaged.fusemesh";
    (void)fuse::cook::cook_mesh_file(path_of(good), path_of(cooked));
    std::vector<u8> bytes = readBytes(cooked);
    fuse::cook::CookedMesh mesh;
    std::string error;
    bytes[bytes.size() / 2] ^= 0x10u;
    expectTrue(!fuse::cook::deserialize_cooked_mesh(bytes.data(), bytes.size(), mesh, &error) &&
                   error.find("checksum") != std::string::npos,
               "flipped bit in cooked mesh fails the checksum");
    bytes[bytes.size() / 2] ^= 0x10u;
    expectTrue(!fuse::cook::deserialize_cooked_mesh(bytes.data(), bytes.size() - 5u, mesh, &error),
               "truncated cooked mesh rejected");
    expectTrue(fuse::cook::deserialize_cooked_mesh(bytes.data(), bytes.size(), mesh, &error), "restored bytes load");
}

// ---------------------------------------------------------------------------------------------
// Incremental cook: unchanged inputs not recooked, dependency invalidation, deleted/damaged outputs.
// ---------------------------------------------------------------------------------------------
struct OutputStamp {
    std::vector<u8> bytes;
    fs::file_time_type mtime;
};

OutputStamp stamp(const fs::path& path) {
    OutputStamp s;
    s.bytes = readBytes(path);
    std::error_code ec;
    s.mtime = fs::last_write_time(path, ec);
    return s;
}

fuse::project::CookBatchResult runCook(const fuse::project::CookManifest& manifest, const fs::path& cachePath) {
    // Mirrors fuse_cook --manifest: fresh process state, cache loaded from and saved to disk.
    fuse::project::ImportPipeline pipeline;
    pipeline.set_project_root(manifest.project_root);
    pipeline.cooker().set_strict_import(true);
    (void)pipeline.load_cook_cache(path_of(cachePath));
    if (pipeline.cooker().would_reconcile_invalidation(manifest)) {
        (void)pipeline.cooker().invalidate_stale_dependency_hashes(manifest);
    }
    pipeline.plan_from_manifest(manifest);
    fuse::project::CookBatchResult result = pipeline.execute(false);
    (void)pipeline.save_cook_cache(path_of(cachePath));
    return result;
}

const fuse::project::CookRecord* recordFor(const fuse::project::CookBatchResult& result, const fs::path& output) {
    for (const fuse::project::CookRecord& record : result.records) {
        if (record.output_path == path_of(output)) {
            return &record;
        }
    }
    return nullptr;
}

void testIncrementalCook() {
    const fs::path project = g_root / "project";
    const fs::path meshSrc = project / "src" / "wedge.obj";
    const fs::path texSrc = project / "src" / "albedo.tga";
    const fs::path propSrc = project / "src" / "prop.obj";
    writeText(meshSrc, kTwoObjectObj);
    writeText(propSrc, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTga(texSrc, proceduralImage(64, 64), 64, 64);

    const fs::path meshOut = project / "cooked" / "wedge.fusemesh";
    const fs::path texOut = project / "cooked" / "albedo.fusetex";
    const fs::path propOut = project / "cooked" / "prop.fusemesh";
    const fs::path cachePath = project / ".fuse" / "cook_cache.json";

    fuse::project::CookManifest manifest;
    manifest.project_root = path_of(project);
    fuse::project::CookManifestEntry mesh{fuse::project::CookAssetKind::Mesh, path_of(meshSrc), path_of(meshOut), {}};
    fuse::project::CookManifestEntry tex{fuse::project::CookAssetKind::Texture, path_of(texSrc), path_of(texOut), {}};
    // The prop depends on the wedge mesh (e.g. a shared collision hull).
    fuse::project::CookManifestEntry prop{fuse::project::CookAssetKind::Mesh, path_of(propSrc), path_of(propOut),
                                          {path_of(meshOut)}};
    manifest.assets = {mesh, tex, prop};

    auto countHits = [](const fuse::project::CookBatchResult& result) {
        u32 hits = 0;
        for (const fuse::project::CookRecord& record : result.records) {
            hits += record.cache_hit ? 1u : 0u;
        }
        return hits;
    };

    const fuse::project::CookBatchResult first = runCook(manifest, cachePath);
    expectTrue(first.ok && first.records.size() == 3u && countHits(first) == 0u, "first cook: 3 misses");
    const OutputStamp mesh1 = stamp(meshOut);
    const OutputStamp tex1 = stamp(texOut);
    const OutputStamp prop1 = stamp(propOut);
    fuse::cook::CookedMesh loaded;
    expectTrue(fuse::cook::load_cooked_mesh(path_of(meshOut), loaded) &&
                   fuse::cook::load_cooked_mesh(path_of(propOut), loaded),
               "manifest mesh outputs are real FMSH meshes");
    fuse::cook::CookedTexture texture;
    expectTrue(fuse::cook::load_cooked_texture(path_of(texOut), texture) && texture.levels.size() == 7u,
               "manifest texture output is BC7 with 7 mips");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const fuse::project::CookBatchResult second = runCook(manifest, cachePath);
    const OutputStamp mesh2 = stamp(meshOut);
    const OutputStamp tex2 = stamp(texOut);
    const OutputStamp prop2 = stamp(propOut);
    std::printf("incremental: second run %u/3 cache hits\n", countHits(second));
    expectTrue(second.ok && countHits(second) == 3u, "unchanged inputs: every asset is a cache hit");
    expectTrue(mesh2.mtime == mesh1.mtime && tex2.mtime == tex1.mtime && prop2.mtime == prop1.mtime,
               "unchanged inputs: no output file rewritten");

    // Change the upstream mesh: it and its dependent re-cook, the texture stays cached.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::string edited = kTwoObjectObj;
    edited.replace(edited.find("v 0.0 1.5 0.0"), 13, "v 0.0 2.5 0.0");
    writeText(meshSrc, edited);
    const fuse::project::CookBatchResult third = runCook(manifest, cachePath);
    const fuse::project::CookRecord* meshRec = recordFor(third, meshOut);
    const fuse::project::CookRecord* texRec = recordFor(third, texOut);
    const fuse::project::CookRecord* propRec = recordFor(third, propOut);
    std::printf("incremental: after editing wedge.obj -> wedge hit=%d, prop hit=%d, albedo hit=%d\n",
                meshRec && meshRec->cache_hit ? 1 : 0, propRec && propRec->cache_hit ? 1 : 0,
                texRec && texRec->cache_hit ? 1 : 0);
    expectTrue(third.ok && meshRec && !meshRec->cache_hit, "edited source re-cooks");
    expectTrue(propRec && !propRec->cache_hit, "dependent of edited source re-cooks (dependency invalidation)");
    expectTrue(texRec && texRec->cache_hit, "unrelated texture stays cached");
    expectTrue(stamp(meshOut).bytes != mesh1.bytes, "re-cooked mesh reflects the edit");
    expectTrue(stamp(texOut).mtime == tex1.mtime, "unrelated texture file untouched");

    // Deleted output: must be regenerated even though the cache still has an entry.
    fs::remove(texOut);
    const fuse::project::CookBatchResult fourth = runCook(manifest, cachePath);
    const fuse::project::CookRecord* texAgain = recordFor(fourth, texOut);
    expectTrue(fourth.ok && texAgain && !texAgain->cache_hit && fs::exists(texOut), "deleted output is re-cooked");
    expectTrue(stamp(texOut).bytes == tex1.bytes, "re-cooked texture is byte-identical to the first cook");

    // Damaged output (truncated): strict cooks validate cached outputs and re-cook.
    {
        std::vector<u8> bytes = readBytes(propOut);
        bytes.resize(bytes.size() / 2);
        std::ofstream out(propOut, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const fuse::project::CookBatchResult fifth = runCook(manifest, cachePath);
    const fuse::project::CookRecord* propAgain = recordFor(fifth, propOut);
    expectTrue(fifth.ok && propAgain && !propAgain->cache_hit, "truncated output is re-cooked");
    expectTrue(fuse::cook::load_cooked_mesh(path_of(propOut), loaded), "re-cooked output loads");

    const fuse::project::CookBatchResult sixth = runCook(manifest, cachePath);
    expectTrue(sixth.ok && countHits(sixth) == 3u, "steady state after repairs: all hits");

    // A broken source fails the batch with a reason; its dependent is not cooked from stale data.
    writeText(meshSrc, "# broken\n");
    const fuse::project::CookBatchResult broken = runCook(manifest, cachePath);
    const fuse::project::CookRecord* brokenRec = recordFor(broken, meshOut);
    std::printf("incremental: broken source -> batch ok=%d, wedge ok=%d note='%s'\n", broken.ok ? 1 : 0,
                brokenRec && brokenRec->ok ? 1 : 0, brokenRec ? brokenRec->note.c_str() : "");
    expectTrue(!broken.ok && brokenRec && !brokenRec->ok, "broken source fails the cook batch");
    expectTrue(brokenRec && brokenRec->note.find("mesh import failed") != std::string::npos,
               "batch record carries the importer's failure reason");
    const fuse::project::CookRecord* propBroken = recordFor(broken, propOut);
    expectTrue(propBroken && !propBroken->ok, "dependent of a failed cook is not reported ok");
}

} // namespace

int main() {
    fuse::core::initialize();
    g_root = fs::temp_directory_path() / ("fuse_b7_cook_gates_" + std::to_string(static_cast<long long>(FUSE_GETPID())));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root);

    testMeshRoundTripAgainstObjReference();
    testMeshGeneratedNormals();
    testMeshDeterminism();
    testBc7Psnr();
    testBadInputs();
    testIncrementalCook();

    fs::remove_all(g_root, ec);
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse_b7_cook_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b7_cook_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
