// WP-4.1 CPU gates (stub-safe): record layouts, the jitter convention, the CPU reference motion kernel against
// an f64 analytic reprojection, the sky reprojection, and the TAAU host packing (TaauGpu's Params == the CPU
// driver's, bit for bit). Lavapipe gates: test_rp_temporal.cpp.
//
//   fuse_rp_temporal_cpu layout | jitter | motion | sky | taau_params
#include "test_rp_temporal_common.hpp"

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/taa/taau.hpp>
#include <fuse/renderer/temporal/motion_kernel.hpp>
#include <fuse/renderer/temporal/taau_gpu.hpp>
#include <fuse/renderer/temporal/temporal_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::temporal;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
namespace kernel = fuse::kernel;
using gpu_scene::GpuInstance;
using gpu_scene::GpuMesh;
using gpu_scene::GpuTransform;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr f64 kTolMotionPx = 1e-3; // acceptance: analytic reprojection error < 1e-3 px

// --- layout ---------------------------------------------------------------------------------------------
void runLayout() {
    // The static_asserts in temporal_types.hpp pin sizes / offsets; here: defaults and the packing.
    taau_kernel::Params p{};
    TaauFrameConstants c{};
    pack_taau_constants(p, c);
    const taau_kernel::Settings s{};
    expect(c.maxAccumulation == s.max_accumulation && c.accumulationMotionFalloff == s.accumulation_motion_falloff &&
               c.clampGamma == s.clamp_gamma && c.depthRejection == s.depth_rejection &&
               c.velocityRejectionPx == s.velocity_rejection_px && c.clipFullMotionPx == s.clip_full_motion_px &&
               c.staticClipStrength == s.static_clip_strength && c.spatialWeight == s.spatial_weight &&
               c.sampleKernelScale == s.sample_kernel_scale && c.reactiveStrength == s.reactive_strength &&
               c.transparencyClip == s.transparency_clip && c.historyFilter == s.history_filter &&
               c.dilateMotion == s.dilate_motion && c.dilateDepthThreshold == s.dilate_depth_threshold,
           "packed settings == taau_kernel::Settings");
    expect(std::memcmp(c.curToPrevView, p.cur_to_prev_view, sizeof(c.curToPrevView)) == 0 && c.hasPrev == 0u,
           "packed identity camera, no prev spans");
    f32 vp[16];
    for (u32 i = 0; i < 16u; ++i) {
        vp[i] = static_cast<f32>(i) * 0.25f - 1.f;
    }
    f32 out[16];
    jitter_view_proj(vp, 0.f, 0.f, 64u, 32u, out);
    expect(std::memcmp(vp, out, sizeof(vp)) == 0, "zero jitter leaves the matrix unchanged");
    std::printf("layout: MotionFrameConstants %zu B, TaauFrameConstants %zu B, TemporalPush %zu B\n",
                sizeof(MotionFrameConstants), sizeof(TaauFrameConstants), sizeof(TemporalPush));
}

// --- jitter ---------------------------------------------------------------------------------------------
void runJitter() {
    std::mt19937 rng(77);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    f64 worst = 0.0;
    u32 checked = 0;
    for (u32 trial = 0; trial < 64u; ++trial) {
        const u32 w = 64u + trial * 7u;
        const u32 h = 48u + trial * 3u;
        const f32 eye[3] = {u(rng) * 3.f, u(rng) * 2.f, 4.f + u(rng)};
        const f32 at[3] = {u(rng), u(rng), -5.f};
        const tm_test::Mat4 vp = tm_test::mul(tm_test::perspective(0.8f + 0.3f * u(rng), static_cast<f32>(w) / h, 0.1f, 200.f),
                                              tm_test::lookAt(eye, at));
        const fuse::math::Vec2 j = upscaleJitterOffset(trial, 16u);
        f32 draw[16];
        jitter_view_proj(vp.m, j.x, j.y, w, h, draw);
        for (u32 k = 0; k < 64u; ++k) {
            const f64 p[3] = {u(rng) * 4.0, u(rng) * 3.0, -2.0 - 10.0 * (u(rng) * 0.5 + 0.5)};
            const tm_test::D4 c = tm_test::clipOf(vp.m, p);
            const tm_test::D4 cd = tm_test::clipOf(draw, p);
            if (!(c.w > 0.0)) {
                continue;
            }
            // Unjittered pixel position s; drawn position must be s - jitter (the pixel centre i + 0.5 then sees the
            // point whose unjittered position is i + 0.5 + jitter: the TAAU kernel's sample convention).
            const f64 s[2] = {(c.x / c.w * 0.5 + 0.5) * w, (c.y / c.w * 0.5 + 0.5) * h};
            const f64 sd[2] = {(cd.x / cd.w * 0.5 + 0.5) * w, (cd.y / cd.w * 0.5 + 0.5) * h};
            worst = std::max({worst, std::fabs(sd[0] - (s[0] - j.x)), std::fabs(sd[1] - (s[1] - j.y))});
            ++checked;
        }
    }
    std::printf("jitter: %u points, |drawn - (unjittered - jitter)| max %.3g px\n", checked, worst);
    expect(checked > 3000u && worst < 1e-3, "jitter_view_proj moves points by -jitter pixels");
}

// --- motion ---------------------------------------------------------------------------------------------
/// A tiny quantised mesh (VPOS u16 x 4) of `tris` random triangles in [-1, 1]^3.
struct CpuMesh {
    std::vector<u16> vpos;
    std::vector<u32> indices;
    GpuMesh mesh{};
    visbuffer::decode_kernel::MeshPositions positions{};
};

CpuMesh makeMesh(std::mt19937& rng, u32 tris) {
    CpuMesh m;
    std::uniform_int_distribution<u32> q(0u, 65535u);
    const u32 verts = tris * 3u;
    m.vpos.resize(verts * 4u);
    for (u32 v = 0; v < verts; ++v) {
        for (u32 a = 0; a < 3u; ++a) {
            m.vpos[v * 4u + a] = static_cast<u16>(q(rng));
        }
        m.vpos[v * 4u + 3u] = 0;
        m.indices.push_back(v);
    }
    for (u32 a = 0; a < 3u; ++a) {
        m.mesh.quantOffset[a] = -1.f;
        m.mesh.quantStep[a] = 2.f / 65535.f;
    }
    m.mesh.vertexCount = verts;
    m.mesh.triangleCount = tris;
    m.mesh.indexCount = tris * 3u;
    m.mesh.firstIndex = 0;
    m.positions.vpos = m.vpos.data();
    m.positions.vertexCount = verts;
    return m;
}

void runMotion() {
    std::mt19937 rng(4242);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    constexpr u32 kW = 96u;
    constexpr u32 kH = 64u;
    CpuMesh mesh = makeMesh(rng, 24u);
    std::vector<GpuInstance> instances;
    std::vector<GpuTransform> cur, prev;
    for (u32 i = 0; i < 10u; ++i) {
        GpuInstance inst{};
        inst.mesh = 0;
        inst.flags = gpu_scene::kInstanceValid;
        instances.push_back(inst);
        const f32 mirror = i % 4u == 3u ? -1.f : 1.f;
        const GpuTransform t = tm_test::place(u(rng) * 3.f, u(rng) * 1.5f, -6.f - 3.f * u(rng), mirror * (0.8f + 0.3f * u(rng)),
                                              0.7f + 0.2f * u(rng), 1.f, u(rng) * 3.f, u(rng));
        cur.push_back(t);
        // Half of the instances move (translation + rotation + scale change), the others are static.
        prev.push_back(i % 2u == 0u ? t
                                    : tm_test::place(t.rows[0][3] - 0.07f, t.rows[1][3] + 0.03f, t.rows[2][3] + 0.05f,
                                                     mirror * 0.9f, 0.75f, 1.05f, u(rng) * 3.f, u(rng)));
    }
    const std::vector<visbuffer::decode_kernel::MeshPositions> positions = {mesh.positions};
    const std::vector<GpuMesh> meshes = {mesh.mesh};
    f64 worst[2] = {0.0, 0.0}; // static, moving
    u32 counted[2] = {0u, 0u};
    f64 worstDepth = 0.0;
    f64 naiveWorst = 0.0;
    u32 skyPixels = 0;
    for (u32 frame = 1; frame < 5u; ++frame) {
        const f32 t = static_cast<f32>(frame);
        const f32 eye[3] = {0.2f * t, 0.3f + 0.05f * t, 2.f - 0.1f * t};
        const f32 eyeP[3] = {0.2f * (t - 1.f), 0.3f + 0.05f * (t - 1.f), 2.f - 0.1f * (t - 1.f)};
        const f32 at[3] = {0.1f * t, 0.f, -8.f};
        const f32 atP[3] = {0.1f * (t - 1.f), 0.f, -8.f};
        const tm_test::Mat4 proj = tm_test::perspective(1.f, static_cast<f32>(kW) / kH, 0.2f, 100.f);
        const tm_test::Mat4 vp = tm_test::mul(proj, tm_test::lookAt(eye, at));
        const tm_test::Mat4 pvp = tm_test::mul(proj, tm_test::lookAt(eyeP, atP));
        const fuse::math::Vec2 j = upscaleJitterOffset(frame, 8u);
        const fuse::math::Vec2 jp = upscaleJitterOffset(frame - 1u, 8u);
        motion_kernel::Params p{};
        MotionFrameConstants& f = p.frame;
        jitter_view_proj(vp.m, j.x, j.y, kW, kH, f.drawViewProj);
        f32 prevDraw[16];
        jitter_view_proj(pvp.m, jp.x, jp.y, kW, kH, prevDraw);
        std::memcpy(f.viewProj, vp.m, sizeof(f.viewProj));
        std::memcpy(f.prevViewProj, pvp.m, sizeof(f.prevViewProj));
        f.flags = sky_reprojection(vp.m, pvp.m, f.skyReproj) ? kMotionSkyValid : 0u;
        f.width = kW;
        f.height = kH;
        f.jitterX = j.x;
        f.jitterY = j.y;
        // CPU visibility buffer: nearest triangle hit at every pixel centre under the draw matrix (f64).
        std::vector<u32> vis(kW * kH * 2u, visbuffer::kVisInvalid);
        std::vector<f64> best(kW * kH, 1e30);
        for (u32 inst = 0; inst < instances.size(); ++inst) {
            for (u32 tri = 0; tri < mesh.mesh.triangleCount; ++tri) {
                f32 v[3][3];
                for (u32 k = 0; k < 3u; ++k) {
                    visbuffer::decode_kernel::mesh_position(mesh.mesh, mesh.vpos.data(), mesh.indices[tri * 3u + k], v[k]);
                }
                tm_test::D4 c[3];
                for (u32 k = 0; k < 3u; ++k) {
                    const f64 o[3] = {v[k][0], v[k][1], v[k][2]};
                    f64 w[3];
                    tm_test::worldOf(cur[inst], o, w);
                    c[k] = tm_test::clipOf(f.drawViewProj, w);
                }
                for (u32 y = 0; y < kH; ++y) {
                    for (u32 x = 0; x < kW; ++x) {
                        // Inside test (f64 barycentrics of the pixel centre, all > 1e-4: no edge ties).
                        const f64 nx = (x + 0.5) * 2.0 / kW - 1.0;
                        const f64 ny = (y + 0.5) * 2.0 / kH - 1.0;
                        f64 e[3];
                        for (u32 k = 0; k < 3u; ++k) {
                            const tm_test::D4& a1 = c[(k + 1u) % 3u];
                            const tm_test::D4& a2 = c[(k + 2u) % 3u];
                            e[k] = (a1.x - nx * a1.w) * (a2.y - ny * a2.w) - (a1.y - ny * a1.w) * (a2.x - nx * a2.w);
                        }
                        const f64 s = e[0] + e[1] + e[2];
                        if (s == 0.0 || !(e[0] / s > 1e-4) || !(e[1] / s > 1e-4) || !(e[2] / s > 1e-4)) {
                            continue;
                        }
                        const tm_test::Analytic a =
                            tm_test::analyticMotion(f.drawViewProj, vp.m, pvp.m, prevDraw, cur[inst], prev[inst], v, x, y, kW, kH);
                        if (!a.ok || a.depth < 0.2) {
                            continue;
                        }
                        const u32 i = y * kW + x;
                        if (a.depth < best[i]) {
                            best[i] = a.depth;
                            vis[i * 2u] = inst;
                            vis[i * 2u + 1u] = tri;
                        }
                    }
                }
            }
        }
        std::vector<fuse::math::Vec2> motion(kW * kH);
        std::vector<f32> depth(kW * kH);
        std::vector<u32> status(kW * kH);
        p.vis = kernel::make_span(static_cast<const u32*>(vis.data()), static_cast<u32>(vis.size()));
        p.instances = kernel::make_span(static_cast<const GpuInstance*>(instances.data()), static_cast<u32>(instances.size()));
        p.transforms = kernel::make_span(static_cast<const GpuTransform*>(cur.data()), static_cast<u32>(cur.size()));
        p.prevTransforms = kernel::make_span(static_cast<const GpuTransform*>(prev.data()), static_cast<u32>(prev.size()));
        p.meshes = kernel::make_span(meshes.data(), 1u);
        p.indices = kernel::make_span(static_cast<const u32*>(mesh.indices.data()), static_cast<u32>(mesh.indices.size()));
        p.positions = kernel::make_span(positions.data(), 1u);
        p.motion = kernel::make_span(motion.data(), kW * kH);
        p.depth = kernel::make_span(depth.data(), kW * kH);
        p.status = kernel::make_span(status.data(), kW * kH);
        expect(kernel::launch(kernel::Backend::CpuReference, motion_kernel::make_launch(kW, kH), motion_kernel::Kernel{}, p).ok,
               "motion kernel launch");
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                const u32 i = y * kW + x;
                if (vis[i * 2u] == visbuffer::kVisInvalid) {
                    f64 sky[2];
                    if (tm_test::analyticSky(vp.m, pvp.m, j.x, j.y, x, y, kW, kH, sky)) {
                        worst[0] = std::max({worst[0], std::fabs(motion[i].x * kW - sky[0]), std::fabs(motion[i].y * kH - sky[1])});
                        ++skyPixels;
                    }
                    continue;
                }
                const u32 inst = vis[i * 2u];
                f32 v[3][3];
                for (u32 k = 0; k < 3u; ++k) {
                    visbuffer::decode_kernel::mesh_position(mesh.mesh, mesh.vpos.data(), mesh.indices[vis[i * 2u + 1u] * 3u + k], v[k]);
                }
                const tm_test::Analytic a =
                    tm_test::analyticMotion(f.drawViewProj, vp.m, pvp.m, prevDraw, cur[inst], prev[inst], v, x, y, kW, kH);
                expect(status[i] == motion_kernel::kStatusOk && a.ok, "covered pixel reconstructs");
                const u32 moving = inst % 2u;
                const f64 err = std::max(std::fabs(motion[i].x * kW - a.motionPx[0]), std::fabs(motion[i].y * kH - a.motionPx[1]));
                worst[moving] = std::max(worst[moving], err);
                ++counted[moving];
                worstDepth = std::max(worstDepth, std::fabs(depth[i] - a.depth) / a.depth);
                // Negative control: motion without explicit jitter handling (pixel centre minus the previous DRAW
                // projection) is off by the jitter difference.
                const f64 naive[2] = {(x + 0.5) - a.prevDrawPx[0], (y + 0.5) - a.prevDrawPx[1]};
                naiveWorst = std::max({naiveWorst, std::fabs(naive[0] - a.motionPx[0]), std::fabs(naive[1] - a.motionPx[1])});
            }
        }
    }
    std::printf("motion (CPU kernel vs f64 analytic, 4 frames, %ux%u, jittered): static + sky %u px max %.3g px, "
                "moving %u px max %.3g px, depth rel %.3g, sky %u px; no-jitter-handling control max %.3g px\n",
                kW, kH, counted[0], worst[0], counted[1], worst[1], worstDepth, skyPixels, naiveWorst);
    expect(counted[0] > 500u && counted[1] > 500u && skyPixels > 500u, "coverage of static, moving and sky pixels");
    expect(worst[0] < kTolMotionPx && worst[1] < kTolMotionPx, "analytic reprojection error < 1e-3 px");
    expect(worstDepth < 1e-5, "linear depth == clip w");
    expect(naiveWorst > 0.05, "negative control: ignoring the jitter fails the 1e-3 px gate");
}

// --- sky ------------------------------------------------------------------------------------------------
void runSky() {
    // Rotation-only: translation never moves the sky; pure rotation matches the homography; orthographic = no sky.
    const tm_test::Mat4 proj = tm_test::perspective(1.1f, 1.5f, 0.1f, 50.f);
    const f32 e0[3] = {0.f, 0.f, 0.f};
    const f32 e1[3] = {5.f, -2.f, 3.f};
    const f32 at0[3] = {0.f, 0.f, -1.f};
    const f32 at1[3] = {5.f, -2.f, 2.f};
    const tm_test::Mat4 a = tm_test::mul(proj, tm_test::lookAt(e0, at0));
    const tm_test::Mat4 b = tm_test::mul(proj, tm_test::lookAt(e1, at1)); // same orientation, translated
    f32 h[12];
    expect(sky_reprojection(a.m, b.m, h), "sky homography exists");
    MotionFrameConstants f{};
    std::memcpy(f.skyReproj, h, sizeof(h));
    f.flags = kMotionSkyValid;
    f.width = 120;
    f.height = 80;
    f.jitterX = 0.3f;
    f.jitterY = -0.2f;
    f64 worstTranslation = 0.0;
    for (u32 y = 0; y < 80u; y += 3u) {
        for (u32 x = 0; x < 120u; x += 3u) {
            const motion_kernel::Result r = motion_kernel::sky(f, x, y);
            worstTranslation = std::max({worstTranslation, std::fabs(r.motion[0] * 120.0), std::fabs(r.motion[1] * 80.0)});
        }
    }
    const f32 at2[3] = {0.4f, 0.3f, -1.f};
    const tm_test::Mat4 c = tm_test::mul(proj, tm_test::lookAt(e0, at2));
    expect(sky_reprojection(a.m, c.m, f.skyReproj), "rotation homography");
    f64 worstRotation = 0.0;
    u32 checked = 0;
    for (u32 y = 0; y < 80u; ++y) {
        for (u32 x = 0; x < 120u; ++x) {
            const motion_kernel::Result r = motion_kernel::sky(f, x, y);
            f64 ref[2];
            if (!tm_test::analyticSky(a.m, c.m, f.jitterX, f.jitterY, x, y, 120u, 80u, ref)) {
                continue;
            }
            worstRotation = std::max({worstRotation, std::fabs(r.motion[0] * 120.0 - ref[0]), std::fabs(r.motion[1] * 80.0 - ref[1])});
            ++checked;
        }
    }
    tm_test::Mat4 ortho{};
    ortho.m[0] = 0.1f;
    ortho.m[5] = -0.1f;
    ortho.m[10] = -0.01f;
    ortho.m[15] = 1.f;
    f32 dummy[12];
    const bool orthoSky = sky_reprojection(ortho.m, ortho.m, dummy);
    std::printf("sky: translation-only camera max %.3g px; rotation vs f64 %u px max %.3g px; orthographic sky %s\n",
                worstTranslation, checked, worstRotation, orthoSky ? "valid (unexpected)" : "disabled");
    expect(worstTranslation < 1e-4, "camera translation does not move the sky");
    expect(checked > 9000u && worstRotation < kTolMotionPx, "sky rotation reprojection < 1e-3 px");
    expect(!orthoSky, "orthographic projection: no sky reprojection");
}

// --- taau_params ----------------------------------------------------------------------------------------
/// TaauGpu builds the kernel's Params with taau_camera_terms + the CPU driver's scalar rules; running the CPU
/// kernel with those Params (and the spans the driver would hand it) must reproduce TaauUpscaler bit for bit.
void runTaauParams() {
    struct Config {
        UpscaleResolution res;
        bool camera;
        bool masks;
        u32 filter;
    };
    const Config configs[3] = {{makeUpscaleResolution(144u, 96u, 1.5f), true, true, 1u},
                               {makeUpscaleResolution(128u, 80u, 2.0f), false, false, 2u},
                               {makeUpscaleResolution(96u, 64u, 1.0f), true, true, 0u}};
    u32 frames = 0;
    u32 mismatches = 0;
    for (const Config& cfg : configs) {
        taau_kernel::Settings settings{};
        settings.history_filter = cfg.filter;
        TaauUpscaler cpu(settings);
        const u32 rn = cfg.res.render_width * cfg.res.render_height;
        const u32 dn = cfg.res.display_width * cfg.res.display_height;
        std::vector<fuse::math::Vec4> history[2] = {std::vector<fuse::math::Vec4>(dn), std::vector<fuse::math::Vec4>(dn)};
        std::vector<f32> prevDepth(rn);
        std::vector<fuse::math::Vec2> prevMotion(rn);
        u32 current = 0;
        bool valid = false;
        for (u32 frame = 0; frame < 10u; ++frame) {
            tm_test::TaauFrame tf = tm_test::makeTaauFrame(cfg.res, frame, cfg.camera, cfg.masks);
            tf.reset = frame == 6u;
            const UpscaleInputs in = tf.inputs();
            std::vector<fuse::math::Vec3> ref(dn), out(dn);
            expect(cpu.upscale(in, ref.data(), kernel::Backend::CpuReference), "cpu upscale");
            if (tf.reset) {
                valid = false;
            }
            taau_kernel::Params p{};
            p.render_w = cfg.res.render_width;
            p.render_h = cfg.res.render_height;
            p.display_w = cfg.res.display_width;
            p.display_h = cfg.res.display_height;
            p.jitter_px = in.jitter_px;
            p.exposure = in.exposure;
            p.history_valid = valid ? 1u : 0u;
            p.settings = settings;
            taau_camera_terms(in.camera, in.previous_camera, p);
            TaauFrameConstants c{};
            pack_taau_constants(p, c);
            expect(c.hasCamera == (cfg.camera ? 1u : 0u), "camera terms");
            p.color = kernel::make_span(in.color, rn);
            p.depth = kernel::make_span(in.depth, rn);
            p.motion = kernel::make_span(in.motion, rn);
            p.reactive = kernel::make_span(in.reactive, in.reactive != nullptr ? rn : 0u);
            p.transparency = kernel::make_span(in.transparency_composition, in.transparency_composition != nullptr ? rn : 0u);
            if (valid) {
                p.prev_depth = kernel::make_span(static_cast<const f32*>(prevDepth.data()), rn);
                p.prev_motion = kernel::make_span(static_cast<const fuse::math::Vec2*>(prevMotion.data()), rn);
            }
            p.history_in = kernel::make_span(static_cast<const fuse::math::Vec4*>(history[current].data()), dn);
            p.history_out = kernel::make_span(history[current ^ 1u].data(), dn);
            p.output = kernel::make_span(out.data(), dn);
            expect(taau_kernel::params_valid(p), "params valid");
            expect(kernel::launch(kernel::Backend::CpuReference, taau_kernel::make_launch(p.display_w, p.display_h),
                                  taau_kernel::Kernel{}, p)
                       .ok,
                   "kernel launch");
            std::copy(in.depth, in.depth + rn, prevDepth.begin());
            std::copy(in.motion, in.motion + rn, prevMotion.begin());
            current ^= 1u;
            valid = true;
            mismatches += std::memcmp(ref.data(), out.data(), dn * sizeof(fuse::math::Vec3)) != 0 ? 1u : 0u;
            mismatches += std::memcmp(cpu.history().data(), history[current].data(), dn * sizeof(fuse::math::Vec4)) != 0 ? 1u : 0u;
            ++frames;
        }
    }
    std::printf("taau_params: %u frames (1.5x / 2x / 1x, camera on / off, masks on / off, reset), %u mismatching "
                "outputs or histories\n",
                frames, mismatches);
    expect(mismatches == 0u, "TaauGpu host rules reproduce TaauUpscaler bit for bit");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "layout";
    if (suite == "layout") {
        runLayout();
    } else if (suite == "jitter") {
        runJitter();
    } else if (suite == "motion") {
        runMotion();
    } else if (suite == "sky") {
        runSky();
    } else if (suite == "taau_params") {
        runTaauParams();
    } else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
