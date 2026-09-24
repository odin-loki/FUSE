// WP-1.4 visibility-buffer CPU gates (no Vulkan; also run in the stub tree). The Lavapipe gates are in
// test_rp_visbuffer.cpp.
//
//   format     vis_format.hpp: bit layout, pack / unpack round trip, the quantised depth is the floor
//              (monotone, never above the depth), atomicMin order == (depth quantum, instance,
//              triangle) order, the clear word loses to every sample, the export depth is the far
//              end of the quantum (>= depth, < depth + 2^-24), raster unpack
//   indices    the scene index layout (WP-1.1 extension): addMeshletMesh appends MTRI-order triangle
//              lists, GpuMesh draw ranges are contiguous, every triangle t of the range is
//              MVRT[meshlet.vertexOffset + micro] of MTRI entry t, the ranges hold exactly the source
//              triangles (through VSRC, winding kept); make_draw uses the range, a bounds-only mesh
//              keeps the legacy {3 x triangleCount, 0, 0}
//   decode     the single-source decode kernel on a CPU-rastered image: every covered pixel decodes
//              (flag Ok) to the reference depth, its barycentrics are inside the triangle and project
//              back onto the pixel centre; bad ids -> BadId, empty -> Empty; CpuReference ==
//              CpuParallel bit for bit at 0 / 2 / 4 workers
//   reference  raster_reference: nearest surface wins, interiors are robust, silhouettes / shared
//              edges / coplanar ties are not, a near-plane crosser is handled and excluded
//   api        VisBuffer::init fails cleanly without a device; queryVisCapabilities(nullptr); layouts
#include "test_rp_visbuffer_meshes.hpp"

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/culling/instance_cull_kernel.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_meshlets.hpp>
#include <fuse/renderer/visbuffer/vis_decode_kernel.hpp>
#include <fuse/renderer/visbuffer/vis_format.hpp>
#include <fuse/renderer/visbuffer/vis_reference.hpp>
#include <fuse/renderer/visbuffer/visbuffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::visbuffer;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace kernel = fuse::kernel;

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

Mat4 camera(f32 ex, f32 ey, f32 ez, f32 ax, f32 ay, f32 az, f32 aspect) {
    const f32 eye[3] = {ex, ey, ez};
    const f32 at[3] = {ax, ay, az};
    return mul(perspective(1.1f, aspect, 0.5f, 120.f), lookAt(eye, at));
}

gpu_scene::GpuTransform place(f32 x, f32 y, f32 z, f32 s, f32 yaw = 0.f) {
    gpu_scene::GpuTransform t{};
    const f32 c = std::cos(yaw) * s;
    const f32 n = std::sin(yaw) * s;
    t.rows[0][0] = c;
    t.rows[0][2] = n;
    t.rows[1][1] = s;
    t.rows[2][0] = -n;
    t.rows[2][2] = c;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

/// A CPU-only GpuScene with three meshlet meshes (sphere, torus, box) and their VPOS views.
struct TestScene {
    gpu_scene::GpuScene scene;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<decode_kernel::MeshPositions> positions;

    bool init() {
        gpu_scene::GpuSceneDesc d{};
        if (!scene.init(d)) {
            return false;
        }
        const vis_test::SourceMesh sources[3] = {vis_test::uvSphere(12, 18, 1.f), vis_test::torus(20, 10, 1.f, 0.35f),
                                                 vis_test::box()};
        meshes.resize(3);
        for (u32 i = 0; i < 3u; ++i) {
            if (!vis_test::build(sources[i], meshes[i]) || scene.addMeshletMesh(meshes[i]) != i) {
                return false;
            }
        }
        for (const geometry::MeshletMesh& m : meshes) {
            positions.push_back(decode_kernel::MeshPositions{m.positions.data(), m.vertex_count()});
        }
        return true;
    }
    VisSceneView view() const { return vis_scene_view(scene, positions); }
};

// --- format -----------------------------------------------------------------------------------------
int runFormat() {
    expect(kVis64DepthShift == 40u && kVis64InstanceShift == 20u && kVis64IdMask == 0xFFFFFu, "bit layout 24 | 20 | 20");
    expect(kVisRasterFormat == 101u && kVis64ImageFormat == 110u && kVisDepthFormat == 126u, "VkFormat values");
    const u64 packed = vis64_pack(0.5f, 1234u, 56789u);
    expect(vis64_instance(packed) == 1234u && vis64_triangle(packed) == 56789u && vis64_depth_bits(packed) == (1u << 23),
           "pack / unpack fields");
    expect(packed == ((u64{1} << 23) << 40 | u64{1234} << 20 | 56789u), "packed word");
    const VisSample s = vis64_unpack(packed);
    expect(s.instance == 1234u && s.triangle == 56789u && vis_valid(s), "unpack sample");
    expect(!vis64_valid(kVis64Clear) && !vis_valid(vis64_unpack(kVis64Clear)) && vis64_export_depth(kVis64Clear) == 1.f,
           "clear word is invalid, depth 1");
    expect(vis64_quantize_depth(0.f) == 0u && vis64_quantize_depth(-1.f) == 0u && vis64_quantize_depth(1.f) == kVis64DepthMax &&
               vis64_quantize_depth(2.f) == kVis64DepthMax && vis64_quantize_depth(std::nanf("")) == 0u,
           "quantisation clamps");
    const VisSample r = vis_raster_unpack(kVisInvalid, 7u);
    expect(!vis_valid(r) && r.triangle == kVisInvalid && vis_raster_unpack(3u, 4u).triangle == 4u, "raster unpack");

    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> ud(0.f, 1.f);
    std::uniform_int_distribution<u32> uid(0u, kVis64IdMask - 1u);
    u32 orderChecks = 0;
    for (u32 i = 0; i < 200000u; ++i) {
        f32 d = ud(rng);
        if (i % 7u == 0u) {
            d = std::nextafter(1.f, 0.f) - static_cast<f32>(i % 13u) * 1e-8f; // crowd near the far plane
        }
        const u32 q = vis64_quantize_depth(d);
        const f32 lo = vis64_depth_floor(q);
        if (!(lo <= d && static_cast<f64>(d) < static_cast<f64>(lo) + 1.0 / 16777216.0) || q > kVis64DepthMax) {
            expect(false, "quantised depth is the floor of depth * 2^24");
            break;
        }
        const u32 inst = uid(rng);
        const u32 tri = uid(rng);
        const u64 v = vis64_pack(d, inst, tri);
        const f32 e = vis64_export_depth(v);
        if (vis64_instance(v) != inst || vis64_triangle(v) != tri || !(e >= d) ||
            !(static_cast<f64>(e) <= static_cast<f64>(d) + 1.0 / 16777216.0) || !(v < kVis64Clear)) {
            expect(false, "round trip / export depth bounds / below the clear word");
            break;
        }
        // atomicMin order: a nearer quantum always wins, equal quanta tie-break on (instance, triangle).
        const f32 d2 = ud(rng);
        const u64 w = vis64_pack(d2, uid(rng), uid(rng));
        const u32 q2 = vis64_quantize_depth(d2);
        if (q != q2) {
            ++orderChecks;
            if ((v < w) != (q < q2) || (d < d2 && q > q2)) {
                expect(false, "64-bit order follows the depth");
                break;
            }
        }
    }
    std::printf("format: 24 | 20 | 20, %u order checks, floor / export bounds on 200000 samples\n", orderChecks);
    return 0;
}

// --- indices ----------------------------------------------------------------------------------------
std::array<u32, 3> canonical(u32 a, u32 b, u32 c) {
    // Rotation with the smallest first element; the cyclic order (winding) is kept.
    if (a <= b && a <= c) {
        return {a, b, c};
    }
    if (b <= a && b <= c) {
        return {b, c, a};
    }
    return {c, a, b};
}

int runIndices() {
    TestScene t;
    expect(t.init(), "CPU-only scene with three meshlet meshes");
    const vis_test::SourceMesh sources[3] = {vis_test::uvSphere(12, 18, 1.f), vis_test::torus(20, 10, 1.f, 0.35f),
                                             vis_test::box()};
    u32 expectedFirst = 0;
    for (u32 m = 0; m < 3u; ++m) {
        const gpu_scene::GpuMesh& g = t.scene.mesh(m);
        const geometry::MeshletMesh& mm = t.meshes[m];
        expect(g.firstIndex == expectedFirst && g.indexCount == 3u * mm.triangle_count() && g.vertexOffset == 0,
               "contiguous draw ranges of 3 x triangleCount");
        expect(gpu_scene::meshDrawIndexCount(g) == g.indexCount, "meshDrawIndexCount uses the range");
        expectedFirst += g.indexCount;
        const u32* idx = t.scene.indexData() + g.firstIndex;
        bool layout = true;
        for (const geometry::MeshletRecord& r : mm.meshlets) {
            for (u32 k = 0; k < r.triangle_count; ++k) {
                const u32 tri = r.triangle_offset + k;
                const u32 packed = mm.meshlet_triangles[tri];
                for (u32 c = 0; c < 3u; ++c) {
                    layout = layout && idx[tri * 3u + c] == mm.meshlet_vertices[r.vertex_offset + geometry::triangle_index(packed, c)] &&
                             idx[tri * 3u + c] < mm.vertex_count();
                }
            }
        }
        expect(layout, "triangle t of the range == MTRI entry t through MVRT");
        // Same triangles as the source (through VSRC), winding kept.
        std::vector<std::array<u32, 3>> a, b;
        for (u32 k = 0; k < mm.triangle_count(); ++k) {
            a.push_back(canonical(mm.source_vertices[idx[k * 3u]], mm.source_vertices[idx[k * 3u + 1u]],
                                  mm.source_vertices[idx[k * 3u + 2u]]));
        }
        for (usize k = 0; k + 2u < sources[m].indices.size(); k += 3u) {
            b.push_back(canonical(sources[m].indices[k], sources[m].indices[k + 1u], sources[m].indices[k + 2u]));
        }
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        expect(a == b, "the range holds exactly the source triangles (winding kept)");
        const culling::DrawIndexedIndirectCommand d = culling::cull_kernel::make_draw(g, 42u);
        expect(d.indexCount == g.indexCount && d.firstIndex == g.firstIndex && d.vertexOffset == 0 && d.instanceCount == 1u &&
                   d.firstInstance == 42u,
               "make_draw uses the draw range");
    }
    expect(t.scene.indexCount() == expectedFirst && t.scene.header().indexAddress == 0u,
           "index mirror size; no GPU address in CPU-only mode");
    gpu_scene::GpuMesh legacy{};
    legacy.triangleCount = 12;
    const culling::DrawIndexedIndirectCommand d = culling::cull_kernel::make_draw(legacy, 5u);
    expect(d.indexCount == 36u && d.firstIndex == 0u && d.vertexOffset == 0 && d.firstInstance == 5u,
           "a bounds-only mesh keeps the legacy implicit range");
    std::printf("indices: 3 meshes, %u indices, ranges contiguous, MTRI order, source triangles preserved\n",
                t.scene.indexCount());
    return 0;
}

// --- decode -----------------------------------------------------------------------------------------
constexpr u32 kW = 160;
constexpr u32 kH = 112;

void addInstances(TestScene& t) {
    gpu_scene::InstanceDesc d{};
    const f32 xs[6] = {-3.f, 0.f, 3.f, -1.5f, 1.5f, 0.f};
    for (u32 i = 0; i < 6u; ++i) {
        d.mesh = i % 3u;
        d.transform = place(xs[i], i < 3u ? 0.8f : -1.2f, -8.f - static_cast<f32>(i), 1.2f, 0.3f * static_cast<f32>(i));
        t.scene.addInstance(d);
    }
    d.mesh = 2;
    d.transform = place(0.f, 0.f, -20.f, 6.f); // backdrop box
    t.scene.addInstance(d);
    d.flags = 0; // hidden: never drawn
    d.transform = place(0.f, 0.f, -5.f, 1.f);
    t.scene.addInstance(d);
}

int runDecode() {
    TestScene t;
    expect(t.init(), "scene");
    addInstances(t);
    const Mat4 vp = camera(0.3f, 0.5f, 2.f, 0.f, 0.f, -10.f, static_cast<f32>(kW) / kH);
    const VisSceneView view = t.view();
    std::vector<RasterRefPixel> ref;
    RasterRefStats st{};
    raster_reference(view, vp.m, kW, kH, RasterRefOptions{}, ref, &st);
    std::vector<u32> vis(static_cast<usize>(kW) * kH * 2u, kVisInvalid);
    for (usize p = 0; p < ref.size(); ++p) {
        vis[p * 2u] = ref[p].instance;
        vis[p * 2u + 1u] = ref[p].triangle;
    }
    std::vector<VisDecodeTexel> dec;
    decode_reference(view, vp.m, vis.data(), kW, kH, dec, kernel::Backend::CpuReference);
    f64 maxDepthErr = 0.0;
    f64 maxProjErr = 0.0;
    f64 minBary = 1.0;
    u32 covered = 0;
    bool flags = true;
    decode_kernel::Params p{};
    p.instances = view.instances;
    p.transforms = view.transforms;
    p.meshes = view.meshes;
    p.indices = view.indices;
    p.positions = view.positions;
    std::memcpy(p.viewProj, vp.m, sizeof(p.viewProj));
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const usize i = static_cast<usize>(y) * kW + x;
            if (ref[i].instance == kVisInvalid) {
                flags = flags && dec[i].flags == kVisDecodeEmpty && dec[i].depth == 1.f;
                continue;
            }
            ++covered;
            flags = flags && dec[i].flags == kVisDecodeOk;
            maxDepthErr = std::max(maxDepthErr, std::fabs(static_cast<f64>(dec[i].depth) - ref[i].depth));
            const f64 b[3] = {1.0 - dec[i].b1 - dec[i].b2, dec[i].b1, dec[i].b2};
            minBary = std::min({minBary, b[0], b[1], b[2]});
            decode_kernel::Clip c[3];
            decode_kernel::triangle_clip(p, ref[i].instance, ref[i].triangle, c);
            f64 P[4] = {0, 0, 0, 0};
            for (u32 k = 0; k < 3u; ++k) {
                P[0] += b[k] * c[k].x;
                P[1] += b[k] * c[k].y;
                P[3] += b[k] * c[k].w;
            }
            f32 nx = 0.f, ny = 0.f;
            decode_kernel::pixel_ndc(x, y, kW, kH, nx, ny);
            maxProjErr = std::max({maxProjErr, std::fabs(P[0] / P[3] - nx), std::fabs(P[1] / P[3] - ny)});
        }
    }
    expect(covered > kW * kH / 4u && st.robustCovered > covered / 2u, "scene covers the view");
    expect(flags, "flags: Ok where covered, Empty (depth 1) elsewhere");
    expect(maxDepthErr <= 2e-6, "decoded depth == reference depth");
    expect(minBary >= -1e-4, "barycentrics inside the triangle");
    expect(maxProjErr <= 1e-5, "barycentrics project onto the pixel centre");
    // Bad ids.
    std::vector<u32> bad = {7u, 0u, 1000u, 0u, 0u, 1u << 30, kVisInvalid, 3u};
    std::vector<VisDecodeTexel> badOut;
    decode_reference(view, vp.m, bad.data(), 4, 1, badOut);
    expect(badOut[0].flags == kVisDecodeOk && badOut[1].flags == kVisDecodeBadId && badOut[2].flags == kVisDecodeBadId &&
               badOut[3].flags == kVisDecodeEmpty,
           "hidden-but-valid instance decodes; out-of-range instance / triangle -> BadId");
    // Parity.
    fuse::jobs::JobScheduler& jobs = fuse::jobs::JobScheduler::instance();
    for (const u32 workers : {0u, 2u, 4u}) {
        jobs.shutdown();
        jobs.initialize(workers);
        std::vector<VisDecodeTexel> par;
        decode_reference(view, vp.m, vis.data(), kW, kH, par, kernel::Backend::CpuParallel);
        expect(std::memcmp(par.data(), dec.data(), dec.size() * sizeof(VisDecodeTexel)) == 0,
               "decode: CpuParallel == CpuReference");
    }
    jobs.shutdown();
    std::printf("decode: %u covered pixels (%u robust), max |depth - ref| %.3g, min barycentric %.3g, max reprojection "
                "%.3g; CpuParallel == CpuReference at 0/2/4 workers\n",
                covered, st.robustCovered, maxDepthErr, minBary, maxProjErr);
    return 0;
}

// --- reference --------------------------------------------------------------------------------------
int runReference() {
    TestScene t;
    expect(t.init(), "scene");
    gpu_scene::InstanceDesc d{};
    d.mesh = 2; // box
    d.transform = place(0.f, 0.f, -6.f, 1.f);
    t.scene.addInstance(d); // 0: front box
    d.transform = place(0.8f, 0.3f, -9.f, 1.5f);
    t.scene.addInstance(d); // 1: behind, partly visible
    d.transform = place(-3.f, 0.f, -6.f, 1.f);
    t.scene.addInstance(d); // 2: side box
    d.transform = place(-3.f, 0.f, -6.f, 1.f);
    t.scene.addInstance(d); // 3: exact duplicate of 2 (coplanar ties)
    const Mat4 vp = camera(0.f, 0.f, 0.f, 0.f, 0.f, -1.f, static_cast<f32>(kW) / kH);
    std::vector<RasterRefPixel> ref;
    RasterRefStats st{};
    raster_reference(t.view(), vp.m, kW, kH, RasterRefOptions{}, ref, &st);
    u32 front = 0, back = 0, dupRobust = 0, emptyRobust = 0;
    for (const RasterRefPixel& px : ref) {
        front += px.robust && px.instance == 0u;
        back += px.robust && px.instance == 1u;
        dupRobust += px.robust && (px.instance == 2u || px.instance == 3u);
        emptyRobust += px.robust && px.instance == kVisInvalid;
    }
    const RasterRefPixel& centre = ref[static_cast<usize>(kH / 2u) * kW + kW / 2u];
    expect(centre.instance == 0u && centre.robust == 1u, "front box wins the centre, robustly");
    expect(front > 100u && back > 20u && emptyRobust > 1000u, "front, back and empty pixels are robust");
    expect(dupRobust == 0u, "coplanar duplicates are never robust");
    expect(st.nearPlaneTriangles == 0u, "no near-plane triangles");
    // A box around the camera (its x = -1 side face crosses w = 0 in the left of the view).
    d.transform = place(2.f, 0.f, 0.f, 3.f);
    t.scene.addInstance(d);
    RasterRefStats st2{};
    raster_reference(t.view(), vp.m, kW, kH, RasterRefOptions{}, ref, &st2);
    u32 robust2 = 0;
    for (const RasterRefPixel& px : ref) {
        robust2 += px.robust;
    }
    // The camera box's front face (z = -3, every vertex in front) hides the rest in the centre; its
    // x = -1 side face crosses w = 0 and covers the left border, which is therefore never robust.
    const RasterRefPixel& centre2 = ref[static_cast<usize>(kH / 2u) * kW + kW / 2u];
    const RasterRefPixel& corner = ref[0];
    expect(st2.nearPlaneTriangles > 0u && centre2.instance == 4u && corner.instance == 4u && corner.robust == 0u,
           "near-plane crossers are rasterised (2DH) and excluded");
    std::printf("reference: %llu triangles, %u covered, %u robust (%u covered); with a box around the camera %u near-plane "
                "triangles, %u robust\n",
                static_cast<unsigned long long>(st.triangles), st.covered, st.robust, st.robustCovered,
                st2.nearPlaneTriangles, robust2);
    return 0;
}

// --- api --------------------------------------------------------------------------------------------
int runApi() {
    VisBuffer vb;
    expect(!vb.init(VisBufferDesc{}), "init without a device fails");
    expect(!vb.valid() && !vb.resize(64, 64), "an uninitialised VisBuffer refuses work");
    const f32 identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    expect(!vb.beginFrame(1, identity, 0u), "beginFrame without init fails");
    rg::Graph graph;
    const VisGraphRefs refs = vb.importInto(graph);
    expect(!refs.vis.valid() && !refs.depth.valid(), "no refs without init");
    const VisCapabilities caps = queryVisCapabilities(nullptr);
    expect(!caps.raster && !caps.atomicBuffer && !caps.atomicImage, "no capabilities without a device");
    expect(sizeof(VisRasterPush) == 96u && sizeof(VisDecodePush) == 96u && sizeof(Vis64Push) == 32u &&
               sizeof(VisDecodeTexel) == 16u && sizeof(gpu_scene::GpuMesh) == 144u &&
               sizeof(gpu_scene::GpuSceneHeader) == 144u,
           "record layouts");
    std::printf("api: init fails cleanly without a device (%s)\n", caps.rasterReason);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (suite == "format" || suite == "all") {
        rc |= runFormat();
    }
    if (suite == "indices" || suite == "all") {
        rc |= runIndices();
    }
    if (suite == "decode" || suite == "all") {
        rc |= runDecode();
    }
    if (suite == "reference" || suite == "all") {
        rc |= runReference();
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
