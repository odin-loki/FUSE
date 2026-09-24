// WP-1.5 material-resolve CPU gates (no Vulkan; also run in the stub tree). The Lavapipe gates are in
// test_rp_material_resolve.cpp.
//
//   layout      resolve_types.hpp records (frame constants, push, attribute texel, bin buffer), the bin
//               feature classes are nested, the G-buffer formats are GBufferLayout's
//   bary        resolve_kernel::bary on 20k random clip-space triangles (incl. vertices behind the
//               camera): b and depth are bit-identical to the visbuffer decode; the analytic
//               derivatives equal central finite differences of an f64 evaluation (relative 1e-3 of
//               the derivative scale); sum(db) = 0; the interpolated clip position projects onto the
//               pixel centre and its NDC derivative is exactly one pixel (2 / width) in x and 0 in y
//   attributes  "material_resolve_attributes" on a CPU-rastered scene (visbuffer raster_reference): every
//               covered pixel reconstructs (flag Ok); the material row == instance base + the submesh
//               material of the triangle's meshlet; the bin == the material's class; d(uv)/dx, d(uv)/dy
//               == central differences of the kernel's own UVs at neighbouring pixels on the same
//               triangle; the normal points along the face; velocity == pixel - previous projection of
//               the reconstructed world point (camera and object motion); CpuReference == CpuParallel
//               bit for bit at 0 / 2 / 4 workers
//   classify    "material_resolve_classify": tile bin == max pixel bin (brute force), every tile in
//               exactly one list, hand-made mixed tiles, CpuReference == CpuParallel
//   api         MaterialResolve::init fails cleanly without a device; queryResolveCapabilities(nullptr)
#include "test_rp_material_resolve_scene.hpp"

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/material_resolve/resolve_kernel.hpp>
#include <fuse/renderer/material_resolve/resolve_reference.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>
#include <fuse/renderer/visbuffer/vis_reference.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::material_resolve;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace kernel = fuse::kernel;
using visbuffer::decode_kernel::Clip;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- math (column-major 4x4, Vulkan clip space, forward depth) --------------------------------------
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

Mat4 perspective(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zFar / (zNear - zFar);
    p.m[11] = -1.f;
    p.m[14] = zNear * zFar / (zNear - zFar);
    return p;
}

Mat4 lookAt(const f32 eye[3], const f32 at[3]) {
    f32 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f32 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f32& v : f) {
        v /= fl;
    }
    const f32 up[3] = {0.f, 1.f, 0.f};
    f32 s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const f32 sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f32& v : s) {
        v /= sl;
    }
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
    return v;
}

Mat4 camera(u32 w, u32 h, f32 ex, f32 ey, f32 ez, f32 ax, f32 ay, f32 az) {
    const f32 eye[3] = {ex, ey, ez};
    const f32 at[3] = {ax, ay, az};
    return mul(perspective(1.1f, static_cast<f32>(w) / static_cast<f32>(h), 0.3f, 80.f), lookAt(eye, at));
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

// --- layout -----------------------------------------------------------------------------------------
int runLayout() {
    expect(sizeof(ResolveFrameConstants) == 176u && sizeof(ResolvePush) == 16u && sizeof(ResolveAttributeTexel) == 112u,
           "record sizes");
    expect(bin_features(kBinEmpty) == 0u && bin_features(kBinFlat) == 0u && bin_features(kBinTextured) == kFeatureTextures &&
               bin_features(kBinNormalMapped) == kFeatureAll && bin_features(kBinUber) == kFeatureAll,
           "bin feature classes are nested and the uber path has every feature");
    expect(ResolveBinLayout::kListOffset == 64u && ResolveBinLayout::bytes(10u) == 64u + 160u, "bin buffer layout");
    expect(pack_tile(3u, 7u) == (3u | (7u << 16)) && tile_x(pack_tile(3u, 7u)) == 3u && tile_y(pack_tile(3u, 7u)) == 7u,
           "tile packing");
    const GpuFormat expected[6] = {GpuFormat::R16G16B16A16Sfloat, GpuFormat::R8G8B8A8Unorm, GpuFormat::R8G8B8A8Unorm,
                                   GpuFormat::R16G16Sfloat,       GpuFormat::R32Sfloat,     GpuFormat::R16G16B16A16Sfloat};
    bool formats = GBufferLayout::attachmentCount() + 1u == kResolveColorAttachments;
    for (u32 i = 0; i < 6u; ++i) {
        formats = formats && GBufferLayout::format(static_cast<GBufferAttachment>(i)) == expected[i];
    }
    expect(formats, "the resolve writes the deferred G-buffer layout (RT0..RT5) + one material id attachment");
    expect(kMaterialIdFormat == 98u && kForwardColorAttachments == kResolveColorAttachments + 1u, "extra attachments");
    std::printf("layout: frame constants %zu B, attribute texel %zu B, bins 64 B + 4 x tiles x 4 B; G-buffer RT0..RT5 + "
                "R32_UINT material id\n",
                sizeof(ResolveFrameConstants), sizeof(ResolveAttributeTexel));
    return 0;
}

// --- bary -------------------------------------------------------------------------------------------
struct Bary64 {
    f64 b[3];
    bool ok;
};

Bary64 bary64(const Clip c[3], f64 nx, f64 ny) {
    f64 u[3][2];
    for (u32 k = 0; k < 3u; ++k) {
        u[k][0] = static_cast<f64>(c[k].x) - nx * c[k].w;
        u[k][1] = static_cast<f64>(c[k].y) - ny * c[k].w;
    }
    const f64 e0 = u[1][0] * u[2][1] - u[1][1] * u[2][0];
    const f64 e1 = u[2][0] * u[0][1] - u[2][1] * u[0][0];
    const f64 e2 = u[0][0] * u[1][1] - u[0][1] * u[1][0];
    const f64 s = e0 + e1 + e2;
    return Bary64{{e0 / s, e1 / s, e2 / s}, s != 0.0};
}

int runBary() {
    std::mt19937 rng(77);
    std::uniform_real_distribution<f32> uni(-1.f, 1.f);
    constexpr u32 kW = 320;
    constexpr u32 kH = 200;
    const f32 sx = 2.f / static_cast<f32>(kW);
    const f32 sy = 2.f / static_cast<f32>(kH);
    u32 tested = 0, behind = 0, badBits = 0, badDeriv = 0, badSum = 0, badProj = 0;
    f64 maxDerivErr = 0.0, maxProjErr = 0.0, maxProjDerivErr = 0.0;
    for (u32 iter = 0; iter < 20000u; ++iter) {
        Clip c[3];
        bool anyBehind = false;
        for (Clip& v : c) {
            v.w = 0.5f + 4.5f * (uni(rng) * 0.5f + 0.5f);
            if (iter % 5u == 0u && &v == &c[0]) {
                v.w = -0.2f - uni(rng) * 0.1f; // a vertex behind the camera
                anyBehind = true;
            }
            v.x = uni(rng) * 1.2f * std::fabs(v.w);
            v.y = uni(rng) * 1.2f * std::fabs(v.w);
            v.z = (0.1f + 0.8f * (uni(rng) * 0.5f + 0.5f)) * v.w;
        }
        const u32 px = static_cast<u32>((uni(rng) * 0.5f + 0.5f) * (kW - 1));
        const u32 py = static_cast<u32>((uni(rng) * 0.5f + 0.5f) * (kH - 1));
        f32 nx = 0.f, ny = 0.f;
        visbuffer::decode_kernel::pixel_ndc(px, py, kW, kH, nx, ny);
        const resolve_kernel::Bary b = resolve_kernel::bary(c[0], c[1], c[2], nx, ny, sx, sy);
        const visbuffer::VisDecodeTexel d = visbuffer::decode_kernel::decode_clip(c[0], c[1], c[2], nx, ny);
        if (b.flags != kAttrOk || d.flags != visbuffer::kVisDecodeOk) {
            continue;
        }
        const Bary64 r = bary64(c, nx, ny);
        // Well-conditioned samples only: |s| not tiny against its terms, pixel ray in front (W > 0).
        const f64 w = r.b[0] * c[0].w + r.b[1] * c[1].w + r.b[2] * c[2].w;
        if (!r.ok || !(w > 1e-3) || std::fabs(r.b[0]) > 50.0 || std::fabs(r.b[1]) > 50.0 || std::fabs(r.b[2]) > 50.0) {
            continue;
        }
        ++tested;
        behind += anyBehind ? 1u : 0u;
        badBits += (std::memcmp(&b.b[1], &d.b1, 4) != 0 || std::memcmp(&b.b[2], &d.b2, 4) != 0 ||
                    std::memcmp(&b.depth, &d.depth, 4) != 0)
                       ? 1u
                       : 0u;
        // Central differences in f64 (h = 1e-3 px).
        const f64 h = 1e-3;
        const Bary64 xp = bary64(c, nx + h * sx, ny), xm = bary64(c, nx - h * sx, ny);
        const Bary64 yp = bary64(c, nx, ny + h * sy), ym = bary64(c, nx, ny - h * sy);
        f64 scale = 0.0;
        f64 fdx[3], fdy[3];
        for (u32 k = 0; k < 3u; ++k) {
            fdx[k] = (xp.b[k] - xm.b[k]) / (2.0 * h);
            fdy[k] = (yp.b[k] - ym.b[k]) / (2.0 * h);
            scale = std::max({scale, std::fabs(fdx[k]), std::fabs(fdy[k])});
        }
        bool derivOk = true;
        for (u32 k = 0; k < 3u; ++k) {
            const f64 ex = std::fabs(b.dbdx[k] - fdx[k]);
            const f64 ey = std::fabs(b.dbdy[k] - fdy[k]);
            maxDerivErr = std::max(maxDerivErr, std::max(ex, ey) / std::max(scale, 1e-12));
            derivOk = derivOk && ex <= 1e-3 * scale + 1e-9 && ey <= 1e-3 * scale + 1e-9;
        }
        badDeriv += derivOk ? 0u : 1u;
        const f64 sumX = static_cast<f64>(b.dbdx[0]) + b.dbdx[1] + b.dbdx[2];
        const f64 sumY = static_cast<f64>(b.dbdy[0]) + b.dbdy[1] + b.dbdy[2];
        badSum += (std::fabs(sumX) <= 1e-4 * scale + 1e-9 && std::fabs(sumY) <= 1e-4 * scale + 1e-9) ? 0u : 1u;
        // The interpolated clip point projects onto the pixel centre; one pixel of x moves it by sx.
        f64 X = 0.0, Y = 0.0, W = 0.0, dX = 0.0, dW = 0.0, dYy = 0.0, dWy = 0.0, dXy = 0.0;
        for (u32 k = 0; k < 3u; ++k) {
            X += static_cast<f64>(b.b[k]) * c[k].x;
            Y += static_cast<f64>(b.b[k]) * c[k].y;
            W += static_cast<f64>(b.b[k]) * c[k].w;
            dX += static_cast<f64>(b.dbdx[k]) * c[k].x;
            dW += static_cast<f64>(b.dbdx[k]) * c[k].w;
            dXy += static_cast<f64>(b.dbdy[k]) * c[k].x;
            dYy += static_cast<f64>(b.dbdy[k]) * c[k].y;
            dWy += static_cast<f64>(b.dbdy[k]) * c[k].w;
        }
        const f64 projErr = std::max(std::fabs(X / W - nx), std::fabs(Y / W - ny));
        const f64 ddx = (dX * W - X * dW) / (W * W); // d(ndc.x)/dx
        const f64 ddxy = (dXy * W - X * dWy) / (W * W);
        const f64 ddyy = (dYy * W - Y * dWy) / (W * W);
        const f64 pdErr = std::max({std::fabs(ddx - sx), std::fabs(ddxy), std::fabs(ddyy - sy)}) / sx;
        maxProjErr = std::max(maxProjErr, projErr);
        maxProjDerivErr = std::max(maxProjDerivErr, pdErr);
        // f32 rounding of b scales with the size of the terms that cancel (condition numbers).
        f64 condP = 0.0, condD = 0.0;
        for (u32 k = 0; k < 3u; ++k) {
            const f64 mag = std::fabs(c[k].x) + std::fabs(c[k].y) + std::fabs(c[k].w);
            condP += std::fabs(b.b[k]) * mag;
            condD += (std::fabs(b.dbdx[k]) + std::fabs(b.dbdy[k])) * mag;
        }
        condP /= std::fabs(W);
        condD = condD / (std::fabs(W) * sx) + condP * std::fabs(dW) / std::fabs(W);
        badProj += (projErr <= 1e-5 * condP && pdErr <= 1e-4 * (1.0 + condD)) ? 0u : 1u;
    }
    expect(tested > 10000u && behind > 1000u, "enough well-conditioned samples, incl. vertices behind the camera");
    expect(badBits == 0u, "b1, b2, depth bit-identical to the visbuffer decode");
    expect(badDeriv == 0u, "analytic derivatives == f64 central differences");
    expect(badSum == 0u, "sum of the barycentric derivatives is 0");
    expect(badProj == 0u, "interpolated clip position projects onto the pixel; its NDC derivative is one pixel");
    std::printf("bary: %u samples (%u with a vertex behind the camera): b / depth bit-identical to the decode, max "
                "|analytic - fd| / scale %.3g, max reprojection %.3g, max |d(ndc)/dpx - 2/w| %.3g px\n",
                tested, behind, maxDerivErr, maxProjErr, maxProjDerivErr);
    return 0;
}

// --- scene ------------------------------------------------------------------------------------------
constexpr u32 kW = 160;
constexpr u32 kH = 112;

struct TestScene {
    gpu_scene::GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<ResolveMeshData> data;
    std::vector<resolve_kernel::MeshStreams> streams;
    std::vector<visbuffer::decode_kernel::MeshPositions> positions;
    std::vector<gpu_scene::InstanceHandle> handles;
    u32 movedInstance = 0;
    Mat4 viewProj{};
    Mat4 prevViewProj{};

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
        const std::vector<Material::GPUMaterial> mats = mr_test::makeMaterials(tex);
        for (u32 i = 0; i < mats.size(); ++i) {
            gpu.setMaterial(i, mats[i]);
        }
        auto add = [&](u32 mesh, u32 material, const gpu_scene::GpuTransform& t) {
            gpu_scene::InstanceDesc d{};
            d.mesh = mesh;
            d.material = material;
            d.transform = t;
            handles.push_back(gpu.addInstance(d));
        };
        add(3, mr_test::kMatLodProbe, place(0.f, -1.6f, -14.f, 1.f, 1.f, 1.f, 0.3f));
        add(0, mr_test::kMatNormalMapped, place(-2.2f, 0.f, -6.f, 1.f, 1.f, 1.f, 0.4f));
        add(1, mr_test::kMatTextured, place(1.8f, 0.2f, -7.f, 1.2f, 0.8f, 1.2f, 0.9f));
        add(2, mr_test::kMatAoEmissiveTex, place(0.2f, -0.4f, -9.f, 0.9f, 0.9f, 0.9f, 0.7f)); // submeshes 0 / 1
        add(0, mr_test::kMatFlat, place(3.5f, 1.2f, -11.f, -1.f, 1.f, 1.f, 0.f));             // mirrored
        add(0, mr_test::kMatEmissive, place(-4.f, 1.5f, -12.f, 0.7f, 1.4f, 0.7f, 0.f));
        add(1, gpu_scene::kInvalidIndex, place(-0.5f, 2.2f, -10.f, 1.f, 1.f, 1.f, 1.2f));       // default material
        movedInstance = 2;
        viewProj = camera(kW, kH, 0.f, 1.f, 3.f, 0.f, -0.5f, -8.f);
        prevViewProj = camera(kW, kH, -0.15f, 1.05f, 3.1f, 0.f, -0.5f, -8.f);
        if (!gpu.commit().ok) {
            return false;
        }
        // Frame 2: move one instance (prev = the old transform until the next beginFrame).
        gpu.beginFrame(2);
        gpu_scene::GpuTransform t = gpu.transform(movedInstance);
        t.rows[0][3] += 0.3f;
        t.rows[1][3] -= 0.1f;
        gpu.setTransform(handles[movedInstance], t);
        return gpu.commit().ok;
    }
};

/// CPU raster of the scene (instance, triangle per pixel) with robustness flags.
void rasterIds(TestScene& s, std::vector<u32>& vis, std::vector<visbuffer::RasterRefPixel>& ref) {
    const visbuffer::VisSceneView view = visbuffer::vis_scene_view(s.gpu, s.positions);
    visbuffer::raster_reference(view, s.viewProj.m, kW, kH, visbuffer::RasterRefOptions{}, ref);
    vis.assign(kW * kH * 2u, visbuffer::kVisInvalid);
    for (u32 p = 0; p < kW * kH; ++p) {
        vis[p * 2u] = ref[p].instance;
        vis[p * 2u + 1u] = ref[p].triangle;
    }
}

/// Independent material check: the submesh of the meshlet whose MTRI range holds `triangle`.
u32 expectedMaterial(const TestScene& s, u32 instance, u32 triangle) {
    const gpu_scene::GpuInstance& inst = s.gpu.instance(instance);
    if (inst.material == gpu_scene::kInvalidIndex) {
        return kNoMaterial;
    }
    const geometry::MeshletMesh& m = s.meshes[inst.mesh];
    for (const geometry::MeshletRecord& r : m.meshlets) {
        if (triangle >= r.triangle_offset && triangle < r.triangle_offset + r.triangle_count) {
            return inst.material + m.submeshes[r.submesh].material_index;
        }
    }
    return kNoMaterial;
}

// --- attributes -------------------------------------------------------------------------------------
int runAttributes() {
    TestScene s;
    if (!s.build()) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    std::vector<u32> vis;
    std::vector<visbuffer::RasterRefPixel> ref;
    rasterIds(s, vis, ref);
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    std::vector<ResolveAttributeTexel> out;
    attributes_reference(view, s.viewProj.m, s.prevViewProj.m, vis.data(), kW, kH, out);
    u32 covered = 0, notOk = 0, badMaterial = 0, badBin = 0, fdTested = 0, fdBad = 0, badNormal = 0, velTested = 0,
        velBad = 0;
    u32 binCounts[kBinCount] = {};
    f64 maxFdErr = 0.0, maxVelErr = 0.0;
    const Material::GPUMaterial* mats = view.materials.data;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u32 p = y * kW + x;
            const ResolveAttributeTexel& a = out[p];
            if (vis[p * 2u] == visbuffer::kVisInvalid) {
                expect(a.flags == kAttrEmpty && a.material == kNoMaterial && a.bin == kBinEmpty, "empty pixel");
                continue;
            }
            ++covered;
            if (a.flags != kAttrOk) {
                ++notOk;
                continue;
            }
            const u32 inst = vis[p * 2u];
            const u32 tri = vis[p * 2u + 1u];
            const u32 em = expectedMaterial(s, inst, tri);
            badMaterial += a.material != em ? 1u : 0u;
            const u32 eb = em == kNoMaterial ? kBinFlat : resolve_kernel::material_bin(mats[em]);
            badBin += a.bin != eb ? 1u : 0u;
            ++binCounts[a.bin];
            // UV derivatives against central differences over neighbours on the same triangle.
            if (x > 0u && x + 1u < kW && y > 0u && y + 1u < kH) {
                const u32 l = p - 1u, r = p + 1u, u = p - kW, d = p + kW;
                auto same = [&](u32 q) { return vis[q * 2u] == inst && vis[q * 2u + 1u] == tri && out[q].flags == kAttrOk; };
                if (same(l) && same(r) && same(u) && same(d)) {
                    ++fdTested;
                    const f64 scale = std::max({static_cast<f64>(std::fabs(a.duvdx[0])), static_cast<f64>(std::fabs(a.duvdx[1])),
                                                static_cast<f64>(std::fabs(a.duvdy[0])),
                                                static_cast<f64>(std::fabs(a.duvdy[1])), 1e-6});
                    f64 err = 0.0;
                    for (u32 k = 0; k < 2u; ++k) {
                        const f64 fx = (static_cast<f64>(out[r].uv[k]) - out[l].uv[k]) * 0.5;
                        const f64 fy = (static_cast<f64>(out[d].uv[k]) - out[u].uv[k]) * 0.5;
                        err = std::max({err, std::fabs(fx - a.duvdx[k]), std::fabs(fy - a.duvdy[k])});
                    }
                    maxFdErr = std::max(maxFdErr, err / scale);
                    fdBad += err <= 0.02 * scale + 2e-6 ? 0u : 1u; // O(h^2) of the perspective + f32 UVs
                }
            }
            // Normal: along the face normal of the world triangle (|cos| > 0.5 on these smooth meshes).
            const gpu_scene::GpuInstance& gi = s.gpu.instance(inst);
            const gpu_scene::GpuMesh& mesh = s.gpu.mesh(gi.mesh);
            const u32* idx = s.gpu.indexData() + mesh.firstIndex + tri * 3u;
            f64 wp[3][3];
            for (u32 k = 0; k < 3u; ++k) {
                f32 o[3];
                visbuffer::decode_kernel::mesh_position(mesh, s.data[gi.mesh].streams.vpos, idx[k], o);
                const gpu_scene::GpuTransform& t = s.gpu.transform(inst);
                for (u32 rr = 0; rr < 3u; ++rr) {
                    wp[k][rr] = static_cast<f64>(t.rows[rr][0]) * o[0] + static_cast<f64>(t.rows[rr][1]) * o[1] +
                                static_cast<f64>(t.rows[rr][2]) * o[2] + t.rows[rr][3];
                }
            }
            const f64 e1[3] = {wp[1][0] - wp[0][0], wp[1][1] - wp[0][1], wp[1][2] - wp[0][2]};
            const f64 e2[3] = {wp[2][0] - wp[0][0], wp[2][1] - wp[0][1], wp[2][2] - wp[0][2]};
            const f64 fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const f64 fl = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
            const f64 nl = std::sqrt(static_cast<f64>(a.normal[0]) * a.normal[0] + static_cast<f64>(a.normal[1]) * a.normal[1] +
                                     static_cast<f64>(a.normal[2]) * a.normal[2]);
            const f64 cosine = (fn[0] * a.normal[0] + fn[1] * a.normal[1] + fn[2] * a.normal[2]) / (fl * nl);
            badNormal += std::fabs(cosine) > 0.5 ? 0u : 1u;
            // Velocity: the reconstructed world point under last frame's transform and camera.
            const f64 bb[3] = {1.0 - static_cast<f64>(a.b1) - a.b2, a.b1, a.b2};
            f64 obj[3] = {0.0, 0.0, 0.0};
            for (u32 k = 0; k < 3u; ++k) {
                f32 o[3];
                visbuffer::decode_kernel::mesh_position(mesh, s.data[gi.mesh].streams.vpos, idx[k], o);
                for (u32 rr = 0; rr < 3u; ++rr) {
                    obj[rr] += bb[k] * o[rr];
                }
            }
            const gpu_scene::GpuTransform& pt = view.prevTransforms[inst];
            f64 pw[3];
            for (u32 rr = 0; rr < 3u; ++rr) {
                pw[rr] = pt.rows[rr][0] * obj[0] + pt.rows[rr][1] * obj[1] + pt.rows[rr][2] * obj[2] + pt.rows[rr][3];
            }
            const f32* m = s.prevViewProj.m;
            const f64 cx = m[0] * pw[0] + m[4] * pw[1] + m[8] * pw[2] + m[12];
            const f64 cy = m[1] * pw[0] + m[5] * pw[1] + m[9] * pw[2] + m[13];
            const f64 cw = m[3] * pw[0] + m[7] * pw[1] + m[11] * pw[2] + m[15];
            if (cw > 0.0) {
                ++velTested;
                const f64 vx = (x + 0.5) - (cx / cw * 0.5 + 0.5) * kW;
                const f64 vy = (y + 0.5) - (cy / cw * 0.5 + 0.5) * kH;
                const f64 e = std::max(std::fabs(vx - a.velocity[0]), std::fabs(vy - a.velocity[1]));
                maxVelErr = std::max(maxVelErr, e);
                velBad += e <= 2e-3 ? 0u : 1u;
            }
        }
    }
    expect(covered > kW * kH / 3u && notOk == 0u, "every covered pixel reconstructs");
    expect(badMaterial == 0u, "material row == instance base + the triangle's submesh material");
    expect(badBin == 0u, "bin == the material's feature class");
    expect(binCounts[kBinFlat] > 0u && binCounts[kBinTextured] > 0u && binCounts[kBinNormalMapped] > 0u,
           "the scene covers every bin");
    expect(fdTested > 1000u && fdBad == 0u, "UV derivatives == central differences of the reconstructed UVs");
    expect(badNormal == 0u, "interpolated normals follow the faces");
    expect(velTested > 1000u && velBad == 0u, "velocity == pixel - previous projection");
    // The moved instance and the camera motion both produce velocity.
    // Parity.
    fuse::jobs::JobScheduler& jobs = fuse::jobs::JobScheduler::instance();
    for (const u32 workers : {0u, 2u, 4u}) {
        jobs.shutdown();
        jobs.initialize(workers);
        std::vector<ResolveAttributeTexel> par;
        attributes_reference(view, s.viewProj.m, s.prevViewProj.m, vis.data(), kW, kH, par, kernel::Backend::CpuParallel);
        expect(std::memcmp(par.data(), out.data(), out.size() * sizeof(ResolveAttributeTexel)) == 0,
               "attributes: CpuParallel == CpuReference");
    }
    jobs.shutdown();
    // Bad ids.
    std::vector<u32> bad = {1000u, 0u, 1u, 1u << 30, visbuffer::kVisInvalid, 3u};
    std::vector<ResolveAttributeTexel> badOut;
    attributes_reference(view, s.viewProj.m, s.prevViewProj.m, bad.data(), 3, 1, badOut);
    expect(badOut[0].flags == kAttrBadId && badOut[1].flags == kAttrBadId && badOut[2].flags == kAttrEmpty &&
               badOut[0].bin == kBinEmpty,
           "out-of-range instance / triangle -> BadId (bin Empty), invalid -> Empty");
    std::printf("attributes: %u covered pixels (flat %u, textured %u, normal-mapped %u), %u UV-derivative checks (max "
                "|fd - analytic| / scale %.3g), %u velocity checks (max err %.3g px); CpuParallel == CpuReference at "
                "0/2/4 workers\n",
                covered, binCounts[kBinFlat], binCounts[kBinTextured], binCounts[kBinNormalMapped], fdTested, maxFdErr,
                velTested, maxVelErr);
    return 0;
}

// --- classify ---------------------------------------------------------------------------------------
int runClassify() {
    TestScene s;
    if (!s.build()) {
        std::fprintf(stderr, "FAIL: scene\n");
        return 1;
    }
    std::vector<u32> vis;
    std::vector<visbuffer::RasterRefPixel> ref;
    rasterIds(s, vis, ref);
    const ResolveSceneView view = resolve_scene_view(s.gpu, s.streams);
    std::vector<u32> bins;
    classify_reference(view, vis.data(), kW, kH, bins);
    const u32 tx = (kW + kTileSize - 1u) / kTileSize;
    const u32 ty = (kH + kTileSize - 1u) / kTileSize;
    expect(bins.size() == tx * ty, "one bin per tile");
    std::vector<ResolveAttributeTexel> attrs;
    attributes_reference(view, s.viewProj.m, s.prevViewProj.m, vis.data(), kW, kH, attrs);
    u32 wrong = 0;
    for (u32 t = 0; t < bins.size(); ++t) {
        u32 m = kBinEmpty;
        for (u32 j = 0; j < kTileSize; ++j) {
            for (u32 i = 0; i < kTileSize; ++i) {
                const u32 x = (t % tx) * kTileSize + i;
                const u32 y = (t / tx) * kTileSize + j;
                if (x < kW && y < kH && attrs[y * kW + x].flags == kAttrOk) {
                    m = std::max(m, attrs[y * kW + x].bin);
                }
            }
        }
        wrong += bins[t] != m ? 1u : 0u;
    }
    expect(wrong == 0u, "tile bin == max of the attribute kernel's pixel bins");
    std::vector<u32> lists[kBinCount];
    tile_lists(bins, tx, lists);
    usize total = 0;
    for (const std::vector<u32>& l : lists) {
        total += l.size();
    }
    expect(total == bins.size(), "every tile in exactly one list");
    // Hand-made: a 16 x 8 image, tile 0 = flat + normal-mapped pixel, tile 1 = empty + textured.
    std::vector<u32> tiny(16u * 8u * 2u, visbuffer::kVisInvalid);
    // Instance 4 (flat sphere), instance 1 (normal-mapped sphere), instance 2 (textured torus); triangle 0.
    tiny[(0u) * 2u] = 4u;
    tiny[(0u) * 2u + 1u] = 0u;
    tiny[(5u * 16u + 3u) * 2u] = 1u;
    tiny[(5u * 16u + 3u) * 2u + 1u] = 0u;
    tiny[(7u * 16u + 12u) * 2u] = 2u;
    tiny[(7u * 16u + 12u) * 2u + 1u] = 0u;
    std::vector<u32> tinyBins;
    classify_reference(view, tiny.data(), 16u, 8u, tinyBins);
    expect(tinyBins.size() == 2u && tinyBins[0] == kBinNormalMapped && tinyBins[1] == kBinTextured, "hand-made mixed tiles");
    fuse::jobs::JobScheduler& jobs = fuse::jobs::JobScheduler::instance();
    for (const u32 workers : {0u, 2u, 4u}) {
        jobs.shutdown();
        jobs.initialize(workers);
        std::vector<u32> par;
        classify_reference(view, vis.data(), kW, kH, par, kernel::Backend::CpuParallel);
        expect(par == bins, "classify: CpuParallel == CpuReference");
    }
    jobs.shutdown();
    std::printf("classify: %u tiles -> empty %zu, flat %zu, textured %zu, normal-mapped %zu; CpuParallel == CpuReference\n",
                tx * ty, lists[0].size(), lists[1].size(), lists[2].size(), lists[3].size());
    return 0;
}

// --- api --------------------------------------------------------------------------------------------
int runApi() {
    MaterialResolve r;
    expect(!r.init(MaterialResolveDesc{}), "init without a device fails");
    expect(!r.valid() && !r.resize(64, 64), "an uninitialised MaterialResolve refuses work");
    expect(!r.beginFrame(1, ResolveFrameDesc{}), "beginFrame without init fails");
    rg::Graph graph;
    const ResolveGraphRefs refs = r.importInto(graph);
    expect(!refs.bins.valid() && !refs.materialId.valid(), "no refs without init");
    const ResolveCapabilities caps = queryResolveCapabilities(nullptr);
    expect(!caps.resolve && !caps.forward, "no capabilities without a device");
    std::printf("api: init fails cleanly without a device (%s)\n", caps.reason);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (suite == "layout" || suite == "all") {
        rc |= runLayout();
    }
    if (suite == "bary" || suite == "all") {
        rc |= runBary();
    }
    if (suite == "attributes" || suite == "all") {
        rc |= runAttributes();
    }
    if (suite == "classify" || suite == "all") {
        rc |= runClassify();
    }
    if (suite == "api" || suite == "all") {
        rc |= runApi();
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures + rc);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
