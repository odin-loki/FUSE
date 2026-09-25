// WP-3.2 virtual shadow maps, page rendering / filtering / local lights: CPU gates (stub-safe; the
// Lavapipe gates are test_rp_vsm_raster.cpp).
//
//   layout  the shader records (vsm_shadow.glsl / .slang: VsmLocalLight, VsmShadowConstants, the probe
//           records) declare the C++ fields in the same order; the lighting constants carry shadowsLo / Hi
//   raster  raster_math::setup_tri / tri_texel on random triangles == a double-precision edge-function
//           oracle (every texel centre inside by > 1e-3 texel is covered, outside by > 1e-3 is not, depth
//           within 1e-5); VsmRasterReference::renderPages of a box caster == a double ray cast along the
//           light through every robust texel centre (depth within 2e-6 d units, ~1/15 texel)
//   local   spot page and cube faces of VsmRasterReference::renderLocal == a double ray cast from the
//           light through every robust texel centre; every direction's cube face projects inside it
//   filter  hard / PCF / PCSS on a roof edge (directional) and under a spot light: hard is a step at the
//           analytic edge, PCF visibility is k / (2r + 1)^2 and monotone across the edge, 0 / 1 away from
//           it; PCSS penumbrae widen with the blocker-receiver distance
//   seam    the B5 CSM seam gate (fuse_b5_shadows_gates: ground + a wall through every cascade, analytic
//           ray / box occlusion) ported to the VSM on the CPU: marking from a ray-cast depth image, the
//           page model, VsmRasterReference pages, the shadow lookup: 0 mismatches on both sides of every
//           clipmap level boundary (hard and PCF), and the coarser level agrees where its page is mapped
//   cache   a static second frame renders 0 pages and samples the cached pages identically; a moved
//           wall re-renders only the pages under its old / new footprint and the result is still exact
//   api     stub / no-device behaviour, makeLocalLight
#include "test_rp_vsm_raster_common.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/renderer/shadow/vsm/vsm_clipmap.hpp>
#include <fuse/renderer/shadow/vsm/vsm_kernel.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_raster_kernel.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_raster_reference.hpp>
#include <fuse/renderer/shadow/vsm_raster/vsm_shadows.hpp>
#include <fuse/renderer/visbuffer/vis_reference.hpp>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace {

using namespace vsmr_test;
using namespace fuse::renderer;
using namespace fuse::renderer::vsm;
using namespace fuse::renderer::vsm::raster_math;
namespace core_logic = fuse::core_logic;
using fuse::usize;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- CPU scene --------------------------------------------------------------------------------------
struct CpuScene {
    gpu_scene::GpuScene gpu;
    std::vector<geometry::MeshletMesh> meshes;
    std::vector<visbuffer::decode_kernel::MeshPositions> positions;
    World world;

    bool init(const World& w) {
        world = w;
        gpu_scene::GpuSceneDesc d{};
        if (!gpu.init(d) || !buildMeshes(meshes, world.groundSize)) {
            return false;
        }
        for (u32 i = 0; i < meshes.size(); ++i) {
            if (gpu.addMeshletMesh(meshes[i]) != i) {
                return false;
            }
            positions.push_back(visbuffer::decode_kernel::MeshPositions{meshes[i].positions.data(), meshes[i].vertex_count()});
        }
        return addInstances(gpu, world);
    }
    visbuffer::VisSceneView view() const { return visbuffer::vis_scene_view(gpu, positions); }
    void moveBox(u32 i, const Box& b) {
        world.boxes[i] = b;
        gpu_scene::SlotAllocator::Handle h{};
        h.slot = world.boxSlots[i];
        h.generation = gpu.instance(h.slot).generation;
        gpu.setTransform(h, World::boxTransform(b));
    }
};

/// A ray-cast depth image of the world (forward depth, 1 = background).
std::vector<f32> depthImage(const World& w, const Camera& cam) {
    std::vector<f32> depth(static_cast<usize>(cam.width) * cam.height, 1.f);
    for (u32 y = 0; y < cam.height; ++y) {
        for (u32 x = 0; x < cam.width; ++x) {
            D3 o, d;
            cam.ray(x, y, o, d);
            f64 t = 0.0;
            if (w.cast(o, d, 0.0, 1e9, t) != World::kMiss) {
                const f64 z = cam.depthOf(o + d * t);
                depth[static_cast<usize>(y) * cam.width + x] = z > 0.0 && z < 1.0 ? static_cast<f32>(z) : 1.f;
            }
        }
    }
    return depth;
}

// --- the whole VSM frame on the CPU -------------------------------------------------------------------
struct CpuVsm {
    VsmClipmap clipmap;
    std::unique_ptr<VsmPageModel> model{new VsmPageModel()};
    VsmInvalidationReference inval;
    VsmRasterReference raster;
    VsmFrameConstants c{};
    u32 poolX = 16, poolY = 16;
    std::vector<u32> pool, pte, request, invalid, scratch, list;
    core_logic::VsmPageStats stats{};
    VsmRasterRefStats rasterStats{};

    bool init(const VsmClipmapDesc& d, u32 px, u32 py) {
        poolX = px;
        poolY = py;
        pool.assign(static_cast<usize>(px) * py * kPageTexels * kPageTexels, kDepthClearBits);
        return clipmap.init(d) && model->reset(d.levels, px * py) == core_logic::ClStatus::Ok;
    }
    u32 poolWidth() const { return poolX * kPageTexels; }

    bool frame(CpuScene& s, const Camera& cam, const D3& light) {
        VsmViewDesc v{};
        v.lightDirection[0] = static_cast<f32>(light.x);
        v.lightDirection[1] = static_cast<f32>(light.y);
        v.lightDirection[2] = static_cast<f32>(light.z);
        v.cameraPosition[0] = static_cast<f32>(cam.eye.x);
        v.cameraPosition[1] = static_cast<f32>(cam.eye.y);
        v.cameraPosition[2] = static_cast<f32>(cam.eye.z);
        std::memcpy(v.invViewProj, cam.invViewProjF.m, sizeof(v.invViewProj));
        v.depthWidth = cam.width;
        v.depthHeight = cam.height;
        v.pixelSpread = cam.pixelSpread();
        if (!clipmap.build(v, c)) {
            return false;
        }
        c.physPages = poolX * poolY;
        c.poolPagesX = poolX;
        c.clearValue = kDepthClearBits;
        c.instanceCount = s.gpu.instanceHighWater();
        const std::vector<f32> depth = depthImage(s.world, cam);
        request.assign(c.requestWords, 0u);
        markReference(c, depth.data(), cam.width, cam.height, request.data(), fuse::kernel::Backend::CpuParallel, scratch);
        const visbuffer::VisSceneView view = s.view();
        invalid.assign(c.requestWords, 0u);
        inval.run(c, view.instances.data, view.transforms.data, view.instances.size, view.meshes.data, view.meshes.size,
                  invalid.data());
        if (model->update(frameInput(c), request.data(), invalid.data(), stats) != core_logic::ClStatus::Ok) {
            return false;
        }
        pte.resize(c.virtualPages);
        for (u32 i = 0; i < c.virtualPages; ++i) {
            pte[i] = model->pte(i);
        }
        list.clear();
        for (u32 i = 0; i < model->render_count(); ++i) {
            const u32 vp = model->render_page(i);
            list.push_back(vp);
            list.push_back(model->pte(vp) & kPtePhysMask);
        }
        raster.prepare(view, &c);
        rasterStats = raster.renderPages(c, list.data(), static_cast<u32>(list.size() / 2u), pool.data(), poolWidth());
        return true;
    }
    ShadowStore store() const {
        ShadowStore st{};
        st.pageTable = pte.data();
        st.pool = pool.data();
        st.poolWidth = poolWidth();
        return st;
    }
};

VsmShadowConstants shadowConstants(u32 filter, u32 radius, f32 sunTan = 0.05f) {
    VsmShadowConstants s{};
    s.vsm = 1u; // any non-zero address: the CPU lookup takes the frame constants directly
    s.directionalSlot = 0u;
    s.filterMode = filter;
    s.pcfRadius = radius;
    s.normalOffset = 1.5f;
    s.depthBias = 1.f;
    s.sunTanAngle = sunTan;
    s.pcssMaxRadius = kMaxFilterRadius;
    return s;
}

// --- layout -----------------------------------------------------------------------------------------
std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Member names of the first block that starts with `head` and ends with "};".
std::vector<std::string> members(const std::string& text, const std::string& head) {
    std::vector<std::string> out;
    const usize at = text.find(head);
    if (at == std::string::npos) {
        return out;
    }
    const usize end = text.find("};", at);
    std::istringstream lines(text.substr(at + head.size(), end - at - head.size()));
    std::string line;
    while (std::getline(lines, line)) {
        const usize semi = line.find(';');
        if (semi == std::string::npos) {
            continue;
        }
        std::string decl = line.substr(0, semi);
        const usize bracket = decl.find('[');
        if (bracket != std::string::npos) {
            decl = decl.substr(0, bracket);
        }
        const usize space = decl.find_last_of(" \t");
        out.push_back(decl.substr(space == std::string::npos ? 0 : space + 1));
    }
    return out;
}

int runLayout() {
    const std::vector<std::string> local = {"position", "slot",     "forward",  "range", "right", "nearPlane", "up",
                                            "invTanHalf", "type", "faces", "invRange", "lightSize", "page", "reserved"};
    const std::vector<std::string> shadow = {"vsm",          "work",      "directionalSlot", "filterMode", "pcfRadius",
                                             "localCount",   "normalOffset", "depthBias",    "sunTanAngle", "pcssMaxRadius",
                                             "localPool",    "localPagesX", "scene",         "instanceCount", "forceMask",
                                             "workHandle",   "localClear", "frame",          "reserved",   "local"};
    const std::vector<std::string> probe = {"position", "slot", "normal", "level"};
    const std::string dir = FUSE_VSMR_SHADER_DIR;
    const std::string glsl = readFile(dir + "/vsm_shadow.glsl");
    const std::string slang = readFile(dir + "/vsm_shadow.slang");
    expect(!glsl.empty() && !slang.empty(), "shader sources readable");
    expect(members(glsl, "struct FuseVsmLocal {") == local, "vsm_shadow.glsl FuseVsmLocal == VsmLocalLight");
    expect(members(slang, "struct VsmLocal {") == local, "vsm_shadow.slang VsmLocal == VsmLocalLight");
    expect(members(glsl, "buffer FuseVsmShadowRef {") == shadow, "vsm_shadow.glsl FuseVsmShadowRef == VsmShadowConstants");
    expect(members(slang, "struct VsmShadow {") == shadow, "vsm_shadow.slang VsmShadow == VsmShadowConstants");
    expect(members(readFile(dir + "/vsm_probe.comp"), "struct FuseVsmrProbeIn {") == probe, "vsm_probe.comp == VsmProbeInput");
    expect(members(readFile(dir + "/vsm_probe.slang"), "struct VsmrProbeIn {") == probe, "vsm_probe.slang == VsmProbeInput");
    const std::string lcGlsl = readFile(dir + "/../lighting/lc_common.glsl");
    const std::string lcSlang = readFile(dir + "/../lighting/lc_common.slang");
    expect(lcGlsl.find("uint shadowsLo;") != std::string::npos && lcGlsl.find("uint shadowsHi;") != std::string::npos &&
               lcSlang.find("uint shadowsLo;") != std::string::npos && lcSlang.find("uint shadowsHi;") != std::string::npos,
           "the lighting frame constants carry the shadow address (shadowsLo / Hi)");
    const std::string ltcGlsl = readFile(dir + "/../lighting/lc_ltc.glsl");
    const std::string ltcSlang = readFile(dir + "/../lighting/lc_ltc.slang");
    expect(ltcGlsl.find("../shadow_vsm/vsm_shadow.glsl") != std::string::npos &&
               ltcSlang.find("../shadow_vsm/vsm_shadow.slang") != std::string::npos,
           "the shared light loop (lc_ltc) includes the shadow lookup");
    expect(kRasterWorkWords * 4u <= 4096u && kWordLocalList == kWordHeaderCount, "raster work buffer layout");
    std::printf("layout: VsmLocalLight %zu B, VsmShadowConstants %zu B, work %u words\n", sizeof(VsmLocalLight),
                sizeof(VsmShadowConstants), kRasterWorkWords);
    return 0;
}

// --- raster -----------------------------------------------------------------------------------------
int runRasterTriangles() {
    Lcg rng{};
    u64 texels = 0, inside = 0, violations = 0, depthBad = 0;
    f64 maxDepthErr = 0.0;
    for (u32 n = 0; n < 3000; ++n) {
        Tri t{};
        const f64 span = n % 3u == 0u ? 400.0 : (n % 3u == 1u ? 60.0 : 8.0);
        const f64 cx = -10.0 + rng.next() * 148.0;
        const f64 cy = -10.0 + rng.next() * 148.0;
        for (u32 k = 0; k < 3u; ++k) {
            t.x[k] = static_cast<f32>(cx + (rng.next() - 0.5) * span);
            t.y[k] = static_cast<f32>(cy + (rng.next() - 0.5) * span);
            t.z[k] = static_cast<f32>(-0.2 + rng.next() * 1.4);
        }
        TriSetup s{};
        const bool ok = setup_tri(t, s);
        const f64 area = (static_cast<f64>(t.x[1]) - t.x[0]) * (static_cast<f64>(t.y[2]) - t.y[0]) -
                         (static_cast<f64>(t.y[1]) - t.y[0]) * (static_cast<f64>(t.x[2]) - t.x[0]);
        for (s32 py = 0; py < 128; ++py) {
            for (s32 px = 0; px < 128; ++px) {
                const f64 qx = px + 0.5;
                const f64 qy = py + 0.5;
                f64 margin = 1e300;
                f64 lambda[3];
                for (u32 k = 0; k < 3u; ++k) {
                    const u32 a = (k + 1u) % 3u;
                    const u32 b = (k + 2u) % 3u;
                    const f64 ex = static_cast<f64>(t.x[b]) - t.x[a];
                    const f64 ey = static_cast<f64>(t.y[b]) - t.y[a];
                    const f64 e = (ex * (qy - t.y[a]) - ey * (qx - t.x[a])) * (area > 0.0 ? 1.0 : -1.0);
                    const f64 len = std::sqrt(ex * ex + ey * ey);
                    margin = std::min(margin, len > 0.0 ? e / len : -1e300);
                    lambda[k] = e / std::fabs(area);
                }
                f32 z = 0.f;
                const bool covered = ok && px >= s.x0 && px <= s.x1 && py >= s.y0 && py <= s.y1 && tri_texel(t, s, px, py, z);
                ++texels;
                if (margin > 1e-3) {
                    ++inside;
                    if (!covered) {
                        ++violations;
                        continue;
                    }
                    const f64 exact = lambda[0] * t.z[0] + lambda[1] * t.z[1] + lambda[2] * t.z[2];
                    const f64 err = std::fabs(static_cast<f64>(clamp_depth(z)) - std::clamp(exact, 0.0, 1.0));
                    maxDepthErr = std::max(maxDepthErr, err);
                    // f32 barycentrics: the error grows with the triangle's extent in texels.
                    depthBad += err > 1e-5 * std::max(1.0, span / 40.0) ? 1u : 0u;
                } else if (margin < -1e-3 && covered) {
                    ++violations;
                }
            }
        }
    }
    std::printf("raster triangles: %llu texel tests, %llu robustly inside, %llu coverage violations, depth max err %.2e (%llu > 1e-5)\n",
                static_cast<unsigned long long>(texels), static_cast<unsigned long long>(inside),
                static_cast<unsigned long long>(violations), maxDepthErr, static_cast<unsigned long long>(depthBad));
    expect(inside > 100000u, "the oracle has robust samples");
    expect(violations == 0u, "coverage == the double edge-function oracle (1e-3 texel margin)");
    expect(depthBad == 0u, "depth == the exact plane within 1e-5 x max(1, extent / 40 texels)");
    return 0;
}

/// World point on the light ray through light-space (lx, ly) at light-space depth lz.
D3 lightToWorld(const VsmFrameConstants& c, f64 lx, f64 ly, f64 lz) {
    const f32* R = c.lightRotation;
    // lightRotation rows are orthonormal: world = R^T l.
    return {R[0] * lx + R[4] * ly + R[8] * lz, R[1] * lx + R[5] * ly + R[9] * lz, R[2] * lx + R[6] * ly + R[10] * lz};
}

int runRasterPages() {
    World w;
    w.groundSize = 40.f;
    w.boxes.push_back(Box{{-1.2, 0.3, -2.0}, {0.8, 1.7, -0.5}});
    CpuScene s;
    expect(s.init(w), "CPU scene");
    VsmClipmapDesc d{};
    d.levels = 8;
    d.firstLevelExtent = 4.f;
    Camera cam;
    cam.eye = {0.0, 3.0, 4.0};
    cam.at = {0.0, 0.0, -1.0};
    cam.width = 64;
    cam.height = 48;
    cam.build();
    CpuVsm v;
    expect(v.init(d, 8, 8), "CPU VSM");
    const D3 light = normalize(D3{0.4, -1.0, 0.3});
    VsmViewDesc view{};
    view.lightDirection[0] = static_cast<f32>(light.x);
    view.lightDirection[1] = static_cast<f32>(light.y);
    view.lightDirection[2] = static_cast<f32>(light.z);
    view.cameraPosition[2] = 0.f;
    std::memcpy(view.invViewProj, cam.invViewProjF.m, sizeof(view.invViewProj));
    view.depthWidth = cam.width;
    view.depthHeight = cam.height;
    expect(v.clipmap.build(view, v.c), "clipmap");
    v.c.physPages = 64u;
    v.c.poolPagesX = 8u;
    v.c.clearValue = kDepthClearBits;
    // Every page of levels 1..3 that the box's light-space footprint touches.
    std::vector<u32> list;
    u32 phys = 0;
    for (u32 level = 1; level <= 3u && phys < 64u; ++level) {
        const VsmLevelConstants& L = v.c.level[level];
        f64 lo[2] = {1e30, 1e30}, hi[2] = {-1e30, -1e30};
        for (u32 k = 0; k < 8u; ++k) {
            const Box& b = w.boxes[0];
            const f32 p[3] = {static_cast<f32>(k & 1u ? b.hi.x : b.lo.x), static_cast<f32>(k & 2u ? b.hi.y : b.lo.y),
                              static_cast<f32>(k & 4u ? b.hi.z : b.lo.z)};
            f32 l[3];
            light_point(v.c, p, l);
            for (u32 a = 0; a < 2u; ++a) {
                lo[a] = std::min(lo[a], static_cast<f64>(l[a]));
                hi[a] = std::max(hi[a], static_cast<f64>(l[a]));
            }
        }
        for (s32 py = static_cast<s32>(std::floor(lo[1] * L.invPageWorld)); py <= static_cast<s32>(std::floor(hi[1] * L.invPageWorld)); ++py) {
            for (s32 px = static_cast<s32>(std::floor(lo[0] * L.invPageWorld)); px <= static_cast<s32>(std::floor(hi[0] * L.invPageWorld)); ++px) {
                if (phys < 64u) {
                    list.push_back(level * kPagesPerLevel + slot_of(py) * kPagesPerAxis + slot_of(px));
                    list.push_back(phys++);
                }
            }
        }
    }
    v.raster.prepare(s.view(), &v.c);
    const VsmRasterRefStats st = v.raster.renderPages(v.c, list.data(), phys, v.pool.data(), v.poolWidth());
    u64 robust = 0, covered = 0, bad = 0;
    f64 maxErr = 0.0;
    for (u32 i = 0; i < phys; ++i) {
        u32 level = 0;
        s32 ax = 0, ay = 0;
        dir_page_of(v.c, list[i * 2u], level, ax, ay);
        const VsmLevelConstants& L = v.c.level[level];
        const f64 pw = L.pageWorld;
        const u32 ox = (i % 8u) * kPageTexels;
        const u32 oy = (i / 8u) * kPageTexels;
        for (u32 ty = 0; ty < kPageTexels; ++ty) {
            for (u32 tx = 0; tx < kPageTexels; ++tx) {
                // Robust: the ray through the texel centre and through four points 0.05 texel away agree.
                f64 depth[5];
                bool hitAll = true, missAll = true;
                const f64 offs[5][2] = {{0, 0}, {0.05, 0}, {-0.05, 0}, {0, 0.05}, {0, -0.05}};
                for (u32 k = 0; k < 5u; ++k) {
                    const f64 lx = (ax + (tx + 0.5 + offs[k][0]) / 128.0) * pw;
                    const f64 ly = (ay + (ty + 0.5 + offs[k][1]) / 128.0) * pw;
                    const f64 lz = L.depthCenter + 128.0 * pw; // d = 0 plane
                    const D3 o = lightToWorld(v.c, lx, ly, lz);
                    const f64 t = rayBox(o, light, w.boxes[0], 0.0, 1e9);
                    // The ground is not a caster in this world's box-only check: include it.
                    f64 tg = light.y < 0.0 ? -o.y / light.y : -1.0;
                    const D3 pg = o + light * tg;
                    if (std::fabs(pg.x) > w.groundSize * 0.5 || std::fabs(pg.z) > w.groundSize * 0.5) {
                        tg = -1.0;
                    }
                    f64 best = -1.0;
                    if (t >= 0.0) {
                        best = t;
                    }
                    if (tg >= 0.0 && (best < 0.0 || tg < best)) {
                        best = tg;
                    }
                    if (best < 0.0) {
                        hitAll = false;
                        depth[k] = 1.0;
                    } else {
                        missAll = false;
                        // d = distance from the d = 0 plane / (256 pageWorld)
                        depth[k] = std::clamp(best / (256.0 * pw), 0.0, 1.0);
                    }
                }
                const bool sameSurface = std::fabs(depth[1] + depth[2] - 2.0 * depth[0]) < 1e-6 &&
                                         std::fabs(depth[3] + depth[4] - 2.0 * depth[0]) < 1e-6;
                if (!(missAll || (hitAll && sameSurface))) {
                    continue;
                }
                ++robust;
                const f32 got = float_of(v.pool[static_cast<usize>(oy + ty) * v.poolWidth() + ox + tx]);
                covered += got < 1.f ? 1u : 0u;
                const f64 err = std::fabs(static_cast<f64>(got) - depth[0]);
                maxErr = std::max(maxErr, err);
                bad += err > 2e-6 ? 1u : 0u;
            }
        }
    }
    std::printf("raster pages: %u pages (levels 1-3), %llu triangles, %llu robust texels (%llu covered), max depth err %.2e, "
                "%llu > 2e-6\n",
                phys, static_cast<unsigned long long>(st.triangles), static_cast<unsigned long long>(robust),
                static_cast<unsigned long long>(covered), maxErr, static_cast<unsigned long long>(bad));
    expect(phys >= 3u && robust > 20000u && covered > 5000u, "the box covers texels on every level");
    expect(bad == 0u, "page depth == the double ray cast along the light (2e-6 d units)");
    return 0;
}

int runRaster() {
    runRasterTriangles();
    runRasterPages();
    return 0;
}

// --- local ------------------------------------------------------------------------------------------
int runLocal() {
    World w;
    w.groundSize = 40.f;
    w.boxes.push_back(Box{{-0.6, 0.0, -0.6}, {0.6, 1.0, 0.6}});
    w.boxes.push_back(Box{{2.0, 0.0, -0.4}, {2.6, 2.2, 0.4}});
    CpuScene s;
    expect(s.init(w), "CPU scene");
    VsmShadowConstants sc = shadowConstants(kFilterHard, 0);
    sc.localPagesX = 4;
    VsmLocalLightDesc descs[2]{};
    descs[0].type = kLocalSpot;
    descs[0].position[0] = 0.3f;
    descs[0].position[1] = 4.f;
    descs[0].position[2] = 0.2f;
    descs[0].direction[0] = 0.1f;
    descs[0].direction[1] = -1.f;
    descs[0].direction[2] = 0.f;
    descs[0].range = 9.f;
    descs[0].cosOuter = 0.8f;
    descs[1].type = kLocalPoint;
    descs[1].position[0] = 1.2f;
    descs[1].position[1] = 1.5f;
    descs[1].position[2] = 0.9f;
    descs[1].range = 7.f;
    // The VsmShadows host packing (buildLocal) is exercised through makeLocalLight in api; here the
    // records are packed like it does.
    auto pack = [](const VsmLocalLightDesc& d, VsmLocalLight& e) {
        e.position[0] = d.position[0];
        e.position[1] = d.position[1];
        e.position[2] = d.position[2];
        e.range = d.range;
        e.invRange = 1.f / d.range;
        e.nearPlane = 0.05f;
        e.type = d.type;
        e.faces = d.type == kLocalPoint ? 6u : 1u;
        if (d.type == kLocalPoint) {
            e.invTanHalf = 1.f;
            return;
        }
        const D3 f = normalize(D3{d.direction[0], d.direction[1], d.direction[2]});
        const D3 r = normalize(cross(f, D3{0, 1, 0}));
        const D3 u = cross(r, f);
        const f32 fv[3] = {static_cast<f32>(f.x), static_cast<f32>(f.y), static_cast<f32>(f.z)};
        const f32 rv[3] = {static_cast<f32>(r.x), static_cast<f32>(r.y), static_cast<f32>(r.z)};
        const f32 uv[3] = {static_cast<f32>(u.x), static_cast<f32>(u.y), static_cast<f32>(u.z)};
        std::memcpy(e.forward, fv, sizeof(fv));
        std::memcpy(e.right, rv, sizeof(rv));
        std::memcpy(e.up, uv, sizeof(uv));
        e.invTanHalf = d.cosOuter / std::sqrt(1.f - d.cosOuter * d.cosOuter);
    };
    pack(descs[0], sc.local[0]);
    pack(descs[1], sc.local[1]);
    sc.local[0].page[0] = 0;
    for (u32 f = 0; f < 6u; ++f) {
        sc.local[1].page[f] = 1u + f;
    }
    sc.localCount = 2;
    std::vector<u32> list;
    for (u32 k = 0; k < 2u; ++k) {
        for (u32 f = 0; f < sc.local[k].faces; ++f) {
            list.push_back(k | (f << 8u));
            list.push_back(sc.local[k].page[f]);
        }
    }
    std::vector<u32> atlas(static_cast<usize>(4 * 128) * (2 * 128), kDepthClearBits);
    VsmRasterReference ref;
    ref.prepare(s.view(), nullptr);
    const VsmRasterRefStats st = ref.renderLocal(sc, list.data(), static_cast<u32>(list.size() / 2u), atlas.data(), 512u);
    u64 robust = 0, covered = 0, bad = 0;
    f64 maxErr = 0.0;
    for (u32 i = 0; i < list.size() / 2u; ++i) {
        const VsmLocalLight& e = sc.local[list[i * 2u] & 0xFFu];
        const u32 face = list[i * 2u] >> 8u;
        const u32 page = list[i * 2u + 1u];
        f32 ff[3], rr[3], uu[3];
        local_basis(e, face, ff, rr, uu);
        const D3 F{ff[0], ff[1], ff[2]}, Rt{rr[0], rr[1], rr[2]}, U{uu[0], uu[1], uu[2]};
        const D3 P{e.position[0], e.position[1], e.position[2]};
        for (u32 ty = 0; ty < 128u; ++ty) {
            for (u32 tx = 0; tx < 128u; ++tx) {
                f64 depth[5];
                bool hitAll = true, missAll = true;
                u32 firstHit = World::kMiss;
                bool sameKind = true;
                const f64 offs[5][2] = {{0, 0}, {0.05, 0}, {-0.05, 0}, {0, 0.05}, {0, -0.05}};
                for (u32 k = 0; k < 5u; ++k) {
                    const f64 sx = ((tx + 0.5 + offs[k][0]) - 64.0) / 64.0 / e.invTanHalf;
                    const f64 sy = ((ty + 0.5 + offs[k][1]) - 64.0) / 64.0 / e.invTanHalf;
                    const D3 dir = normalize(F + Rt * sx + U * sy);
                    f64 t = 0.0;
                    u32 box = ~0u;
                    const u32 hit = w.cast(P, dir, 0.0, 1e9, t, &box);
                    const u32 kind = hit == World::kBox ? 10u + box : hit;
                    if (k == 0u) {
                        firstHit = kind;
                    }
                    sameKind = sameKind && kind == firstHit;
                    const f64 z = hit == World::kMiss ? 1e30 : t * dot(dir, F);
                    if (hit == World::kMiss || z >= e.range) {
                        hitAll = false;
                        depth[k] = 1.0;
                    } else {
                        missAll = false;
                        depth[k] = z / e.range;
                    }
                }
                if (!(missAll || (hitAll && sameKind))) {
                    continue;
                }
                if (hitAll && (std::fabs(depth[1] + depth[2] - 2.0 * depth[0]) > 1e-4 || std::fabs(depth[3] + depth[4] - 2.0 * depth[0]) > 1e-4)) {
                    continue; // a crease inside the footprint
                }
                ++robust;
                const u32 px = (page % 4u) * 128u + tx;
                const u32 py = (page / 4u) * 128u + ty;
                const f32 got = float_of(atlas[static_cast<usize>(py) * 512u + px]);
                covered += got < 1.f ? 1u : 0u;
                const f64 err = std::fabs(static_cast<f64>(got) - depth[0]);
                maxErr = std::max(maxErr, err / std::max(depth[0], 1e-3));
                bad += err > 1e-6 + 1e-6 * depth[0] ? 1u : 0u;
            }
        }
    }
    // Cube faces: every direction maps to a face whose projection lies in [0, 128)^2.
    Lcg rng{};
    u32 outside = 0;
    for (u32 n = 0; n < 20000; ++n) {
        const f32 rel[3] = {static_cast<f32>(rng.next() * 2.0 - 1.0), static_cast<f32>(rng.next() * 2.0 - 1.0),
                            static_cast<f32>(rng.next() * 2.0 - 1.0)};
        const u32 face = cube_face(rel);
        f32 ff[3], rr[3], uu[3];
        cube_basis(face, ff, rr, uu);
        const f32 vv[3] = {dot3(rel, rr), dot3(rel, uu), dot3(rel, ff)};
        f32 x, y, iz;
        local_project(sc.local[1], vv, x, y, iz);
        outside += (vv[2] > 0.f && x >= 0.f && x <= 128.f && y >= 0.f && y <= 128.f) ? 0u : 1u;
    }
    std::printf("local: %zu pages (spot + 6 cube faces), %llu triangles, %llu robust texels (%llu covered), max rel err %.2e, %llu bad; "
                "cube faces: %u of 20000 directions outside their face\n",
                list.size() / 2u, static_cast<unsigned long long>(st.triangles), static_cast<unsigned long long>(robust),
                static_cast<unsigned long long>(covered), maxErr, static_cast<unsigned long long>(bad), outside);
    expect(robust > 50000u && covered > 20000u, "local pages cover the casters");
    expect(bad == 0u, "local page depth == the double ray cast from the light (1e-6 + 1e-6 d)");
    expect(outside == 0u, "every direction projects inside its cube face");
    return 0;
}

// --- filter -----------------------------------------------------------------------------------------
struct FilterSample {
    f64 x = 0.0;
    f32 visibility = 1.f;
    f64 texel = 0.0; ///< world texel size of the sampled level
};

/// A roof (box) over the ground at `height`, its edge at x = 0; light almost straight down; receivers on
/// the ground across the edge (z = 0).
std::vector<FilterSample> roofVisibility(f64 height, u32 filter, u32 radius) {
    World w;
    w.groundSize = 60.f;
    w.boxes.push_back(Box{{-10.0, height, -6.0}, {0.0, height + 0.3, 6.0}});
    CpuScene s;
    expect(s.init(w), "CPU scene");
    Camera cam;
    cam.eye = {0.0, height + 6.0, 2.5};
    cam.at = {0.0, 0.0, 0.0};
    cam.width = 160;
    cam.height = 120;
    cam.build();
    CpuVsm v;
    VsmClipmapDesc d{};
    d.levels = 12;
    d.firstLevelExtent = 4.f;
    d.markRadiusTexels = 6.f;
    d.texelsPerPixel = 8.f;
    expect(v.init(d, 16, 16), "CPU VSM");
    const D3 light = normalize(D3{0.0001, -1.0, 0.00013});
    expect(v.frame(s, cam, light), "CPU VSM frame");
    expect(v.stats.failed == 0u, "every requested page mapped");
    const VsmShadowConstants sc = shadowConstants(filter, radius);
    const ShadowStore st = v.store();
    std::vector<FilterSample> out;
    for (f64 x = -0.5; x <= 0.5; x += 0.002) {
        const f32 p[3] = {static_cast<f32>(x), 0.f, 0.f};
        const f32 n[3] = {0.f, 1.f, 0.f};
        const SampleResult r = dir_visibility(v.c, sc, st, p, n, -1);
        FilterSample fs{};
        fs.x = x;
        fs.visibility = r.visibility;
        fs.texel = r.level >= 0 ? v.c.level[r.level].pageWorld / 128.0 : 1.0;
        out.push_back(fs);
    }
    return out;
}

int runFilter() {
    const std::vector<FilterSample> hard = roofVisibility(1.0, kFilterHard, 0);
    const std::vector<FilterSample> pcf = roofVisibility(1.0, kFilterPcf, 2);
    const std::vector<FilterSample> pcssLow = roofVisibility(0.3, kFilterPcss, 0);
    const std::vector<FilterSample> pcssHigh = roofVisibility(3.0, kFilterPcss, 0);
    u32 hardFrac = 0, hardBad = 0, pcfNonMono = 0, pcfBadValue = 0, pcfFrac = 0, lowFrac = 0, highFrac = 0, checked = 0;
    for (usize i = 0; i < hard.size(); ++i) {
        hardFrac += hard[i].visibility > 0.f && hard[i].visibility < 1.f ? 1u : 0u;
        // Hard: shadowed under the roof (x < 0), lit beyond, 3 texels (+ the PCF radius) from the edge.
        if (std::fabs(hard[i].x) > 3.0 * hard[i].texel) {
            ++checked;
            hardBad += (hard[i].visibility == 1.f) != (hard[i].x > 0.0) ? 1u : 0u;
        }
        if (std::fabs(pcf[i].x) > 5.0 * pcf[i].texel) {
            pcfBadValue += (pcf[i].visibility == 1.f) != (pcf[i].x > 0.0) || (pcf[i].visibility != 0.f && pcf[i].visibility != 1.f) ? 1u : 0u;
        }
        const f32 k = pcf[i].visibility * 25.f;
        pcfBadValue += std::fabs(k - std::round(k)) > 1e-4f ? 1u : 0u;
        pcfFrac += pcf[i].visibility > 0.f && pcf[i].visibility < 1.f ? 1u : 0u;
        if (i > 0u && pcf[i].visibility + 1e-6f < pcf[i - 1u].visibility) {
            ++pcfNonMono;
        }
        lowFrac += pcssLow[i].visibility > 0.f && pcssLow[i].visibility < 1.f ? 1u : 0u;
        highFrac += pcssHigh[i].visibility > 0.f && pcssHigh[i].visibility < 1.f ? 1u : 0u;
    }
    std::printf("filter (directional roof edge, %zu receivers, texel %.4f m): hard %u fractional, %u wrong of %u checked; PCF r=2 %u "
                "fractional, %u non-monotone, %u bad values; PCSS penumbra samples: blocker 0.3 m above %u, 3 m above %u\n",
                hard.size(), hard[0].texel, hardFrac, hardBad, checked, pcfFrac, pcfNonMono, pcfBadValue, lowFrac, highFrac);
    expect(checked > 300u && hardFrac == 0u && hardBad == 0u, "hard filter: a step at the roof edge");
    expect(pcfFrac > 0u && pcfNonMono == 0u && pcfBadValue == 0u, "PCF: k / 25 values, monotone across the edge, 0 / 1 away from it");
    expect(highFrac > lowFrac && lowFrac > 0u, "PCSS: the penumbra widens with the blocker distance");
    return 0;
}

// --- seam ---------------------------------------------------------------------------------------------
struct SeamReport {
    u32 checked = 0;
    u32 mismatches = 0;
    u32 boundary = 0;
    u32 boundaryMismatch = 0;
    u32 coarseChecked = 0;
    u32 coarseMismatch = 0;
    u32 unmapped = 0;
    u32 boundariesBothSides = 0;
};

/// Checks every ground pixel of the seam scene against the analytic answer (B5 port). `pixelLevel`
/// receives the sampled level per pixel (-1 = not checked).
SeamReport checkSeam(const World& w, const Camera& cam, const CpuVsm& v, const VsmShadowConstants& sc, u32 extraTexels) {
    SeamReport rep{};
    const D3 toLight = seamSunTravel() * -1.0;
    const ShadowStore st = v.store();
    std::vector<s32> level(static_cast<usize>(cam.width) * cam.height, -1);
    std::vector<signed char> answer(level.size(), -1);
    std::vector<unsigned char> wrong(level.size(), 0u);
    for (u32 y = 0; y < cam.height; ++y) {
        for (u32 x = 0; x < cam.width; ++x) {
            D3 o, d;
            cam.ray(x, y, o, d);
            f64 t = 0.0;
            if (w.cast(o, d, 0.0, 1e9, t) != World::kGround) {
                continue;
            }
            D3 p = o + d * t;
            p.y = 0.0;
            const f32 pf[3] = {static_cast<f32>(p.x), 0.f, static_cast<f32>(p.z)};
            const f32 nf[3] = {0.f, 1.f, 0.f};
            const SampleResult r = dir_visibility(v.c, sc, st, pf, nf, -1);
            if (r.level < 0) {
                ++rep.unmapped;
                continue;
            }
            const s32 coarser = std::min<s32>(r.level + 1, static_cast<s32>(v.c.levels) - 1);
            const f64 margin = (3.0 + extraTexels) * v.c.level[coarser].pageWorld / 128.0;
            if (!analyticClear(w, p, toLight, margin)) {
                continue;
            }
            const bool truth = w.occluded(p, toLight);
            const usize i = static_cast<usize>(y) * cam.width + x;
            level[i] = r.level;
            answer[i] = truth ? 1 : 0;
            ++rep.checked;
            const bool got = r.visibility < 0.5f;
            const bool exact = r.visibility == 0.f || r.visibility == 1.f;
            if (got != truth || !exact) {
                ++rep.mismatches;
                wrong[i] = 1u;
            }
            if (r.level + 1 < static_cast<s32>(v.c.levels)) {
                const SampleResult rc = dir_visibility(v.c, sc, st, pf, nf, r.level + 1);
                if (rc.visibility >= 0.f) {
                    ++rep.coarseChecked;
                    rep.coarseMismatch += (rc.visibility < 0.5f) != truth || !(rc.visibility == 0.f || rc.visibility == 1.f) ? 1u : 0u;
                }
            }
        }
    }
    // Boundary samples: pixels next to a pixel sampled on another level.
    u32 sides[16][2][2] = {}; // [boundary level][finer / coarser side][lit / shadowed]
    for (u32 y = 0; y < cam.height; ++y) {
        for (u32 x = 0; x < cam.width; ++x) {
            const usize i = static_cast<usize>(y) * cam.width + x;
            if (level[i] < 0) {
                continue;
            }
            bool isBoundary = false;
            s32 other = -1;
            const s32 dx[4] = {1, -1, 0, 0};
            const s32 dy[4] = {0, 0, 1, -1};
            for (u32 k = 0; k < 4u; ++k) {
                const s32 nx = static_cast<s32>(x) + dx[k];
                const s32 ny = static_cast<s32>(y) + dy[k];
                if (nx < 0 || ny < 0 || nx >= static_cast<s32>(cam.width) || ny >= static_cast<s32>(cam.height)) {
                    continue;
                }
                const s32 l = level[static_cast<usize>(ny) * cam.width + static_cast<usize>(nx)];
                if (l >= 0 && l != level[i]) {
                    isBoundary = true;
                    other = l;
                }
            }
            if (!isBoundary) {
                continue;
            }
            ++rep.boundary;
            rep.boundaryMismatch += wrong[i];
            const s32 b = std::min(level[i], other);
            if (b >= 0 && b < 16) {
                sides[b][level[i] == b ? 0 : 1][answer[i] == 1 ? 1 : 0]++;
            }
        }
    }
    for (u32 b = 0; b < 16u; ++b) {
        if (sides[b][0][0] > 0u && sides[b][0][1] > 0u && sides[b][1][0] > 0u && sides[b][1][1] > 0u) {
            ++rep.boundariesBothSides;
        }
    }
    return rep;
}

int runSeam() {
    const World w = seamWorld();
    CpuScene s;
    expect(s.init(w), "CPU scene");
    const Camera cam = seamCamera();
    VsmClipmapDesc d{};
    d.levels = 16;
    d.firstLevelExtent = 4.f;
    d.markRadiusTexels = 2.5f;
    d.texelsPerPixel = 2.f;
    CpuVsm v;
    expect(v.init(d, 16, 16), "CPU VSM");
    expect(v.frame(s, cam, seamSunTravel()), "CPU VSM frame");
    std::printf("seam: %u pages requested, %u rendered, %u failed; %llu triangles set up\n", v.stats.requested, v.stats.toRender,
                v.stats.failed, static_cast<unsigned long long>(v.rasterStats.triangles));
    expect(v.stats.failed == 0u && v.stats.toRender == v.stats.requested, "every requested page rendered");
    const struct {
        u32 filter;
        u32 radius;
        const char* name;
    } modes[2] = {{kFilterHard, 0u, "hard"}, {kFilterPcf, 1u, "pcf1"}};
    for (const auto& m : modes) {
        const SeamReport r = checkSeam(w, cam, v, shadowConstants(m.filter, m.radius), m.radius);
        std::printf("seam %s: %u receivers checked, %u mismatches; %u at level boundaries (%u mismatches), %u boundaries with "
                    "lit + shadowed samples on both sides; coarser level %u checked, %u mismatches; %u unmapped\n",
                    m.name, r.checked, r.mismatches, r.boundary, r.boundaryMismatch, r.boundariesBothSides, r.coarseChecked,
                    r.coarseMismatch, r.unmapped);
        expect(r.checked > 10000u && r.boundary > 200u, "the seam scene samples every level boundary");
        expect(r.mismatches == 0u && r.boundaryMismatch == 0u, "0 mismatches across clipmap boundaries (VSM == analytic)");
        expect(r.boundariesBothSides >= 2u, "the wall's shadow edge crosses the level boundaries (lit + shadowed on both sides)");
        expect(r.coarseChecked > 100u && r.coarseMismatch == 0u, "the coarser level agrees where its page is mapped");
        expect(r.unmapped == 0u, "every receiver finds a mapped page");
    }
    return 0;
}

// --- cache ----------------------------------------------------------------------------------------------
int runCache() {
    const World w = seamWorld();
    CpuScene s;
    expect(s.init(w), "CPU scene");
    const Camera cam = seamCamera();
    VsmClipmapDesc d{};
    d.levels = 16;
    d.firstLevelExtent = 4.f;
    d.markRadiusTexels = 2.5f;
    d.texelsPerPixel = 2.f;
    CpuVsm v;
    expect(v.init(d, 16, 16), "CPU VSM");
    expect(v.frame(s, cam, seamSunTravel()), "frame 0");
    const u32 first = v.stats.toRender;
    const std::vector<u32> pool0 = v.pool;
    expect(v.frame(s, cam, seamSunTravel()), "frame 1");
    std::printf("cache: frame 0 renders %u pages; static frame 1 renders %u (%u rasterised)\n", first, v.stats.toRender,
                v.rasterStats.pages);
    expect(first > 30u && v.stats.toRender == 0u && v.rasterStats.pages == 0u, "a static frame renders 0 pages");
    expect(v.pool == pool0, "cached pages keep their texels");
    const SeamReport r1 = checkSeam(s.world, cam, v, shadowConstants(kFilterHard, 0u), 0u);
    expect(r1.mismatches == 0u && r1.checked > 10000u, "the cached frame samples exactly");
    // Move the wall 0.4 m: only the pages under its old / new footprint re-render.
    Box moved = s.world.boxes[0];
    moved.lo.x += 0.4;
    moved.hi.x += 0.4;
    s.moveBox(0, moved);
    expect(v.frame(s, cam, seamSunTravel()), "frame 2");
    u32 requestedInvalid = 0;
    for (u32 i = 0; i < v.c.virtualPages; ++i) {
        const bool req = ((v.request[i / 32u] >> (i % 32u)) & 1u) != 0u;
        const bool inv = ((v.invalid[i / 32u] >> (i % 32u)) & 1u) != 0u;
        requestedInvalid += req && inv ? 1u : 0u;
    }
    const SeamReport r2 = checkSeam(s.world, cam, v, shadowConstants(kFilterHard, 0u), 0u);
    std::printf("cache: moved wall: %u of %u requested pages re-render (%u requested under the footprints); seam check %u receivers, "
                "%u mismatches\n",
                v.stats.toRender, v.stats.requested, requestedInvalid, r2.checked, r2.mismatches);
    expect(v.stats.toRender > 0u && v.stats.toRender < v.stats.requested, "only the moved caster's pages re-render");
    expect(r2.mismatches == 0u && r2.checked > 10000u, "partially re-rendered pages sample exactly (cache + re-render)");
    return 0;
}

// --- api ------------------------------------------------------------------------------------------------
int runApi() {
    VsmShadows shadows;
    VsmShadowsDesc d{};
    expect(!shadows.init(d), "VsmShadows::init fails without a device");
    expect(!shadows.valid() && shadows.shadowConstantsAddress() == 0u, "an uninitialised VsmShadows is inert");
    VsmShadowFrameDesc f{};
    expect(!shadows.beginFrame(1, f), "beginFrame fails when not initialised");
    gpu_scene::GpuLight spot{};
    spot.type = static_cast<u32>(gpu_scene::GpuLightType::Spot);
    spot.range = 5.f;
    spot.cosOuter = 0.6f;
    spot.direction[1] = -1.f;
    VsmLocalLightDesc out{};
    expect(makeLocalLight(3, spot, out) && out.type == kLocalSpot && out.slot == 3u && out.cosOuter == 0.6f, "spot -> local shadow");
    gpu_scene::GpuLight point{};
    point.type = static_cast<u32>(gpu_scene::GpuLightType::Point);
    point.range = 2.f;
    expect(makeLocalLight(4, point, out) && out.type == kLocalPoint, "point -> local shadow");
    gpu_scene::GpuLight sun{};
    sun.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
    expect(!makeLocalLight(5, sun, out), "directional -> no local shadow");
    point.range = 0.f;
    expect(!makeLocalLight(6, point, out), "a point light without a range has no local shadow");
    // shadow_visibility without shadows / for other slots is exactly 1.
    VsmShadowConstants sc{};
    const f32 p[3] = {0.f, 0.f, 0.f};
    const f32 n[3] = {0.f, 1.f, 0.f};
    expect(shadow_visibility(sc, nullptr, ShadowStore{}, 7u, p, n).visibility == 1.f, "no shadow -> visibility 1");
    std::printf("api: ok\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        runLayout();
    }
    if (all || suite == "raster") {
        runRaster();
    }
    if (all || suite == "local") {
        runLocal();
    }
    if (all || suite == "filter") {
        runFilter();
    }
    if (all || suite == "seam") {
        runSeam();
    }
    if (all || suite == "cache") {
        runCache();
    }
    if (all || suite == "api") {
        runApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
