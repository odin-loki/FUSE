// Asset W0.7 x WP-1.5: CPU gates of the material resolve's layered bin (no Vulkan; also run in the stub tree). The
// Lavapipe gates are in test_rp_material_resolve_layered.cpp.
//
//   layout    MlResolveTable (C++ vs ml_common.glsl / .slang, names + order), ResolveFrameConstants::layered at the
//             old `flags` slot (176 bytes unchanged) and its twins' field lists, ResolveBinLayout's appended layered
//             args + list (the four feature bins' layout unchanged), resolve_features / bin_features, the
//             kGpuMaterialLayered bit and its shader twins
//   mips      ml_build_mips (level count / sizes / contiguity; box filter on hand-made textures: constant, unorm
//             checker, sRGB averaged in linear light) and ml_trilinear: == the pool's ml_bilinear bit for bit at
//             magnification, == bilinear of level L at LOD L, the LOD-L+0.5 blend, clamping at the last level;
//             ml_evaluate with the trilinear filter and no footprint == the texel-pool ml_evaluate bit for bit, and
//             the pool ml_evaluate ignores derivatives (bit for bit): the standalone W0.7 path is unchanged
//   surface   on a CPU-rastered scene with layered rows: every layered pixel's layered_surface (world position
//             reprojects onto the pixel centre, dP/dx and dP/dy == central differences, camera_centre == the eye,
//             view distance, layered-table index = GPUMaterial::padding, vertex colour 1), non-layered pixels have
//             none; the CPU reference evaluation of every layered pixel is finite and in range
//   classify  pixel bin == kBinLayered exactly for flagged rows, tile bin == max pixel bin, layered_tile_list ==
//             the tiles whose bin is kBinLayered, mixed tiles exist
//   api       MaterialLayers::setLibrary without init fails and exposes no table; ResolveFrameDesc::layered default 0
#include "test_rp_material_layers_common.hpp"
#include "test_rp_material_resolve_layered_scene.hpp"

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_layers/material_layers.hpp>
#include <fuse/renderer/material_layers/ml_mips.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/material_resolve/resolve_kernel.hpp>
#include <fuse/renderer/material_resolve/resolve_reference.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>
#include <fuse/renderer/visbuffer/vis_reference.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef FUSE_RP_MR_SHADER_DIR
#define FUSE_RP_MR_SHADER_DIR "."
#endif
#ifndef FUSE_RP_ML_SHADER_DIR
#define FUSE_RP_ML_SHADER_DIR "."
#endif
#ifndef FUSE_RP_SCENE_SHADER_DIR
#define FUSE_RP_SCENE_SHADER_DIR "."
#endif
#ifndef FUSE_RP_ML_FIXTURE_DIR
#define FUSE_RP_ML_FIXTURE_DIR "."
#endif

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::material_resolve;
using namespace fuse::renderer::material_layers;
using fuse::f32;
using fuse::f64;
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

bool sameBits(const void* a, const void* b, usize n) { return std::memcmp(a, b, n) == 0; }

// --- layout -----------------------------------------------------------------------------------------------------------
/// Field names of `struct <name> {` ... `};` in a shader source (last identifier before ';', array suffix dropped).
std::vector<std::string> shaderFields(const std::string& text, const std::string& open) {
    std::vector<std::string> out;
    const usize at = text.find(open);
    if (at == std::string::npos) {
        return out;
    }
    const usize end = text.find("};", at);
    if (end == std::string::npos) {
        return out;
    }
    const std::string block = text.substr(at, end - at);
    usize line = block.find('\n');
    while (line != std::string::npos) {
        const usize next = block.find('\n', line + 1u);
        std::string l = block.substr(line + 1u, (next == std::string::npos ? block.size() : next) - line - 1u);
        const usize comment = l.find("//");
        if (comment != std::string::npos) {
            l = l.substr(0, comment);
        }
        const usize semi = l.find(';');
        if (semi != std::string::npos) {
            std::string decl = l.substr(0, semi);
            const usize bracket = decl.find('[');
            if (bracket != std::string::npos) {
                decl = decl.substr(0, bracket);
            }
            while (!decl.empty() && decl.back() == ' ') {
                decl.pop_back();
            }
            const usize space = decl.find_last_of(' ');
            out.push_back(space == std::string::npos ? decl : decl.substr(space + 1u));
        }
        line = next;
    }
    return out;
}

int runLayout() {
    expect(sizeof(MlResolveTable) == 32u && offsetof(MlResolveTable, materials) == 0u && offsetof(MlResolveTable, textures) == 8u &&
               offsetof(MlResolveTable, materialCount) == 16u && offsetof(MlResolveTable, textureCount) == 20u,
           "MlResolveTable layout");
    const std::vector<std::string> table = {"materials", "textures", "materialCount", "textureCount", "reserved"};
    const std::vector<std::string> frame = {"viewProj", "prevViewProj", "width", "height", "tilesX", "tilesY", "scene",
                                            "vis", "sampler_", "tileCapacity", "bins", "binsHandle", "layered"};
    for (const char* lang : {"glsl", "slang"}) {
        std::string ml;
        std::string mr;
        std::string scene;
        const bool read = ml_test::readText(std::string(FUSE_RP_ML_SHADER_DIR) + "/ml_common." + lang, ml) &&
                          ml_test::readText(std::string(FUSE_RP_MR_SHADER_DIR) + "/mr_common." + lang, mr) &&
                          ml_test::readText(std::string(FUSE_RP_SCENE_SHADER_DIR) + "/gpu_scene." + lang, scene);
        expect(read, "shader sources readable");
        const bool glsl = std::string(lang) == "glsl";
        expect(shaderFields(ml, "struct MlResolveTable {") == table, "MlResolveTable twin: same fields, same order");
        expect(shaderFields(mr, glsl ? "struct FuseMrFrame {" : "struct MrFrame {") == frame,
               "resolve frame constants twin: `layered` in the old `flags` slot");
        expect(scene.find(glsl ? "#define FUSE_GPU_MATERIAL_LAYERED 0x80000u" : "kFuseGpuMaterialLayered = 0x80000u") !=
                   std::string::npos,
               "kGpuMaterialLayered shader twin");
        expect(mr.find(glsl ? "#define FUSE_MR_BIN_LAYERED 5u" : "kFuseMrBinLayered = 5u") != std::string::npos,
               "kBinLayered shader twin");
        expect(ml.find("ML_NO_PUSH_CONSTANTS") != std::string::npos && ml.find("ML_BINDLESS_TEXTURES") != std::string::npos,
               "ml_common is includable by the resolve (push-constant guard, bindless filter hook)");
        std::printf("layout: %s twins: MlResolveTable %zu fields, resolve frame %zu fields (last `%s`)\n", lang,
                    shaderFields(ml, "struct MlResolveTable {").size(),
                    shaderFields(mr, glsl ? "struct FuseMrFrame {" : "struct MrFrame {").size(),
                    shaderFields(mr, glsl ? "struct FuseMrFrame {" : "struct MrFrame {").empty()
                        ? "?"
                        : shaderFields(mr, glsl ? "struct FuseMrFrame {" : "struct MrFrame {").back().c_str());
    }
    expect(gpu_scene::kGpuMaterialLayered == 0x80000u, "kGpuMaterialLayered = bit 19");
    expect(sizeof(ResolveFrameConstants) == 176u && offsetof(ResolveFrameConstants, layered) == 172u,
           "ResolveFrameConstants: 176 bytes, `layered` at 172 (the former unused `flags`)");
    expect(ResolveBinLayout::bytes(10u) == 240u && ResolveBinLayout::layeredArgsOffset(10u) == 240u &&
               ResolveBinLayout::layeredListOffset(10u) == 256u && ResolveBinLayout::totalBytes(10u) == 296u,
           "layered args + list appended after the four feature-bin lists");
    bool offsets = true;
    for (u32 b = 0; b < kBinCount; ++b) {
        offsets = offsets && ResolveBinLayout::argsOffset(b, 10u) == b * 16u && ResolveBinLayout::listOffset(b, 10u) == 80u + b * 40u;
    }
    expect(offsets && ResolveBinLayout::argsOffset(kBinLayered, 10u) == 240u && ResolveBinLayout::listOffset(kBinLayered, 10u) == 256u,
           "argsOffset / listOffset of every bin");
    expect(resolve_features(kBinFlat) == 0u && resolve_features(kBinTextured) == kFeatureTextures &&
               resolve_features(kBinNormalMapped) == kFeatureAll && resolve_features(kBinUber) == (kFeatureAll | kFeatureLayered) &&
               resolve_features(kBinLayered) == (kFeatureAll | kFeatureLayered) && bin_features(kBinUber) == kFeatureAll,
           "feature classes: layered bin and uber evaluate every feature + the layered one");
    expect(kBinLayered > kBinUber && kBinLayered > kBinNormalMapped && kBinDrawCount == kBinCount + 1u,
           "the layered bin wins the tile maximum; five binned draws");
    std::printf("layout: ResolveFrameConstants %zu B (layered @ %zu), bins 80 B + 4 x tiles x 4 B + layered 16 B + tiles x 4 B\n",
                sizeof(ResolveFrameConstants), offsetof(ResolveFrameConstants, layered));
    return 0;
}

// --- mips -------------------------------------------------------------------------------------------------------------
MlF4 bilinearAt(const MlMipChains& c, u32 tex, u32 level, u32 flags, MlF2 uv) {
    const MlMipLevel& l = c.levels[c.firstLevel[tex] + level];
    MlView v{};
    v.texels = c.texels.data();
    v.lut = c.lut;
    MlTexture t{};
    t.offset = l.offset;
    t.width = l.width;
    t.height = l.height;
    t.flags = flags;
    return ml_bilinear(v, t, uv);
}

int runMips() {
    // Hand-made textures: constant, unorm checker, sRGB black / white checker, non-square.
    MlLibrary lib;
    std::vector<u32> constant(16u, 0x80402010u);
    std::vector<u32> checker(4u);
    for (u32 i = 0; i < 4u; ++i) {
        checker[i] = ((i + i / 2u) & 1u) != 0u ? 0xFFFFFFFFu : 0xFF000000u;
    }
    const u32 tConst = lib.addTexture("constant", 4u, 4u, 0u, constant);
    const u32 tChecker = lib.addTexture("checker", 2u, 2u, 0u, checker);
    const u32 tSrgb = lib.addTexture("checker_srgb", 2u, 2u, kMlTexSrgb, checker);
    std::vector<u32> wide(8u * 2u, 0xFF204060u);
    const u32 tWide = lib.addTexture("wide", 8u, 2u, 0u, wide);
    MlMipChains c;
    ml_build_mips(lib, c);
    expect(c.levelCount[tConst] == 3u && c.levelCount[tChecker] == 2u && c.levelCount[tWide] == 4u, "level counts");
    const MlMipLevel& w3 = c.levels[c.firstLevel[tWide] + 3u];
    expect(w3.width == 1u && w3.height == 1u && c.levels[c.firstLevel[tWide] + 1u].height == 1u, "non-square level sizes");
    bool constOk = true;
    for (u32 l = 0; l < 3u; ++l) {
        const MlMipLevel& lv = c.levels[c.firstLevel[tConst] + l];
        for (u32 i = 0; i < lv.width * lv.height; ++i) {
            constOk = constOk && c.texels[lv.offset + i] == 0x80402010u;
        }
    }
    expect(constOk, "a constant texture stays constant at every level");
    const u32 unorm = c.texels[c.levels[c.firstLevel[tChecker] + 1u].offset];
    const u32 srgb = c.texels[c.levels[c.firstLevel[tSrgb] + 1u].offset];
    expect((unorm & 255u) == 128u && (unorm >> 24) == 255u, "unorm checker averages to 128 (round half up), alpha 255");
    expect((srgb & 255u) == 188u && ((srgb >> 8) & 255u) == 188u && (srgb >> 24) == 255u,
           "sRGB checker averages in linear light (0.5 -> sRGB 188)");
    bool contiguous = true;
    for (u32 t = 0; t < c.textureCount(); ++t) {
        u32 cursor = c.levels[c.firstLevel[t]].offset;
        for (u32 l = 0; l < c.levelCount[t]; ++l) {
            const MlMipLevel& lv = c.levels[c.firstLevel[t] + l];
            contiguous = contiguous && lv.offset == cursor;
            cursor += lv.width * lv.height;
        }
    }
    expect(contiguous, "levels are contiguous and tightly packed (UploadQueue::stageImage layout)");

    // The fixture library: trilinear vs the pool filter.
    MlLibrary fix;
    if (!ml_test::buildLibrary(FUSE_RP_ML_FIXTURE_DIR, fix)) {
        std::fprintf(stderr, "FAIL: fixture library\n");
        return 1;
    }
    MlMipChains fc;
    ml_build_mips(fix, fc);
    const MlView pool = fix.view();
    u32 state = 12345u;
    auto rnd = [&state]() {
        state = ml_hash(state + 0x9e3779b9u);
        return ml_unit(state);
    };
    u32 magnified = 0;
    u32 exactLevel = 0;
    u32 halfLevel = 0;
    u32 clamped = 0;
    bool magOk = true;
    bool levelOk = true;
    bool halfOk = true;
    bool clampOk = true;
    for (u32 t = 0; t < fc.textureCount(); ++t) {
        const MlTexture& tex = fix.textures()[t];
        const f32 W = static_cast<f32>(tex.width);
        for (u32 i = 0; i < 64u; ++i) {
            const MlF2 uv{(rnd() * 2.f - 1.f) * 3.f, (rnd() * 2.f - 1.f) * 3.f};
            // Magnification: footprint below one texel.
            const MlF2 sub{rnd() * 0.5f / W, 0.f};
            const MlF4 a = ml_trilinear(fc, t, tex.flags, uv, sub, MlF2{0.f, sub.x * 0.5f});
            const MlF4 b = ml_bilinear(pool, tex, uv);
            magOk = magOk && sameBits(&a, &b, sizeof(a));
            ++magnified;
            // LOD exactly L (footprint 2^L texels along u).
            const u32 L = 1u + i % (fc.levelCount[t] - 1u);
            const MlF2 dx{static_cast<f32>(1u << L) / W, 0.f};
            const MlF4 e = ml_trilinear(fc, t, tex.flags, uv, dx, MlF2{0.f, 0.f});
            const MlF4 f = bilinearAt(fc, t, L, tex.flags, uv);
            levelOk = levelOk && sameBits(&e, &f, sizeof(e)) && ml_trilinear_lod(fc, t, dx, MlF2{}) == static_cast<f32>(L);
            ++exactLevel;
            // LOD L - 0.5: the midpoint of levels L - 1 and L.
            const MlF4 g = ml_trilinear(fc, t, tex.flags, uv, dx, MlF2{0.f, 0.f}, -0.5f);
            const MlF4 lo = bilinearAt(fc, t, L - 1u, tex.flags, uv);
            halfOk = halfOk && std::fabs(g.x - (lo.x + (f.x - lo.x) * 0.5f)) <= 1e-6f &&
                     std::fabs(g.w - (lo.w + (f.w - lo.w) * 0.5f)) <= 1e-6f;
            ++halfLevel;
            // Beyond the last level: the 1 x 1 level.
            const MlF4 h = ml_trilinear(fc, t, tex.flags, uv, MlF2{64.f, 0.f}, MlF2{0.f, 0.f});
            const MlF4 k = bilinearAt(fc, t, fc.levelCount[t] - 1u, tex.flags, uv);
            clampOk = clampOk && sameBits(&h, &k, sizeof(h));
            ++clamped;
        }
    }
    expect(magOk, "trilinear at magnification == the pool's bilinear filter, bit for bit");
    expect(levelOk, "trilinear at LOD L == bilinear of level L, bit for bit");
    expect(halfOk, "trilinear at LOD L - 0.5 == the midpoint of levels L - 1 and L");
    expect(clampOk, "LOD above the chain clamps to the 1 x 1 level");
    std::printf("mips: %u textures (%u texels in all levels); trilinear checks: %u magnified, %u exact-LOD, %u half-LOD, "
                "%u clamped\n",
                fc.textureCount(), static_cast<u32>(fc.texels.size()), magnified, exactLevel, halfLevel, clamped);

    // ml_evaluate: the trilinear filter without a footprint and the pool filter with any footprint == the pool path.
    const MlTrilinearFilter filter{&fc, fix.textures().data(), static_cast<u32>(fix.textures().size()), 0.f};
    const MlView tri = ml_trilinear_view(fix, filter);
    const std::vector<MlSurface> surfaces = ml_test::randomSurfaces(4096u, 77u);
    u32 sameTri = 0;
    u32 sameGrad = 0;
    for (const MlSurface& s : surfaces) {
        const MlResult ref = ml_evaluate(pool, s);
        const MlResult a = ml_evaluate(tri, s, MlGrad{});
        MlGrad g{};
        g.duvdx = MlF2{rnd() * 0.1f, rnd() * 0.1f};
        g.duvdy = MlF2{rnd() * 0.1f, rnd() * 0.1f};
        g.dPdx = MlF3{rnd(), rnd(), rnd()};
        g.dPdy = MlF3{rnd(), rnd(), rnd()};
        const MlResult b = ml_evaluate(pool, s, g);
        sameTri += sameBits(&ref, &a, sizeof(ref)) ? 1u : 0u;
        sameGrad += sameBits(&ref, &b, sizeof(ref)) ? 1u : 0u;
    }
    expect(sameTri == surfaces.size(), "ml_evaluate(trilinear, no footprint) == ml_evaluate(pool), bit for bit");
    expect(sameGrad == surfaces.size(), "the pool path ignores derivatives (standalone W0.7 results unchanged)");
    std::printf("mips: ml_evaluate over %zu random surfaces: trilinear/no footprint == pool %u, pool with derivatives == pool %u\n",
                surfaces.size(), sameTri, sameGrad);
    return 0;
}

// --- scene with layered rows ------------------------------------------------------------------------------------------
constexpr u32 kW = 160;
constexpr u32 kH = 112;

struct Mat4 {
    f32 m[16] = {};
};
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f32 s = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}
Mat4 camera(const f32 eye[3], const f32 at[3]) {
    const f32 fy = 1.f / std::tan(0.55f);
    Mat4 p{};
    p.m[0] = fy / (static_cast<f32>(kW) / static_cast<f32>(kH));
    p.m[5] = -fy;
    p.m[10] = 80.f / (0.3f - 80.f);
    p.m[11] = -1.f;
    p.m[14] = 0.3f * 80.f / (0.3f - 80.f);
    f32 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f32 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f32& v : f) {
        v /= fl;
    }
    f32 s[3] = {-f[2], 0.f, f[0]};
    const f32 sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
    s[0] /= sl;
    s[2] /= sl;
    const f32 u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 v{};
    v.m[0] = s[0];
    v.m[4] = s[1];
    v.m[8] = s[2];
    v.m[1] = u[0];
    v.m[5] = u[1];
    v.m[9] = u[2];
    v.m[2] = -f[0];
    v.m[6] = -f[1];
    v.m[10] = -f[2];
    v.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    v.m[15] = 1.f;
    return mul(p, v);
}
gpu_scene::GpuTransform place(f32 x, f32 y, f32 z, f32 sx, f32 sy, f32 sz, f32 yaw) {
    gpu_scene::GpuTransform t{};
    const f32 c = std::cos(yaw);
    const f32 n = std::sin(yaw);
    t.rows[0][0] = c * sx;
    t.rows[0][2] = n * sz;
    t.rows[1][1] = sy;
    t.rows[2][0] = -n * sx;
    t.rows[2][2] = c * sz;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

constexpr f32 kEye[3] = {0.3f, 1.2f, 3.f};

struct Scene {
    gpu_scene::GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<ResolveMeshData> data;
    std::vector<resolve_kernel::MeshStreams> streams;
    std::vector<visbuffer::decode_kernel::MeshPositions> positions;
    std::vector<Material::GPUMaterial> rows;
    Mat4 viewProj{};

    bool build() {
        if (!gpu.init(gpu_scene::GpuSceneDesc{})) {
            return false;
        }
        gpu.beginFrame(1);
        const mr_test::SourceMesh src[4] = {mr_test::uvSphere(16, 24, 1.f), mr_test::torus(24, 12, 1.f, 0.35f), mr_test::box(),
                                            mr_test::plane(8, 40.f, 6.f)};
        meshes.resize(4);
        data.resize(4);
        for (u32 i = 0; i < 4u; ++i) {
            if (!mr_test::build(src[i], meshes[i]) || gpu.addMeshletMesh(meshes[i]) != i) {
                return false;
            }
            make_mesh_streams(meshes[i], data[i]);
            streams.push_back(data[i].streams);
            positions.push_back(visbuffer::decode_kernel::MeshPositions{meshes[i].positions.data(), meshes[i].vertex_count()});
        }
        u32 tex[mr_test::kTexCount];
        for (u32 i = 0; i < mr_test::kTexCount; ++i) {
            tex[i] = 100u + i;
        }
        rows = mrl_test::makeMaterials(tex);
        for (u32 i = 0; i < rows.size(); ++i) {
            gpu.setMaterial(i, rows[i]);
        }
        auto add = [&](u32 mesh, u32 material, const gpu_scene::GpuTransform& t) {
            gpu_scene::InstanceDesc d{};
            d.mesh = mesh;
            d.material = material;
            d.transform = t;
            gpu.addInstance(d);
        };
        using mrl_test::kLayeredBase;
        add(3, kLayeredBase + 3u, place(0.f, -1.6f, -14.f, 1.f, 1.f, 1.f, 0.3f));       // ground: triplanar + stochastic
        add(0, kLayeredBase + 7u, place(-2.2f, 0.f, -6.f, 1.f, 1.f, 1.f, 0.4f));        // everything
        add(1, mr_test::kMatTextured, place(1.8f, 0.2f, -7.f, 1.2f, 0.8f, 1.2f, 0.9f)); // non-layered
        add(2, kLayeredBase + 4u, place(0.2f, -0.4f, -9.f, 0.9f, 0.9f, 0.9f, 0.7f));   // box: rows base+4 / base+5
        add(0, kLayeredBase + 6u, place(3.5f, 1.2f, -11.f, -1.f, 1.f, 1.f, 0.f));      // mirrored, wet + detail
        add(0, mr_test::kMatNormalMapped, place(-4.f, 1.5f, -12.f, 0.7f, 1.4f, 0.7f, 0.f));
        add(1, kLayeredBase + 1u, place(-0.5f, 2.2f, -10.f, 1.f, 1.f, 1.f, 1.2f));     // stochastic
        const f32 at[3] = {0.f, -0.5f, -8.f};
        viewProj = camera(kEye, at);
        return gpu.commit().ok;
    }
};

void raster(Scene& s, std::vector<u32>& vis) {
    std::vector<visbuffer::RasterRefPixel> ref;
    const visbuffer::VisSceneView view = visbuffer::vis_scene_view(s.gpu, s.positions);
    visbuffer::raster_reference(view, s.viewProj.m, kW, kH, visbuffer::RasterRefOptions{}, ref);
    vis.assign(kW * kH * 2u, visbuffer::kVisInvalid);
    for (u32 p = 0; p < kW * kH; ++p) {
        vis[p * 2u] = ref[p].instance;
        vis[p * 2u + 1u] = ref[p].triangle;
    }
}

resolve_kernel::Params params(const ResolveSceneView& view, const std::vector<u32>& vis, const Mat4& vp) {
    resolve_kernel::Params p{};
    p.vis = {vis.data(), static_cast<u32>(vis.size())};
    p.instances = view.instances;
    p.transforms = view.transforms;
    p.prevTransforms = view.prevTransforms;
    p.meshes = view.meshes;
    p.indices = view.indices;
    p.streams = view.streams;
    p.materials = view.materials;
    std::memcpy(p.viewProj, vp.m, sizeof(p.viewProj));
    std::memcpy(p.prevViewProj, vp.m, sizeof(p.prevViewProj));
    p.width = kW;
    p.height = kH;
    p.tilesX = (kW + kTileSize - 1u) / kTileSize;
    p.tilesY = (kH + kTileSize - 1u) / kTileSize;
    return p;
}

int runSurface() {
    Scene s;
    if (!s.build()) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    std::vector<u32> vis;
    raster(s, vis);
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    const resolve_kernel::Params p = params(view, vis, s.viewProj);
    f32 centre[3];
    const bool hasCentre = resolve_kernel::camera_centre(s.viewProj.m, centre);
    const f64 centreErr = std::fabs(centre[0] - kEye[0]) + std::fabs(centre[1] - kEye[1]) + std::fabs(centre[2] - kEye[2]);
    expect(hasCentre && centreErr <= 1e-4, "camera_centre == the eye");
    f32 ortho[16] = {0.1f, 0.f, 0.f, 0.f, 0.f, 0.1f, 0.f, 0.f, 0.f, 0.f, 0.01f, 0.f, 0.f, 0.f, 0.5f, 1.f};
    f32 none[3];
    expect(!resolve_kernel::camera_centre(ortho, none), "no finite centre for an orthographic projection");

    std::vector<LayeredSurface> surfaces;
    layered_surfaces_reference(view, s.viewProj.m, s.viewProj.m, vis.data(), kW, kH, surfaces);
    MlLibrary lib;
    if (!ml_test::buildLibrary(FUSE_RP_ML_FIXTURE_DIR, lib)) {
        std::fprintf(stderr, "FAIL: fixture library\n");
        return 1;
    }
    MlMipChains chains;
    ml_build_mips(lib, chains);
    const MlTrilinearFilter filter{&chains, lib.textures().data(), static_cast<u32>(lib.textures().size()), 0.f};
    const MlView mv = ml_trilinear_view(lib, filter);
    u32 layered = 0;
    u32 plain = 0;
    u32 surfaceBad = 0;
    u32 kindBad = 0;
    u32 evalBad = 0;
    f64 reprojMax = 0.0;
    f64 derivMax = 0.0;
    f64 distMax = 0.0;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u32 pixel = y * kW + x;
            const u32 inst = vis[pixel * 2u];
            if (inst == visbuffer::kVisInvalid) {
                continue;
            }
            const ResolveAttributeTexel a = resolve_kernel::attributes(p, x, y, inst, vis[pixel * 2u + 1u]);
            if (a.flags != kAttrOk) {
                continue;
            }
            const bool rowLayered = a.material != kNoMaterial && gpu_scene::gpu_material_layered(s.rows[a.material]);
            const LayeredSurface& ls = surfaces[pixel];
            if (!rowLayered) {
                ++plain;
                kindBad += ls.valid ? 1u : 0u;
                continue;
            }
            ++layered;
            if (!ls.valid || ls.row != a.material || ls.surface.material != s.rows[a.material].padding ||
                ls.surface.color[0] != 1.f || ls.surface.color[3] != 1.f) {
                ++kindBad;
                continue;
            }
            // Reprojection of P onto the pixel centre.
            const f32* P = ls.surface.position;
            const f32* m = s.viewProj.m;
            const f64 cx = m[0] * P[0] + m[4] * P[1] + m[8] * P[2] + m[12];
            const f64 cy = m[1] * P[0] + m[5] * P[1] + m[9] * P[2] + m[13];
            const f64 cw = m[3] * P[0] + m[7] * P[1] + m[11] * P[2] + m[15];
            const f64 px = (cx / cw * 0.5 + 0.5) * kW;
            const f64 py = (cy / cw * 0.5 + 0.5) * kH;
            const f64 reproj = std::fabs(px - (x + 0.5)) + std::fabs(py - (y + 0.5));
            reprojMax = std::max(reprojMax, reproj);
            // Richardson-extrapolated central differences (steps 1 and 2 px, O(h^4)) on the same triangle's plane.
            const u32 tri = vis[pixel * 2u + 1u];
            f32 Q[8][3];
            f32 d0[3];
            f32 d1[3];
            const int offs[4] = {-2, -1, 1, 2};
            bool ok = x >= 2u && y >= 2u && x + 2u < kW && y + 2u < kH;
            for (u32 k = 0; ok && k < 4u; ++k) {
                ok = resolve_kernel::world_position(p, static_cast<u32>(static_cast<int>(x) + offs[k]), y, inst, tri, Q[k], d0, d1) &&
                     resolve_kernel::world_position(p, x, static_cast<u32>(static_cast<int>(y) + offs[k]), inst, tri, Q[4u + k], d0, d1);
            }
            if (ok) {
                const f32 gx[3] = {ls.grad.dPdx.x, ls.grad.dPdx.y, ls.grad.dPdx.z};
                const f32 gy[3] = {ls.grad.dPdy.x, ls.grad.dPdy.y, ls.grad.dPdy.z};
                f64 scale = 1e-6;
                f64 err = 0.0;
                for (u32 k = 0; k < 3u; ++k) {
                    const f64 dx1 = (static_cast<f64>(Q[2][k]) - Q[1][k]) * 0.5;
                    const f64 dx2 = (static_cast<f64>(Q[3][k]) - Q[0][k]) * 0.25;
                    const f64 dy1 = (static_cast<f64>(Q[6][k]) - Q[5][k]) * 0.5;
                    const f64 dy2 = (static_cast<f64>(Q[7][k]) - Q[4][k]) * 0.25;
                    const f64 cdx = (4.0 * dx1 - dx2) / 3.0;
                    const f64 cdy = (4.0 * dy1 - dy2) / 3.0;
                    scale = std::max({scale, std::fabs(cdx), std::fabs(cdy)});
                    err = std::max({err, std::fabs(cdx - gx[k]), std::fabs(cdy - gy[k])});
                }
                derivMax = std::max(derivMax, err / scale);
            }
            const f64 dist = std::sqrt(static_cast<f64>(P[0] - kEye[0]) * (P[0] - kEye[0]) +
                                       static_cast<f64>(P[1] - kEye[1]) * (P[1] - kEye[1]) +
                                       static_cast<f64>(P[2] - kEye[2]) * (P[2] - kEye[2]));
            distMax = std::max(distMax, std::fabs(dist - ls.surface.viewDistance) / dist);
            surfaceBad += reproj > 2e-3 ? 1u : 0u;
            // CPU reference evaluation of the layered bin.
            const MlResult r = ml_evaluate(mv, ls.surface, ls.grad);
            const f32 nl = std::sqrt(r.normal[0] * r.normal[0] + r.normal[1] * r.normal[1] + r.normal[2] * r.normal[2]);
            bool fine = std::fabs(nl - 1.f) <= 1e-4f && r.roughness >= 0.f && r.roughness <= 1.f && r.ao >= 0.f &&
                        r.ao <= 1.f && r.metallic >= 0.f && r.metallic <= 1.f;
            for (const f32 c : r.albedo) {
                fine = fine && std::isfinite(c) && c >= 0.f && c < 4.f;
            }
            evalBad += fine ? 0u : 1u;
        }
    }
    std::printf("surface: %u layered pixels, %u non-layered; reprojection max %.2e px, dP/dx,dy vs central differences "
                "max %.2e of scale, view distance max rel %.2e; camera centre error %.1e\n",
                layered, plain, reprojMax, derivMax, distMax, centreErr);
    expect(layered > 2000u && plain > 200u, "the scene has layered and non-layered pixels");
    expect(kindBad == 0u, "layered_surface exactly for layered rows (row, table index = padding, colour 1)");
    expect(surfaceBad == 0u, "the world position reprojects onto the pixel centre (2e-3 px)");
    expect(derivMax <= 1e-2, "analytic dP/dx, dP/dy == Richardson central differences (1% of scale; grazing ground pixels)");
    expect(distMax <= 1e-4, "view distance = |P - eye|");
    expect(evalBad == 0u, "the CPU reference evaluates every layered pixel to a finite, in-range surface");
    return 0;
}

int runClassify() {
    Scene s;
    if (!s.build()) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    std::vector<u32> vis;
    raster(s, vis);
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    const resolve_kernel::Params p = params(view, vis, s.viewProj);
    u32 bad = 0;
    u32 layeredPixels = 0;
    for (u32 pixel = 0; pixel < kW * kH; ++pixel) {
        u32 row = kNoMaterial;
        const u32 bin = resolve_kernel::pixel_bin(p, vis[pixel * 2u], vis[pixel * 2u + 1u], &row);
        const bool flagged = row != kNoMaterial && gpu_scene::gpu_material_layered(s.rows[row]);
        bad += (bin == kBinLayered) != flagged ? 1u : 0u;
        layeredPixels += bin == kBinLayered ? 1u : 0u;
    }
    expect(bad == 0u, "pixel bin == kBinLayered exactly for kGpuMaterialLayered rows");
    std::vector<u32> tileBins;
    classify_reference(view, vis.data(), kW, kH, tileBins);
    const u32 tilesX = (kW + kTileSize - 1u) / kTileSize;
    u32 tileBad = 0;
    u32 mixed = 0;
    u32 layeredTiles = 0;
    for (usize t = 0; t < tileBins.size(); ++t) {
        u32 maxBin = kBinEmpty;
        bool hasLayered = false;
        bool hasOther = false;
        for (u32 j = 0; j < kTileSize; ++j) {
            for (u32 i = 0; i < kTileSize; ++i) {
                const u32 x = static_cast<u32>(t % tilesX) * kTileSize + i;
                const u32 y = static_cast<u32>(t / tilesX) * kTileSize + j;
                if (x >= kW || y >= kH) {
                    continue;
                }
                const u32 b = resolve_kernel::pixel_bin(p, vis[(y * kW + x) * 2u], vis[(y * kW + x) * 2u + 1u]);
                maxBin = std::max(maxBin, b);
                hasLayered = hasLayered || b == kBinLayered;
                hasOther = hasOther || (b != kBinLayered && b != kBinEmpty);
            }
        }
        tileBad += maxBin != tileBins[t] ? 1u : 0u;
        mixed += hasLayered && hasOther ? 1u : 0u;
        layeredTiles += tileBins[t] == kBinLayered ? 1u : 0u;
    }
    std::vector<u32> list;
    layered_tile_list(tileBins, tilesX, list);
    std::vector<u32> lists[kBinCount];
    tile_lists(tileBins, tilesX, lists);
    usize listed = list.size();
    for (const std::vector<u32>& l : lists) {
        listed += l.size();
    }
    std::printf("classify: %u layered pixels, %u layered tiles (%u mixed with other bins), %zu tiles\n", layeredPixels,
                layeredTiles, mixed, tileBins.size());
    expect(tileBad == 0u, "tile bin == max pixel bin (the layered bin wins)");
    expect(list.size() == layeredTiles && listed == tileBins.size(), "every tile in exactly one list (layered appended)");
    expect(layeredTiles > 20u && mixed > 3u, "layered tiles and mixed tiles exist");
    return 0;
}

int runApi() {
    MaterialLayers layers;
    MlLibrary lib;
    expect(!layers.setLibrary(lib, nullptr) && layers.resolveTableHandle() == 0u, "setLibrary before init fails, no table");
    const ResolveFrameDesc rf{};
    expect(rf.layered == 0u, "ResolveFrameDesc::layered defaults to none");
    Material::GPUMaterial g{};
    gpu_scene::set_gpu_material_layered(g, 5u);
    expect(gpu_scene::gpu_material_layered(g) && gpu_scene::gpu_material_layered_index(g) == 5u &&
               resolve_kernel::material_bin(g) == kBinLayered,
           "set_gpu_material_layered marks the row, keeps the index, bins it as layered");
    g.normalTexIdx = 3u;
    expect(resolve_kernel::material_bin(g) == kBinLayered, "the layered flag wins over the texture fields");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (suite == "layout" || suite == "all") {
        rc |= runLayout();
    }
    if (suite == "mips" || suite == "all") {
        rc |= runMips();
    }
    if (suite == "surface" || suite == "all") {
        rc |= runSurface();
    }
    if (suite == "classify" || suite == "all") {
        rc |= runClassify();
    }
    if (suite == "api" || suite == "all") {
        rc |= runApi();
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures + rc);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
