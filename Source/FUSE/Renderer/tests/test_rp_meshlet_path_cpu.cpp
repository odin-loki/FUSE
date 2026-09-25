// WP-5.1 mesh-shader path, CPU gates (no device; also run in the stub tree). Lavapipe gates:
// test_rp_meshlet_path.cpp.
//
//   layout  record sizes / offsets, the shader-side constants (tolerance bit pattern, limits == the
//           WP-1.2 cook limits, reset words), MeshletDrawPush prefix == VisRasterPush
//   kernel  world_meshlet on the WP-1.2 meshlets of 3 meshes under 400 random transforms and 64
//           cameras: identity transform reproduces cull_meshlet on the object-space record exactly;
//           the world sphere contains every transformed vertex; is_similarity accepts rotation x
//           uniform scale (incl. mirrors) and rejects non-uniform scale / shear; every cone-culled
//           meshlet has ALL its triangles back-facing (f64, orientation-corrected for mirrors);
//           every frustum-culled meshlet has all its vertices outside one plane; cone skipped for
//           cutoff >= 1 and non-similar transforms
//   parity  reference kernel over task-group records: CpuReference == CpuParallel; phase rules
//           (occlusion off, no history, far / near Hi-Z, late re-test of deferred meshlets); the
//           strict / lenient bracket marks only boundary meshlets
//   select  the tier switch: T1 + task + mesh -> mesh shaders; tier cap T0 on T1 hardware, T0
//           hardware, missing features, Indirect mode, no caps -> the WP-1.4 fallback with a reason
//   api     init / queries fail cleanly without a device
#include "test_rp_visbuffer_meshes.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/culling/cull_reference.hpp>
#include <fuse/renderer/geometry/meshlet_cull_kernel.hpp>
#include <fuse/renderer/meshlet/meshlet_cull_kernel.hpp>
#include <fuse/renderer/meshlet/meshlet_path.hpp>
#include <fuse/renderer/meshlet/meshlet_reference.hpp>
#include <fuse/renderer/visbuffer/vis_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::meshlet;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using gpu_scene::GpuTransform;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

u32 bits(f32 v) {
    u32 b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

// --- shared fixtures -------------------------------------------------------------------------------
std::vector<geometry::MeshletMesh> buildMeshes() {
    std::vector<geometry::MeshletMesh> meshes(3);
    const vis_test::SourceMesh sources[3] = {vis_test::uvSphere(16, 24, 1.f), vis_test::torus(24, 12, 1.f, 0.35f),
                                             vis_test::box()};
    for (u32 i = 0; i < 3u; ++i) {
        if (!vis_test::build(sources[i], meshes[i])) {
            ++g_failures;
        }
    }
    return meshes;
}

void decodeVertex(const geometry::MeshletMesh& mesh, u32 v, f64 out[3]) {
    for (u32 a = 0; a < 3u; ++a) {
        out[a] = static_cast<f64>(mesh.quant.offset[a] + static_cast<f32>(mesh.positions[v * 4u + a]) * mesh.quant.step[a]);
    }
}

void apply(const GpuTransform& t, const f64 p[3], f64 out[3]) {
    for (u32 r = 0; r < 3u; ++r) {
        out[r] = t.rows[r][0] * p[0] + t.rows[r][1] * p[1] + t.rows[r][2] * p[2] + t.rows[r][3];
    }
}

GpuTransform rotationScale(std::mt19937& rng, f32 sx, f32 sy, f32 sz, bool mirror) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    // Random rotation from a normalised quaternion.
    f32 q[4] = {u(rng), u(rng), u(rng), u(rng)};
    const f32 n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (f32& c : q) {
        c /= n;
    }
    const f32 w = q[0], x = q[1], y = q[2], z = q[3];
    const f32 r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                         {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                         {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    const f32 s[3] = {mirror ? -sx : sx, sy, sz};
    GpuTransform t{};
    for (u32 row = 0; row < 3u; ++row) {
        for (u32 c = 0; c < 3u; ++c) {
            t.rows[row][c] = r[row][c] * s[c];
        }
        t.rows[row][3] = u(rng) * 20.f;
    }
    return t;
}

/// Frustum looking from `e` at `at` (column-major viewProj, Vulkan clip space, forward depth).
void lookView(const f32 e[3], const f32 at[3], f32 viewProj[16], f32 eye[3]) {
    f32 f[3] = {at[0] - e[0], at[1] - e[1], at[2] - e[2]};
    f32 l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f32& c : f) {
        c /= l;
    }
    f32 s[3] = {-f[2], 0.f, f[0]}; // f x (0, 1, 0)
    l = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f32& c : s) {
        c /= l;
    }
    const f32 up[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    f32 view[16] = {};
    view[0] = s[0], view[4] = s[1], view[8] = s[2];
    view[1] = up[0], view[5] = up[1], view[9] = up[2];
    view[2] = -f[0], view[6] = -f[1], view[10] = -f[2];
    view[12] = -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]);
    view[13] = -(up[0] * e[0] + up[1] * e[1] + up[2] * e[2]);
    view[14] = f[0] * e[0] + f[1] * e[1] + f[2] * e[2];
    view[15] = 1.f;
    const f32 zn = 0.5f, zf = 200.f, fy = 1.f / std::tan(0.5f), aspect = 1.3f;
    f32 proj[16] = {};
    proj[0] = fy / aspect;
    proj[5] = -fy;
    proj[10] = zf / (zn - zf);
    proj[11] = -1.f;
    proj[14] = zn * zf / (zn - zf);
    for (u32 c = 0; c < 4u; ++c) {
        for (u32 r = 0; r < 4u; ++r) {
            f32 acc = 0.f;
            for (u32 k = 0; k < 4u; ++k) {
                acc += proj[k * 4u + r] * view[c * 4u + k];
            }
            viewProj[c * 4u + r] = acc;
        }
    }
    eye[0] = e[0];
    eye[1] = e[1];
    eye[2] = e[2];
}

/// Random frustum around the origin region + its eye.
void randomView(std::mt19937& rng, f32 viewProj[16], f32 eye[3]) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    const f32 e[3] = {u(rng) * 30.f, u(rng) * 30.f, u(rng) * 30.f};
    const f32 at[3] = {u(rng) * 8.f, u(rng) * 8.f, u(rng) * 8.f};
    lookView(e, at, viewProj, eye);
}

MeshletConstants makeConstants(const f32 viewProj[16], const f32 eye[3], u32 flags, u32 cullFlags) {
    MeshletConstants c{};
    const f32 zero[3] = {0.f, 0.f, 0.f};
    const geometry::cull_kernel::CullView v = geometry::cull_kernel::make_cull_view(viewProj, zero);
    std::memcpy(c.cull.planes, v.planes, sizeof(c.cull.planes));
    std::memcpy(c.cull.viewProj, viewProj, sizeof(c.cull.viewProj));
    std::memcpy(c.cull.prevViewProj, viewProj, sizeof(c.cull.prevViewProj));
    c.cull.flags = cullFlags;
    c.camera[0] = eye[0];
    c.camera[1] = eye[1];
    c.camera[2] = eye[2];
    c.flags = flags;
    return c;
}

// --- layout ----------------------------------------------------------------------------------------
void testLayout() {
    expect(bits(kMeshletSimilarityTolerance) == 0x38D1B717u, "tolerance bit pattern == the shaders' 0x38D1B717");
    expect(kMeshletMaxVertices == geometry::kMeshletMaxVertices && kMeshletMaxTriangles == geometry::kMeshletMaxTriangles,
           "mesh output limits == the WP-1.2 cook limits");
    expect(kMeshletTaskGroup == 32u, "one deferral-mask bit per task invocation");
    expect(sizeof(MeshletConstants) == 400u && offsetof(MeshletConstants, camera) == sizeof(culling::CullConstants),
           "MeshletConstants = CullConstants + tail at 352");
    expect(offsetof(MeshletDrawPush, scene) == offsetof(visbuffer::VisRasterPush, scene) &&
               offsetof(MeshletDrawPush, target64) == offsetof(visbuffer::VisRasterPush, target64) &&
               offsetof(MeshletDrawPush, width) == offsetof(visbuffer::VisRasterPush, width) &&
               sizeof(MeshletDrawPush) == sizeof(visbuffer::VisRasterPush),
           "MeshletDrawPush prefix == VisRasterPush (the WP-1.4 fragment shaders read it)");
    expect(kMeshletCountTasks1 == 3u && kMeshletCountWords == 16u, "counts layout: two 3-word task commands first");
    std::printf("layout: ok\n");
}

// --- kernel ----------------------------------------------------------------------------------------
void testKernel() {
    const std::vector<geometry::MeshletMesh> meshes = buildMeshes();
    std::mt19937 rng(51);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    // (1) identity: world record == object record for sphere and apex, and the decision equals the
    // WP-1.2 kernel on the object-space record wherever the cone test is not on its boundary.
    {
        GpuTransform id{};
        u32 same = 0, total = 0, boundary = 0;
        for (u32 v = 0; v < 64u; ++v) {
            f32 vp[16], eye[3];
            randomView(rng, vp, eye);
            const MeshletConstants c = makeConstants(vp, eye, kMeshletCullFrustum | kMeshletCullCone, 0u);
            geometry::cull_kernel::CullView view{};
            std::memcpy(view.planes, c.cull.planes, sizeof(view.planes));
            std::memcpy(view.camera, eye, sizeof(view.camera));
            for (const geometry::MeshletMesh& mesh : meshes) {
                for (const geometry::MeshletRecord& m : mesh.meshlets) {
                    const cull_kernel::WorldMeshlet w = cull_kernel::world_meshlet(id, m, c.flags, 1.f, 1.f);
                    expect(std::memcmp(w.record.center, m.center, sizeof(m.center)) == 0 && bits(w.record.radius) == bits(m.radius) &&
                               std::memcmp(w.record.cone_apex, m.cone_apex, sizeof(m.cone_apex)) == 0 &&
                               std::memcmp(w.record.cone_axis, m.cone_axis, sizeof(m.cone_axis)) == 0,
                           "identity transform keeps sphere, apex and axis bit for bit");
                    view.flags = w.tests;
                    const u32 ref = geometry::cull_kernel::cull_meshlet(m, view);
                    const u32 got = cull_kernel::frustum_cone(w, c);
                    const bool refVisible = ref == geometry::cull_kernel::kVisible;
                    const u32 refResult = refVisible ? kMeshletResultNone
                                                     : ((ref & geometry::cull_kernel::kCulledFrustum) != 0u ? kMeshletResultFrustumCulled
                                                                                                          : kMeshletResultConeCulled);
                    // Boundary: cutoff' = cutoff * |axis| differs from cutoff by the axis' rounding.
                    const f64 d[3] = {static_cast<f64>(m.cone_apex[0]) - eye[0], static_cast<f64>(m.cone_apex[1]) - eye[1],
                                      static_cast<f64>(m.cone_apex[2]) - eye[2]};
                    const f64 dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    const f64 lhs = d[0] * m.cone_axis[0] + d[1] * m.cone_axis[1] + d[2] * m.cone_axis[2];
                    const bool nearCone = std::fabs(lhs - static_cast<f64>(m.cone_cutoff) * dl) <= 1e-5 * (dl + 1.0);
                    ++total;
                    if (nearCone && (w.tests & geometry::cull_kernel::kTestCone) != 0u) {
                        ++boundary;
                        continue;
                    }
                    same += got == refResult ? 1u : 0u;
                }
            }
        }
        std::printf("kernel: identity transform: %u / %u decisions == cull_meshlet on the object record (%u on the cone "
                    "boundary skipped)\n",
                    same, total - boundary, boundary);
        expect(same == total - boundary, "identity transform == WP-1.2 cull_meshlet");
    }
    // (2) similarity detection.
    {
        u32 ok = 0;
        for (u32 i = 0; i < 200u; ++i) {
            const f32 s = 0.2f + 3.f * u(rng);
            ok += cull_kernel::is_similarity(rotationScale(rng, s, s, s, (i & 1u) != 0u)) ? 1u : 0u;
            ok += !cull_kernel::is_similarity(rotationScale(rng, s, s * 1.3f, s, false)) ? 1u : 0u;
            GpuTransform shear = rotationScale(rng, s, s, s, false);
            shear.rows[0][1] += 0.2f * s;
            ok += !cull_kernel::is_similarity(shear) ? 1u : 0u;
        }
        GpuTransform zero{};
        zero.rows[0][0] = zero.rows[1][1] = zero.rows[2][2] = 0.f;
        expect(!cull_kernel::is_similarity(zero), "a zero linear part is not a similarity");
        expect(ok == 600u, "is_similarity: rotation x uniform scale (incl. mirrors) yes, non-uniform / shear no");
        std::printf("kernel: similarity classification %u / 600\n", ok);
    }
    // (3) conservativeness under random transforms and views.
    u32 coneCulled = 0, frustumCulled = 0, visible = 0, badCone = 0, badFrustum = 0, badSphere = 0, coneSkipped = 0;
    for (u32 trial = 0; trial < 400u; ++trial) {
        const f32 s = 0.3f + 2.f * u(rng);
        const u32 kind = trial % 4u; // 0 uniform, 1 mirrored uniform, 2 non-uniform, 3 uniform
        const GpuTransform t = rotationScale(rng, s, kind == 2u ? s * 1.7f : s, s, kind == 1u);
        const bool similar = cull_kernel::is_similarity(t);
        expect(similar == (kind != 2u), "test transforms classified as built");
        f64 det = 0.0;
        {
            const f64 a[3][3] = {{t.rows[0][0], t.rows[0][1], t.rows[0][2]},
                                 {t.rows[1][0], t.rows[1][1], t.rows[1][2]},
                                 {t.rows[2][0], t.rows[2][1], t.rows[2][2]}};
            det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                  a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        }
        f32 vp[16], eye[3];
        randomView(rng, vp, eye);
        const MeshletConstants c = makeConstants(vp, eye, kMeshletCullFrustum | kMeshletCullCone, 0u);
        const geometry::MeshletMesh& mesh = meshes[trial % 3u];
        for (const geometry::MeshletRecord& m : mesh.meshlets) {
            const cull_kernel::WorldMeshlet w = cull_kernel::world_meshlet(t, m, c.flags, 1.f, 1.f);
            if (!similar || m.cone_cutoff >= 1.f) {
                coneSkipped += (w.tests & geometry::cull_kernel::kTestCone) == 0u ? 1u : 0u;
                expect((w.tests & geometry::cull_kernel::kTestCone) == 0u, "cone skipped (non-similar or cutoff >= 1)");
            }
            // World sphere contains every transformed vertex.
            for (u32 k = 0; k < m.vertex_count; ++k) {
                f64 p[3], q[3];
                decodeVertex(mesh, mesh.meshlet_vertices[m.vertex_offset + k], p);
                apply(t, p, q);
                const f64 dx = q[0] - w.record.center[0], dy = q[1] - w.record.center[1], dz = q[2] - w.record.center[2];
                badSphere += std::sqrt(dx * dx + dy * dy + dz * dz) <= static_cast<f64>(w.record.radius) * (1.0 + 1e-5) + 1e-5 ? 0u : 1u;
            }
            const u32 r = cull_kernel::frustum_cone(w, c);
            if (r == kMeshletResultNone) {
                ++visible;
                continue;
            }
            if (r == kMeshletResultFrustumCulled) {
                ++frustumCulled;
                // Some plane has every vertex strictly outside (f64).
                bool anyPlane = false;
                for (u32 pl = 0; pl < 6u && !anyPlane; ++pl) {
                    bool all = true;
                    for (u32 k = 0; k < m.vertex_count && all; ++k) {
                        f64 p[3], q[3];
                        decodeVertex(mesh, mesh.meshlet_vertices[m.vertex_offset + k], p);
                        apply(t, p, q);
                        const f32* P = c.cull.planes[pl];
                        all = P[0] * q[0] + P[1] * q[1] + P[2] * q[2] + P[3] < 1e-5;
                    }
                    anyPlane = all;
                }
                badFrustum += anyPlane ? 0u : 1u;
                continue;
            }
            ++coneCulled;
            // Every triangle back-facing from the eye (winding normal, sign-corrected for mirrors).
            for (u32 k = 0; k < m.triangle_count; ++k) {
                const u32 packed = mesh.meshlet_triangles[m.triangle_offset + k];
                f64 v[3][3];
                for (u32 corner = 0; corner < 3u; ++corner) {
                    f64 p[3];
                    decodeVertex(mesh, mesh.meshlet_vertices[m.vertex_offset + geometry::triangle_index(packed, corner)], p);
                    apply(t, p, v[corner]);
                }
                const f64 e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]};
                const f64 e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
                const f64 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const f64 sign = det < 0.0 ? -1.0 : 1.0;
                const f64 facing = sign * (n[0] * (v[0][0] - eye[0]) + n[1] * (v[0][1] - eye[1]) + n[2] * (v[0][2] - eye[2]));
                const f64 scale = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                badCone += facing >= -1e-4 * scale * 50.0 ? 0u : 1u;
            }
        }
    }
    std::printf("kernel: 400 transforms x views: %u visible, %u frustum-culled, %u cone-culled, %u cone tests skipped;\n"
                "        violations: sphere %u, frustum %u, cone (front-facing triangle in a culled meshlet) %u\n",
                visible, frustumCulled, coneCulled, coneSkipped, badSphere, badFrustum, badCone);
    expect(coneCulled > 0u && frustumCulled > 0u && visible > 0u, "every outcome occurs");
    expect(badSphere == 0u, "world sphere contains every transformed vertex");
    expect(badFrustum == 0u, "frustum-culled meshlets are entirely outside one plane");
    expect(badCone == 0u, "cone-culled meshlets are entirely back-facing (similarity transforms, incl. mirrors)");
}

// --- parity ----------------------------------------------------------------------------------------
struct FlatHiz {
    culling::HizPyramid pyramid;
    explicit FlatHiz(f32 value) {
        pyramid.dim0 = 64u;
        pyramid.mipCount = 7u;
        for (u32 m = 0; m < 7u; ++m) {
            const u32 dim = 64u >> m;
            pyramid.levels[m].assign(static_cast<usize>(dim) * dim, value);
        }
    }
};

void testParity() {
    const std::vector<geometry::MeshletMesh> meshes = buildMeshes();
    std::vector<fuse::kernel::Span<const geometry::MeshletRecord>> spans;
    for (const geometry::MeshletMesh& m : meshes) {
        spans.push_back({m.meshlets.data(), static_cast<u32>(m.meshlets.size())});
    }
    std::mt19937 rng(77);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    constexpr u32 kInstances = 120;
    std::vector<gpu_scene::GpuInstance> instances(kInstances);
    std::vector<GpuTransform> xf(kInstances), prev(kInstances);
    std::vector<MeshletGroup> groups;
    for (u32 i = 0; i < kInstances; ++i) {
        instances[i].mesh = i % 3u;
        instances[i].flags = gpu_scene::kInstanceValid | gpu_scene::kInstanceVisible;
        const f32 s = 0.5f + 0.4f * (u(rng) + 1.f);
        xf[i] = rotationScale(rng, s, (i % 5u) == 0u ? s * 1.5f : s, s, (i % 7u) == 0u);
        xf[i].rows[2][3] = -10.f - 20.f * (u(rng) + 1.f);
        prev[i] = xf[i];
        prev[i].rows[0][3] += 0.3f;
        const u32 n = static_cast<u32>(meshes[i % 3u].meshlets.size());
        for (u32 first = 0; first < n; first += kMeshletTaskGroup) {
            groups.push_back(MeshletGroup{i, first});
        }
    }
    f32 vp[16], eye[3];
    const f32 e[3] = {0.f, 0.f, 20.f};
    const f32 at[3] = {0.f, 0.f, -30.f};
    lookView(e, at, vp, eye);
    FlatHiz far(1.f), near(0.f);
    MeshletReferenceInput in{};
    in.scene.instances = {instances.data(), kInstances};
    in.scene.transforms = {xf.data(), kInstances};
    in.scene.prevTransforms = {prev.data(), kInstances};
    in.meshlets = {spans.data(), static_cast<u32>(spans.size())};
    in.groups = {groups.data(), static_cast<u32>(groups.size())};
    auto run = [&](u32 flags, u32 cullFlags, const FlatHiz& prevHiz, const FlatHiz& hiz, u32 region,
                   fuse::kernel::Backend backend) {
        MeshletConstants c = makeConstants(vp, eye, flags, cullFlags);
        c.cull.hizDim = 64u;
        c.cull.hizMipCount = 7u;
        c.cull.hizScale[0] = 64.f;
        c.cull.hizScale[1] = 48.f;
        in.constants = &c;
        in.prevHiz = prevHiz.pyramid.view();
        in.hiz = hiz.pyramid.view();
        in.region = region;
        std::vector<u32> r;
        meshlet_cull_reference(in, 1.f, 1.f, r, backend);
        in.constants = nullptr;
        return r;
    };
    auto histogram = [](const std::vector<u32>& r, u32 (&h)[8]) {
        std::fill(std::begin(h), std::end(h), 0u);
        for (u32 v : r) {
            ++h[v < 8u ? v : 0u];
        }
    };
    const u32 all = kMeshletCullFrustum | kMeshletCullCone | kMeshletCullOcclusion;
    const u32 occl = culling::kCullOcclusion | culling::kCullHistoryValid | culling::kCullFrustum;
    // CpuReference == CpuParallel.
    const std::vector<u32> a = run(all, occl, near, far, 0u, fuse::kernel::Backend::CpuReference);
    const std::vector<u32> b = run(all, occl, near, far, 0u, fuse::kernel::Backend::CpuParallel);
    expect(a == b, "CpuReference == CpuParallel");
    u32 h[8];
    histogram(a, h);
    std::printf("parity: %zu records; region 0, prev Hi-Z near / cur far: none %u frustum %u cone %u p1 %u p2 %u occluded %u\n",
                groups.size(), h[0], h[1], h[2], h[3], h[5], h[6]);
    expect(h[kMeshletResultPhase1Drawn] == 0u && h[kMeshletResultPhase2Drawn] > 0u,
           "near last-frame Hi-Z defers everything, the far current Hi-Z draws it late");
    const std::vector<u32> c1 = run(all, occl, far, near, 0u, fuse::kernel::Backend::CpuReference);
    histogram(c1, h);
    expect(h[kMeshletResultPhase1Drawn] > 0u && h[kMeshletResultPhase2Drawn] == 0u && h[kMeshletResultOccluded] == 0u,
           "far last-frame Hi-Z draws everything early");
    const std::vector<u32> c2 = run(all, occl, near, near, 1u, fuse::kernel::Backend::CpuReference);
    histogram(c2, h);
    expect(h[kMeshletResultOccluded] > 0u && h[kMeshletResultPhase2Drawn] == 0u, "region 1 against a near Hi-Z: occluded");
    const std::vector<u32> c3 = run(kMeshletCullFrustum | kMeshletCullCone, occl, near, near, 0u, fuse::kernel::Backend::CpuReference);
    histogram(c3, h);
    expect(h[kMeshletResultPhase1Drawn] > 0u && h[kMeshletResultDeferred] == 0u && h[kMeshletResultOccluded] == 0u,
           "meshlet occlusion off: early draws every frustum / cone survivor");
    const std::vector<u32> c4 = run(all, culling::kCullOcclusion | culling::kCullFrustum, far, far, 0u, fuse::kernel::Backend::CpuReference);
    histogram(c4, h);
    expect(h[kMeshletResultPhase1Drawn] == 0u && h[kMeshletResultPhase2Drawn] > 0u, "no history: deferred, drawn late");
    const std::vector<u32> c5 = run(kMeshletCullFrustum, occl, far, far, 0u, fuse::kernel::Backend::CpuReference);
    histogram(c5, h);
    expect(h[kMeshletResultConeCulled] == 0u, "cone off: nothing cone-culled");
    // Strict / lenient bracket.
    MeshletConstants c = makeConstants(vp, eye, all, occl);
    c.cull.hizDim = 64u;
    c.cull.hizMipCount = 7u;
    c.cull.hizScale[0] = 64.f;
    c.cull.hizScale[1] = 48.f;
    FlatHiz mid(0.99f);
    in.constants = &c;
    in.prevHiz = mid.pyramid.view();
    in.hiz = mid.pyramid.view();
    in.region = 0u;
    MeshletParityReference pr;
    meshlet_cull_reference_parity(in, pr);
    std::printf("parity: bracket over %zu meshlet slots: %u ambiguous\n", pr.results.size(), pr.ambiguousCount);
    expect(pr.ambiguousCount * 50u <= pr.results.size(), "at most 2% of the slots sit on a test boundary");
}

// --- select ----------------------------------------------------------------------------------------
void testSelect() {
    RendererCaps caps{};
    MeshletPathSelection s = selectMeshletPath(caps, MeshletPathMode::Auto);
    expect(!s.meshShaders && std::strstr(s.reason, "no device") != nullptr, "no caps -> fallback");
    caps.valid = true;
    caps.hardwareTier = RenderTier::T1;
    caps.tier = RenderTier::T1;
    caps.setEnabledMask(renderT0RequiredMask() | renderFeatureBit(RenderFeature::TaskShader) |
                        renderFeatureBit(RenderFeature::MeshShader));
    s = selectMeshletPath(caps, MeshletPathMode::Auto);
    expect(s.meshShaders && std::strcmp(s.reason, "ok") == 0, "T1 + task + mesh -> mesh shaders");
    expect(selectMeshletPath(caps, MeshletPathMode::MeshShader).meshShaders, "MeshShader mode on T1");
    s = selectMeshletPath(caps, MeshletPathMode::Indirect);
    expect(!s.meshShaders && std::strstr(s.reason, "Indirect") != nullptr, "Indirect mode -> fallback");
    RendererCaps capped = caps;
    capped.tier = RenderTier::T0;
    capped.tierCap = RenderTier::T0;
    capped.setEnabledMask(renderT0RequiredMask());
    s = selectMeshletPath(capped, MeshletPathMode::Auto);
    expect(!s.meshShaders && std::strstr(s.reason, "tier cap") != nullptr, "T1 hardware capped at T0 -> fallback (tier cap)");
    RendererCaps t0 = capped;
    t0.hardwareTier = RenderTier::T0;
    s = selectMeshletPath(t0, MeshletPathMode::Auto);
    expect(!s.meshShaders && std::strstr(s.reason, "device is T0") != nullptr, "T0 hardware -> fallback");
    RendererCaps noTask = caps;
    noTask.setEnabledMask(renderT0RequiredMask() | renderFeatureBit(RenderFeature::MeshShader));
    s = selectMeshletPath(noTask, MeshletPathMode::Auto);
    expect(!s.meshShaders, "mesh without task -> fallback");
    std::printf("select: ok\n");
}

// --- api -------------------------------------------------------------------------------------------
void testApi() {
    const MeshletCapabilities caps = queryMeshletCapabilities(nullptr);
    expect(!caps.meshPath, "no device: no mesh path");
    MeshletPath path;
    MeshletPathDesc d{};
    expect(!path.init(d), "init without a device fails");
    expect(!path.valid() && !path.usesMeshShaders(), "failed init leaves nothing");
    MeshletFrameDesc f{};
    culling::InstanceCuller culler;
    expect(!path.beginFrame(1, f, culler), "beginFrame before init fails");
    std::printf("api: ok\n");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "kernel") {
        testKernel();
    }
    if (all || suite == "parity") {
        testParity();
    }
    if (all || suite == "select") {
        testSelect();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
