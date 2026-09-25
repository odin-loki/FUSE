// B5.12 gate rows — Temporal & Screen-Space (TAA, HBAO, SSR).
//
// CI has no GPU, so each row is proven on the CPU reference implementations of the same algorithms
// (`TaaCpuResolver`, `hbaoPixelVisibility`, `ssrTracePixel`) against independent references:
//   - TAA edges/sub-pixel: supersampled ground-truth coverage (16x16 per pixel) with the engine Halton jitter.
//   - TAA rejection: a fast object (> 10 m/s) crossing a static background; vacated pixels vs background.
//   - HBAO: brute-force cosine-weighted hemisphere SSAO on the same depth/normals, and ray casts against the
//     true analytic geometry.
//   - SSR: analytic mirror reflection traced against the true scene geometry.
// GPU timing rows (HBAO < 1 ms, SSR < 1.5 ms, TAA < 0.5 ms) are hardware-only and not measured here.

#include <fuse/renderer/ssfx/hbao.hpp>
#include <fuse/renderer/ssfx/ssfx_view.hpp>
#include <fuse/renderer/ssfx/ssr.hpp>
#include <fuse/renderer/taa/taa_cpu_resolve.hpp>
#include <fuse/renderer/taa/taa_jitter.hpp>
#include <fuse/renderer/taa/taa_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using fuse::f32;
using fuse::i32;
using fuse::u32;
using fuse::math::Vec2;
using fuse::math::Vec3;
using namespace fuse::renderer;

constexpr f32 kPi = 3.14159265358979323846f;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectLess(f32 value, f32 limit, const char* message) {
    if (!(value < limit)) {
        std::fprintf(stderr, "FAIL: %s (got %f, limit %f)\n", message, value, limit);
        ++g_failures;
    }
}

void expectGreater(f32 value, f32 limit, const char* message) {
    if (!(value > limit)) {
        std::fprintf(stderr, "FAIL: %s (got %f, must exceed %f)\n", message, value, limit);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------------------------------------
// TAA — anti-aliasing of geometry edges and sub-pixel detail
// ---------------------------------------------------------------------------------------------------------

constexpr u32 kAaWidth = 48;
constexpr u32 kAaHeight = 48;
constexpr f32 kLineHalfWidth = 0.175f; // 0.35 px wide line — thinner than a pixel.

/// Scene for the anti-aliasing row: a nearly horizontal (slope 0.2) hard edge in the upper half and a
/// sub-pixel line (slope 0.13) in the lower half. Returns scalar coverage at continuous pixel coordinates.
f32 aaScene(f32 x, f32 y) {
    if (y < 24.f) {
        return (y > 6.f + 0.2f * x) ? 1.f : 0.f;
    }
    const f32 lineY = 30.f + 0.13f * x;
    const f32 dist = std::fabs(y - lineY) / std::sqrt(1.f + 0.13f * 0.13f);
    return dist < kLineHalfWidth ? 1.f : 0.f;
}

void renderAaFrame(const Vec2& jitter, std::vector<Vec3>& out) {
    for (u32 y = 0; y < kAaHeight; ++y) {
        for (u32 x = 0; x < kAaWidth; ++x) {
            const f32 v = aaScene(static_cast<f32>(x) + jitter.x, static_cast<f32>(y) + jitter.y);
            out[y * kAaWidth + x] = Vec3{v, v, v};
        }
    }
}

std::vector<f32> supersampledTruth() {
    // Ground truth: 16x16 stratified supersampling of every pixel footprint.
    std::vector<f32> truth(kAaWidth * kAaHeight, 0.f);
    for (u32 y = 0; y < kAaHeight; ++y) {
        for (u32 x = 0; x < kAaWidth; ++x) {
            f32 sum = 0.f;
            for (u32 j = 0; j < 16; ++j) {
                for (u32 i = 0; i < 16; ++i) {
                    sum += aaScene(static_cast<f32>(x) + (static_cast<f32>(i) + 0.5f) / 16.f,
                                   static_cast<f32>(y) + (static_cast<f32>(j) + 0.5f) / 16.f);
                }
            }
            truth[y * kAaWidth + x] = sum / 256.f;
        }
    }
    return truth;
}

struct AaMetrics {
    f32 maxErr = 0.f;         // worst per-pixel |TAA - truth| over the measured frames
    f32 edgeErr = 0.f;        // mean |TAA - truth| over edge pixels (0 < truth < 1)
    f32 cycleMaxErr = 0.f;    // worst |mean over measured frames - truth|
    f32 haltonMaxErr = 0.f;   // worst |mean over measured frames - Halton box estimate| (jitter-limited target)
    f32 haltonTruthErr = 0.f; // worst |Halton box estimate - truth| (sampling limit of the sequence)
    f32 maxDelta = 0.f;       // worst frame-to-frame change (shimmer) after convergence
    f32 meanDelta = 0.f;
    f32 rawMaxDelta = 0.f;    // same for the jittered frames without TAA
    u32 gapColumns = 0;       // sub-pixel line columns where the line vanished
    f32 lineColumnErr = 0.f;  // worst |column sum - truth column sum| on the sub-pixel line (px of coverage)
};

AaMetrics runTaaStatic(const std::vector<f32>& truth, u32 sequenceLength, const TaaCpuResolveOptions& options) {
    const u32 count = kAaWidth * kAaHeight;
    TaaJitterDesc jitterDesc{};
    jitterDesc.sequence_length = sequenceLength;
    TaaJitter jitter(jitterDesc);
    TaaCpuResolver resolver(options);
    const TAAParams params{};

    // Box estimate the jitter sequence can converge to: mean of one full cycle of jittered samples.
    std::vector<f32> haltonBox(count, 0.f);
    std::vector<Vec3> current(count);
    for (u32 k = 0; k < sequenceLength; ++k) {
        renderAaFrame(TaaJitterLayout::haltonPixelOffset(k, sequenceLength), current);
        for (u32 i = 0; i < count; ++i) {
            haltonBox[i] += current[i].x / static_cast<f32>(sequenceLength);
        }
    }

    std::vector<Vec3> output(count);
    std::vector<Vec3> previous(count);
    std::vector<Vec3> prevRaw(count);
    std::vector<f32> cycleMean(count, 0.f);
    const u32 warmup = 128;
    const u32 measured = 4u * sequenceLength;
    u32 edgePixels = 0;
    for (f32 t : truth) {
        edgePixels += (t > 0.f && t < 1.f) ? 1u : 0u;
    }
    AaMetrics m{};
    for (u32 frame = 0; frame < warmup + measured; ++frame) {
        renderAaFrame(jitter.currentPixelOffset(), current);
        TaaCpuFrameInputs in{};
        in.current = current.data();
        in.width = kAaWidth;
        in.height = kAaHeight;
        expectTrue(resolver.resolve(in, params, output.data()), "TAA CPU resolve succeeds");
        if (frame >= warmup) {
            f32 edgeErr = 0.f;
            f32 deltaSum = 0.f;
            for (u32 i = 0; i < count; ++i) {
                const f32 err = std::fabs(output[i].x - truth[i]);
                m.maxErr = std::max(m.maxErr, err);
                if (truth[i] > 0.f && truth[i] < 1.f) {
                    edgeErr += err;
                }
                const f32 delta = std::fabs(output[i].x - previous[i].x);
                m.maxDelta = std::max(m.maxDelta, delta);
                deltaSum += delta;
                m.rawMaxDelta = std::max(m.rawMaxDelta, std::fabs(current[i].x - prevRaw[i].x));
                cycleMean[i] += output[i].x / static_cast<f32>(measured);
            }
            m.edgeErr += edgeErr / static_cast<f32>(std::max(1u, edgePixels)) / static_cast<f32>(measured);
            m.meanDelta += deltaSum / static_cast<f32>(count) / static_cast<f32>(measured);
        }
        previous = output;
        prevRaw = current;
        jitter.advance();
    }
    for (u32 i = 0; i < count; ++i) {
        m.cycleMaxErr = std::max(m.cycleMaxErr, std::fabs(cycleMean[i] - truth[i]));
        m.haltonMaxErr = std::max(m.haltonMaxErr, std::fabs(cycleMean[i] - haltonBox[i]));
        m.haltonTruthErr = std::max(m.haltonTruthErr, std::fabs(haltonBox[i] - truth[i]));
    }
    for (u32 x = 0; x < kAaWidth; ++x) {
        f32 truthSum = 0.f;
        f32 taaSum = 0.f;
        f32 taaPeak = 0.f;
        for (u32 y = 26; y < kAaHeight; ++y) {
            const u32 i = y * kAaWidth + x;
            truthSum += truth[i];
            taaSum += output[i].x;
            taaPeak = std::max(taaPeak, output[i].x);
        }
        if (taaPeak < 0.1f) {
            ++m.gapColumns;
        }
        m.lineColumnErr = std::max(m.lineColumnErr, std::fabs(taaSum - truthSum));
    }
    return m;
}

void testTaaEliminatesEdgeAliasing() {
    const u32 count = kAaWidth * kAaHeight;
    const std::vector<f32> truth = supersampledTruth();

    // Baseline: no TAA, no jitter (pixel-centre sampling).
    std::vector<Vec3> aliased(count);
    renderAaFrame(Vec2{0.5f, 0.5f}, aliased);
    f32 aliasedMaxErr = 0.f;
    f32 aliasedEdgeErr = 0.f;
    u32 edgePixels = 0;
    for (u32 i = 0; i < count; ++i) {
        const f32 err = std::fabs(aliased[i].x - truth[i]);
        aliasedMaxErr = std::max(aliasedMaxErr, err);
        if (truth[i] > 0.f && truth[i] < 1.f) {
            aliasedEdgeErr += err;
            ++edgePixels;
        }
    }
    aliasedEdgeErr /= static_cast<f32>(std::max(1u, edgePixels));
    u32 aliasedGapColumns = 0;
    for (u32 x = 0; x < kAaWidth; ++x) {
        f32 sum = 0.f;
        for (u32 y = 26; y < kAaHeight; ++y) {
            sum += aliased[y * kAaWidth + x].x;
        }
        aliasedGapColumns += sum <= 0.f ? 1u : 0u;
    }

    const TaaCpuResolveOptions defaults{};
    const AaMetrics taa8 = runTaaStatic(truth, kTaaDefaultJitterSequenceLength, defaults);
    const AaMetrics taa16 = runTaaStatic(truth, 16u, defaults);
    TaaCpuResolveOptions alwaysClip{};
    alwaysClip.static_clip_strength = 1.f;
    const AaMetrics clip8 = runTaaStatic(truth, kTaaDefaultJitterSequenceLength, alwaysClip);

    std::printf("[B5 gate] TAA edges (%u edge px): no-TAA unjittered max err %.3f, mean edge err %.3f, sub-pixel line "
                "gaps %u/%u columns\n",
                edgePixels, aliasedMaxErr, aliasedEdgeErr, aliasedGapColumns, kAaWidth);
    const AaMetrics* rows[] = {&taa8, &taa16, &clip8};
    const char* names[] = {"Halton x8 (default)", "Halton x16", "Halton x8, clip always on"};
    for (u32 r = 0; r < 3; ++r) {
        const AaMetrics& m = *rows[r];
        std::printf("[B5 gate] TAA %-26s: max err %.3f, mean edge err %.3f, cycle-mean err %.3f (vs Halton box %.3f; "
                    "box vs truth %.3f) | shimmer max %.4f mean %.5f (no-TAA jittered %.2f) | line gaps %u, line "
                    "column err %.3f px\n",
                    names[r], m.maxErr, m.edgeErr, m.cycleMaxErr, m.haltonMaxErr, m.haltonTruthErr, m.maxDelta,
                    m.meanDelta, m.rawMaxDelta, m.gapColumns, m.lineColumnErr);
    }

    expectGreater(aliasedMaxErr, 0.4f, "unjittered no-TAA baseline is aliased (binary coverage)");
    expectGreater(static_cast<f32>(aliasedGapColumns), 10.f, "unjittered sub-pixel line breaks up without TAA");
    expectLess(taa8.edgeErr, 0.25f * aliasedEdgeErr, "TAA cuts edge coverage error to < 1/4 of aliased baseline");
    expectLess(taa8.edgeErr, 0.08f, "TAA mean edge coverage error vs supersampled truth");
    expectLess(taa8.haltonMaxErr, 0.03f, "TAA converges to the jitter-integrated (Halton box) coverage");
    expectLess(taa8.maxErr, 0.25f, "TAA max per-pixel coverage error vs supersampled truth (8-sample limit)");
    expectLess(taa8.maxDelta, 0.1f, "TAA converged static frame-to-frame delta (no shimmer)");
    expectLess(taa8.meanDelta, 0.005f, "TAA converged mean frame-to-frame delta");
    expectTrue(taa8.gapColumns == 0u, "TAA sub-pixel line visible in every column");
    expectLess(taa8.lineColumnErr, 0.2f, "TAA sub-pixel line column coverage matches truth");
    expectLess(taa16.edgeErr, taa8.edgeErr + 1e-4f, "longer Halton sequence does not increase edge error");
    expectLess(taa16.haltonMaxErr, 0.03f, "TAA (x16) converges to its Halton box coverage");
    // With the clip always on, jittered sub-pixel detail is clipped away in frames that miss it (flicker).
    expectGreater(clip8.maxDelta, taa8.maxDelta, "always-on clip flickers more than the motion-adaptive clip");
}

// ---------------------------------------------------------------------------------------------------------
// TAA — history rejection on fast-moving objects
// ---------------------------------------------------------------------------------------------------------

constexpr u32 kMoveWidth = 160;
constexpr u32 kMoveHeight = 64;
constexpr f32 kMoveFovY = 60.f * kPi / 180.f;
constexpr f32 kFrameRate = 60.f;
constexpr f32 kObjectDepth = 5.f;
constexpr f32 kBackgroundDepth = 30.f;
constexpr f32 kObjectSizePx = 12.f;

Vec3 moveBackground(f32 x, f32 y) {
    return Vec3{0.1f + 0.3f * x / static_cast<f32>(kMoveWidth), 0.35f + 0.1f * y / static_cast<f32>(kMoveHeight),
                0.6f};
}

const Vec3 kObjectColor{1.f, 0.2f, 0.1f};

bool insideObject(f32 x, f32 y, f32 left, f32 margin = 0.f) {
    const f32 top = 26.f;
    return x >= left - margin && x < left + kObjectSizePx + margin && y >= top - margin &&
           y < top + kObjectSizePx + margin;
}

struct GhostResult {
    f32 maxGhost = 0.f;       // worst |out - background| on background pixels >= 1.5 px from the object
    f32 maxGhostAfter1 = 0.f; // same, restricted to pixels vacated at least 1 frame ago
    f32 objectErr = 0.f;      // mean |out - truth| over object-interior pixels (object not smeared away)
    u32 depthRejections = 0;
    u32 velocityRejections = 0;
};

GhostResult runMovingObject(f32 speedMps, const TaaCpuResolveOptions& options, const TAAParams& params) {
    const SsfxCamera cam = SsfxCamera::fromVerticalFov(kMoveWidth, kMoveHeight, kMoveFovY);
    const f32 pxPerFrame = cam.fx * (speedMps / kFrameRate) / kObjectDepth;
    const u32 count = kMoveWidth * kMoveHeight;

    TaaJitter jitter;
    TaaCpuResolver resolver(options);
    std::vector<Vec3> current(count);
    std::vector<Vec2> velocity(count);
    std::vector<f32> depth(count);
    std::vector<Vec3> output(count);
    std::vector<i32> lastCovered(count, -1000);

    GhostResult result{};
    // Object parked for 40 frames (history fully converged on it), then moves right at `speedMps`.
    const u32 parked = 40;
    f32 left = 8.f;
    u32 objectSamples = 0;
    for (u32 frame = 0; frame < parked + 60; ++frame) {
        const bool moving = frame >= parked;
        if (moving) {
            left += pxPerFrame;
        }
        if (left + kObjectSizePx >= static_cast<f32>(kMoveWidth) - 2.f) {
            break;
        }
        const Vec2 j = jitter.currentPixelOffset();
        for (u32 y = 0; y < kMoveHeight; ++y) {
            for (u32 x = 0; x < kMoveWidth; ++x) {
                const u32 i = y * kMoveWidth + x;
                const f32 sx = static_cast<f32>(x) + j.x;
                const f32 sy = static_cast<f32>(y) + j.y;
                // Colour, depth and velocity are rasterised with the same jittered sample (as on the GPU).
                const bool sampleOnObject = insideObject(sx, sy, left);
                current[i] = sampleOnObject ? kObjectColor : moveBackground(sx, sy);
                velocity[i] = sampleOnObject && moving ? Vec2{pxPerFrame, 0.f} : Vec2{};
                depth[i] = sampleOnObject ? kObjectDepth : kBackgroundDepth;
                const f32 cxp = static_cast<f32>(x) + 0.5f;
                const f32 cyp = static_cast<f32>(y) + 0.5f;
                if (insideObject(cxp, cyp, left, 1.5f)) {
                    lastCovered[i] = static_cast<i32>(frame);
                }
            }
        }
        TaaCpuFrameInputs in{};
        in.current = current.data();
        in.velocity = velocity.data();
        in.depth = depth.data();
        in.width = kMoveWidth;
        in.height = kMoveHeight;
        resolver.resolve(in, params, output.data());
        result.depthRejections += resolver.lastStats().depth_rejections;
        result.velocityRejections += resolver.lastStats().velocity_rejections;

        if (!moving) {
            jitter.advance();
            continue;
        }
        for (u32 y = 0; y < kMoveHeight; ++y) {
            for (u32 x = 0; x < kMoveWidth; ++x) {
                const u32 i = y * kMoveWidth + x;
                const f32 cxp = static_cast<f32>(x) + 0.5f;
                const f32 cyp = static_cast<f32>(y) + 0.5f;
                if (insideObject(cxp, cyp, left, 1.5f)) {
                    if (insideObject(cxp, cyp, left, -1.5f)) {
                        const Vec3 d = output[i] - kObjectColor;
                        result.objectErr += d.length();
                        ++objectSamples;
                    }
                    continue;
                }
                const Vec3 bg = moveBackground(cxp, cyp);
                const f32 ghost = (output[i] - bg).length();
                result.maxGhost = std::max(result.maxGhost, ghost);
                if (static_cast<i32>(frame) - lastCovered[i] >= 1 && lastCovered[i] >= 0) {
                    result.maxGhostAfter1 = std::max(result.maxGhostAfter1, ghost);
                }
            }
        }
        jitter.advance();
    }
    result.objectErr /= static_cast<f32>(std::max(1u, objectSamples));
    return result;
}

void testTaaRejectsFastMovingHistory() {
    const SsfxCamera cam = SsfxCamera::fromVerticalFov(kMoveWidth, kMoveHeight, kMoveFovY);
    const f32 colourStep = (kObjectColor - moveBackground(40.f, 32.f)).length();

    TAAParams params{};
    TaaCpuResolveOptions full{};

    TaaCpuResolveOptions naive{};
    naive.neighborhood_clip = false;
    TAAParams naiveParams = params;
    naiveParams.velocity_rejection = 0.f;
    naiveParams.depth_rejection = 0.f;

    // Standard always-on YCoCg clip, no depth/velocity rejection.
    TaaCpuResolveOptions clipOnly{};
    clipOnly.static_clip_strength = 1.f;
    TAAParams clipOnlyParams = naiveParams;

    TaaCpuResolveOptions depthOnly{};
    depthOnly.neighborhood_clip = false;
    TAAParams depthOnlyParams = params;
    depthOnlyParams.velocity_rejection = 0.f;

    const f32 speeds[] = {12.f, 30.f};
    for (f32 speed : speeds) {
        const f32 pxPerFrame = cam.fx * (speed / kFrameRate) / kObjectDepth;
        const GhostResult taa = runMovingObject(speed, full, params);
        const GhostResult base = runMovingObject(speed, naive, naiveParams);
        const GhostResult clip = runMovingObject(speed, clipOnly, clipOnlyParams);
        const GhostResult depthRej = runMovingObject(speed, depthOnly, depthOnlyParams);
        std::printf("[B5 gate] TAA ghosting @ %.0f m/s (%.2f px/frame at %.0f m), colour step %.3f: full resolve max "
                    "ghost %.4f (object err %.4f, depth rej %u, velocity rej %u) | clip only %.4f | depth only %.4f | "
                    "no rejection %.3f\n",
                    speed, pxPerFrame, kObjectDepth, colourStep, taa.maxGhost, taa.objectErr, taa.depthRejections,
                    taa.velocityRejections, clip.maxGhost, depthRej.maxGhost, base.maxGhost);

        expectGreater(base.maxGhost, 0.3f * colourStep, "no-rejection baseline leaves a ghost trail");
        expectLess(taa.maxGhost, 0.03f * colourStep + 0.005f, "full TAA resolve: no ghost trail behind fast object");
        expectLess(clip.maxGhost, 0.05f * colourStep + 0.005f, "YCoCg clip alone removes the trail");
        expectLess(depthRej.maxGhost, base.maxGhost, "depth rejection alone reduces the trail");
        expectGreater(static_cast<f32>(taa.depthRejections), 0.f, "depth rejection fires on vacated pixels");
        expectGreater(static_cast<f32>(taa.velocityRejections), 0.f, "velocity rejection fires on vacated pixels");
        expectLess(taa.objectErr, 0.05f, "moving object interior keeps its colour (reprojected, not smeared)");
    }
}

// ---------------------------------------------------------------------------------------------------------
// Shared analytic scene helpers (world space, +Y up) for HBAO / SSR
// ---------------------------------------------------------------------------------------------------------

struct WorldCamera {
    Vec3 position{};
    Vec3 right{1.f, 0.f, 0.f};
    Vec3 down{0.f, -1.f, 0.f};
    Vec3 forward{0.f, 0.f, 1.f};

    static WorldCamera pitchedDown(const Vec3& position, f32 pitch) {
        WorldCamera c{};
        c.position = position;
        c.forward = Vec3{0.f, -std::sin(pitch), std::cos(pitch)};
        c.down = Vec3{0.f, -std::cos(pitch), -std::sin(pitch)};
        return c;
    }
    Vec3 viewToWorldDir(const Vec3& v) const { return right * v.x + down * v.y + forward * v.z; }
    Vec3 worldToViewDir(const Vec3& w) const { return Vec3{w.dot(right), w.dot(down), w.dot(forward)}; }
    Vec3 viewToWorldPoint(const Vec3& v) const { return position + viewToWorldDir(v); }
};

struct Hit {
    bool hit = false;
    f32 t = 0.f;
    Vec3 normal{};
    Vec3 color{};
    int surface = -1;
};

struct Box {
    Vec3 lo;
    Vec3 hi;
    Vec3 color;
};

void intersectBox(const Vec3& o, const Vec3& d, const Box& b, int id, Hit& best) {
    f32 tmin = -1e30f;
    f32 tmax = 1e30f;
    Vec3 nmin{};
    const f32 os[3] = {o.x, o.y, o.z};
    const f32 ds[3] = {d.x, d.y, d.z};
    const f32 los[3] = {b.lo.x, b.lo.y, b.lo.z};
    const f32 his[3] = {b.hi.x, b.hi.y, b.hi.z};
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(ds[a]) < 1e-9f) {
            if (os[a] < los[a] || os[a] > his[a]) {
                return;
            }
            continue;
        }
        f32 t0 = (los[a] - os[a]) / ds[a];
        f32 t1 = (his[a] - os[a]) / ds[a];
        f32 sign = -1.f;
        if (t0 > t1) {
            std::swap(t0, t1);
            sign = 1.f;
        }
        if (t0 > tmin) {
            tmin = t0;
            nmin = Vec3{a == 0 ? sign : 0.f, a == 1 ? sign : 0.f, a == 2 ? sign : 0.f};
        }
        tmax = std::min(tmax, t1);
        if (tmin > tmax) {
            return;
        }
    }
    if (tmin > 1e-4f && (!best.hit || tmin < best.t)) {
        best.hit = true;
        best.t = tmin;
        best.normal = nmin;
        best.color = b.color;
        best.surface = id;
    }
}

void intersectPlaneY(const Vec3& o, const Vec3& d, f32 planeY, f32 zMin, f32 zMax, int id, const Vec3& color,
                     Hit& best) {
    if (std::fabs(d.y) < 1e-9f) {
        return;
    }
    const f32 t = (planeY - o.y) / d.y;
    if (t <= 1e-4f || (best.hit && t >= best.t)) {
        return;
    }
    const Vec3 p = o + d * t;
    if (p.z < zMin || p.z > zMax) {
        return;
    }
    best.hit = true;
    best.t = t;
    best.normal = Vec3{0.f, d.y < 0.f ? 1.f : -1.f, 0.f};
    best.color = color;
    best.surface = id;
}

void intersectPlaneZ(const Vec3& o, const Vec3& d, f32 planeZ, f32 yMin, f32 yMax, int id, Hit& best,
                     Vec3 (*colorFn)(const Vec3&)) {
    if (std::fabs(d.z) < 1e-9f) {
        return;
    }
    const f32 t = (planeZ - o.z) / d.z;
    if (t <= 1e-4f || (best.hit && t >= best.t)) {
        return;
    }
    const Vec3 p = o + d * t;
    if (p.y < yMin || p.y > yMax) {
        return;
    }
    best.hit = true;
    best.t = t;
    best.normal = Vec3{0.f, 0.f, d.z > 0.f ? -1.f : 1.f};
    best.color = colorFn != nullptr ? colorFn(p) : Vec3{};
    best.surface = id;
}

struct GBuffer {
    SsfxCamera camera{};
    std::vector<f32> depth;
    std::vector<Vec3> normals;
    std::vector<Vec3> color;
    std::vector<int> surface;
    SsfxGBufferView view() const {
        SsfxGBufferView v{};
        v.camera = camera;
        v.depth = depth.data();
        v.normals = normals.data();
        return v;
    }
};

template <typename TraceFn>
GBuffer rasterize(const SsfxCamera& camera, const WorldCamera& world, TraceFn trace) {
    GBuffer g{};
    g.camera = camera;
    const u32 count = camera.width * camera.height;
    g.depth.assign(count, 0.f);
    g.normals.assign(count, Vec3{});
    g.color.assign(count, Vec3{});
    g.surface.assign(count, -1);
    for (u32 y = 0; y < camera.height; ++y) {
        for (u32 x = 0; x < camera.width; ++x) {
            // View ray with z == 1, so the hit parameter is the linear view depth.
            const Vec3 viewDir = camera.rayDirection(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f);
            const Hit h = trace(world.position, world.viewToWorldDir(viewDir));
            const u32 i = y * camera.width + x;
            if (h.hit) {
                g.depth[i] = h.t;
                g.normals[i] = world.worldToViewDir(h.normal);
                g.color[i] = h.color;
                g.surface[i] = h.surface;
            }
        }
    }
    return g;
}

// ---------------------------------------------------------------------------------------------------------
// HBAO — concave corner vs SSAO reference and true geometry
// ---------------------------------------------------------------------------------------------------------

constexpr f32 kStepZ = 3.f;      // riser plane (faces the camera, normal -Z)
constexpr f32 kStepHeight = 1.6f; // riser height; top face at y = kStepHeight for z >= kStepZ

enum StepSurface { kFloor = 0, kRiser = 1, kTop = 2 };

Hit traceStep(const Vec3& o, const Vec3& d) {
    Hit best{};
    intersectPlaneY(o, d, 0.f, -1e30f, kStepZ, kFloor, Vec3{0.5f, 0.5f, 0.5f}, best);
    intersectPlaneZ(o, d, kStepZ, 0.f, kStepHeight, kRiser, best, nullptr);
    intersectPlaneY(o, d, kStepHeight, kStepZ, 1e30f, kTop, Vec3{0.5f, 0.5f, 0.5f}, best);
    return best;
}

Hit traceFlat(const Vec3& o, const Vec3& d) {
    Hit best{};
    intersectPlaneY(o, d, 0.f, -1e30f, 1e30f, kFloor, Vec3{0.5f, 0.5f, 0.5f}, best);
    return best;
}

/// Cosine-weighted visibility against the true geometry within `radius` (stratified 48x48 rays).
template <typename TraceFn>
f32 trueGeometryVisibility(const Vec3& p, const Vec3& n, f32 radius, TraceFn trace) {
    const Vec3 helper = std::fabs(n.x) < 0.9f ? Vec3{1.f, 0.f, 0.f} : Vec3{0.f, 1.f, 0.f};
    const Vec3 t = fuse::math::cross(helper, n).normalized();
    const Vec3 b = fuse::math::cross(n, t);
    const u32 k = 48;
    u32 visible = 0;
    for (u32 i = 0; i < k; ++i) {
        for (u32 j = 0; j < k; ++j) {
            const f32 u1 = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(k);
            const f32 u2 = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(k);
            const f32 r = std::sqrt(u1);
            const f32 phi = 2.f * kPi * u2;
            const Vec3 dir = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(1.f - u1);
            const Hit h = trace(p + n * 1e-4f, dir);
            if (!h.hit || h.t > radius) {
                ++visible;
            }
        }
    }
    return static_cast<f32>(visible) / static_cast<f32>(k * k);
}

void testHbaoConcaveCorner() {
    const SsfxCamera camera = SsfxCamera::fromVerticalFov(160, 120, 60.f * kPi / 180.f);
    const WorldCamera world = WorldCamera::pitchedDown(Vec3{0.f, 2.2f, 0.f}, 30.f * kPi / 180.f);
    const f32 radius = 1.f;

    HbaoParams quality{};
    quality.radius = radius;
    quality.directions = 16;
    quality.steps_per_dir = 32;
    quality.max_radius_px = 128.f;
    quality.bias = 0.02f;

    HbaoParams defaults{};
    defaults.radius = radius;

    // Flat plane: no occlusion anywhere.
    {
        const GBuffer g = rasterize(camera, world, traceFlat);
        const SsfxGBufferView view = g.view();
        std::vector<f32> ao(camera.width * camera.height);
        expectTrue(computeHbaoCpu(view, quality, ao.data()), "HBAO full-frame CPU pass succeeds");
        f32 minVis = 1.f;
        for (f32 v : ao) {
            minVis = std::min(minVis, v);
        }
        SsfxGBufferView reconstructed = view;
        reconstructed.normals = nullptr;
        f32 minVisReconstructed = 1.f;
        for (u32 y = 70; y < camera.height; y += 7) {
            for (u32 x = 5; x < camera.width; x += 11) {
                minVisReconstructed = std::min(minVisReconstructed, hbaoPixelVisibility(reconstructed, quality, x, y));
            }
        }
        std::printf("[B5 gate] HBAO flat plane: min visibility %.4f (G-buffer normals), %.4f (depth-reconstructed)\n",
                    minVis, minVisReconstructed);
        expectGreater(minVis, 0.99f, "HBAO flat plane is unoccluded");
        expectGreater(minVisReconstructed, 0.99f, "HBAO flat plane unoccluded with reconstructed normals");
    }

    const GBuffer g = rasterize(camera, world, traceStep);
    const SsfxGBufferView view = g.view();

    // Walk down the centre column across the riser base (concave corner) and the top edge (convex edge).
    const u32 column = camera.width / 2;
    f32 worstVsRef = 0.f;
    f32 worstVsTrue = 0.f;
    f32 worstDefaultVsTrue = 0.f;
    f32 closestCornerVis = 1.f;
    f32 closestCornerDist = 1e9f;
    f32 closestCornerTrue = 1.f;
    f32 minConvex = 1.f;
    u32 concaveSamples = 0;
    f32 prevFloorVis = -1.f;
    bool monotonic = true;
    std::printf("[B5 gate] HBAO step scene (riser %.1f m, R = %.1f m), centre column:\n", kStepHeight, radius);
    std::printf("            surface  dist_to_corner   HBAO   HBAO(default)  SSAO-ref  true-geom\n");
    for (u32 y = 0; y < camera.height; ++y) {
        const u32 i = y * camera.width + column;
        if (g.surface[i] < 0) {
            continue;
        }
        const Vec3 pv = view.positionAt(column, y);
        const Vec3 pw = world.viewToWorldPoint(pv);
        const Vec3 nw = g.surface[i] == kRiser ? Vec3{0.f, 0.f, -1.f} : Vec3{0.f, 1.f, 0.f};
        f32 distCorner = 0.f;
        f32 distConvex = 0.f;
        if (g.surface[i] == kFloor) {
            distCorner = kStepZ - pw.z;
        } else if (g.surface[i] == kRiser) {
            distCorner = pw.y;
            distConvex = kStepHeight - pw.y;
        } else {
            distConvex = pw.z - kStepZ;
        }

        const bool nearConcave = g.surface[i] != kTop && distCorner < 1.2f * radius;
        const bool nearConvex = g.surface[i] != kFloor && distConvex < 0.6f * radius;
        if (!nearConcave && !nearConvex) {
            continue;
        }
        const f32 hbao = hbaoPixelVisibility(view, quality, column, y);
        const f32 hbaoDefault = hbaoPixelVisibility(view, defaults, column, y);
        const f32 ref = ssaoHemisphereReferenceVisibility(view, column, y, radius, 24u, 64u);
        const f32 truth = trueGeometryVisibility(pw, nw, radius, traceStep);
        const char* name = g.surface[i] == kFloor ? "floor" : (g.surface[i] == kRiser ? "riser" : "top");
        std::printf("            %-6s   %6.3f m          %.3f  %.3f          %.3f     %.3f\n", name,
                    nearConcave ? distCorner : distConvex, hbao, hbaoDefault, ref, truth);

        if (nearConcave && g.surface[i] == kFloor) {
            ++concaveSamples;
            worstVsRef = std::max(worstVsRef, std::fabs(hbao - ref));
            worstVsTrue = std::max(worstVsTrue, std::fabs(hbao - truth));
            worstDefaultVsTrue = std::max(worstDefaultVsTrue, std::fabs(hbaoDefault - truth));
            if (distCorner < closestCornerDist) {
                closestCornerDist = distCorner;
                closestCornerVis = hbao;
                closestCornerTrue = truth;
            }
            // Floor rows run from the corner towards the camera: visibility must rise with distance.
            if (prevFloorVis >= 0.f && hbao < prevFloorVis - 0.005f) {
                monotonic = false;
            }
            prevFloorVis = hbao;
        }
        if (nearConvex && g.surface[i] == kTop) {
            minConvex = std::min(minConvex, hbao);
        }
        if (nearConvex && g.surface[i] == kRiser && distCorner > radius) {
            minConvex = std::min(minConvex, hbao);
        }
    }
    std::printf("[B5 gate] HBAO concave floor samples %u: closest %.3f m from corner -> HBAO %.3f (true %.3f, "
                "analytic limit 0.5 at the corner line); max |HBAO - SSAO ref| %.3f, max |HBAO - true| %.3f, "
                "default-quality max |HBAO - true| %.3f; convex edge min visibility %.3f\n",
                concaveSamples, closestCornerDist, closestCornerVis, closestCornerTrue, worstVsRef, worstVsTrue,
                worstDefaultVsTrue, minConvex);

    expectGreater(static_cast<f32>(concaveSamples), 8.f, "HBAO corner walk has enough floor samples");
    expectLess(std::fabs(closestCornerVis - 0.5f), 0.08f, "HBAO ~0.5 visibility at the 90 degree corner line");
    expectLess(worstVsRef, 0.08f, "HBAO matches brute-force SSAO reference in the concave corner");
    expectLess(worstVsTrue, 0.08f, "HBAO matches true-geometry cosine AO in the concave corner");
    expectLess(worstDefaultVsTrue, 0.15f, "default-quality HBAO (8 dirs x 4 steps) tracks true AO");
    expectTrue(monotonic, "HBAO occlusion decays monotonically away from the corner");
    expectGreater(minConvex, 0.97f, "HBAO: no occlusion on the convex edge");
}

// ---------------------------------------------------------------------------------------------------------
// SSR — reflective floor vs analytic mirror ground truth
// ---------------------------------------------------------------------------------------------------------

constexpr f32 kWallZ = 12.f;

Vec3 wallColor(const Vec3& p) {
    return Vec3{0.5f + 0.35f * std::sin(0.6f * p.x), 0.5f + 0.35f * std::cos(0.5f * p.y),
                0.55f + 0.3f * std::sin(0.3f * p.x + 0.4f * p.y)};
}

const Box kPillarA{Vec3{-2.5f, 0.f, 6.f}, Vec3{-1.5f, 3.f, 7.f}, Vec3{0.9f, 0.2f, 0.15f}};
const Box kPillarB{Vec3{1.2f, 0.f, 4.5f}, Vec3{2.0f, 2.f, 5.3f}, Vec3{0.15f, 0.8f, 0.25f}};
enum SsrSurface { kSsrFloor = 0, kSsrWall = 1, kSsrPillarA = 2, kSsrPillarB = 3 };

Hit traceSsrScene(const Vec3& o, const Vec3& d, bool includeFloor) {
    Hit best{};
    if (includeFloor) {
        intersectPlaneY(o, d, 0.f, -1e30f, kWallZ, kSsrFloor, Vec3{0.3f, 0.3f, 0.3f}, best);
    }
    intersectPlaneZ(o, d, kWallZ, 0.f, 1e30f, kSsrWall, best, wallColor);
    intersectBox(o, d, kPillarA, kSsrPillarA, best);
    intersectBox(o, d, kPillarB, kSsrPillarB, best);
    return best;
}

void testSsrReflectiveFloor() {
    const SsfxCamera camera = SsfxCamera::fromVerticalFov(160, 120, 60.f * kPi / 180.f);
    const WorldCamera world = WorldCamera::pitchedDown(Vec3{0.f, 1.5f, 0.f}, 10.f * kPi / 180.f);
    const GBuffer g =
        rasterize(camera, world, [](const Vec3& o, const Vec3& d) { return traceSsrScene(o, d, true); });
    const SsfxGBufferView view = g.view();

    auto evaluate = [&](const SsrParams& params, const char* label, f32& meanErr, f32& within15, f32& coverage) {
        u32 valid = 0;
        u32 hits = 0;
        u32 good = 0;
        f32 errSum = 0.f;
        f32 worstErr = 0.f;
        std::vector<f32> errs;
        for (u32 y = 0; y < camera.height; ++y) {
            for (u32 x = 0; x < camera.width; ++x) {
                const u32 i = y * camera.width + x;
                if (g.surface[i] != kSsrFloor) {
                    continue;
                }
                // Analytic mirror reflection from the floor point.
                const Vec3 pw = world.viewToWorldPoint(view.positionAt(x, y));
                const Vec3 incident = (pw - world.position).normalized();
                const Vec3 reflected = Vec3{incident.x, -incident.y, incident.z};
                const Hit truth = traceSsrScene(pw, reflected, false);
                if (!truth.hit || truth.t > params.max_distance) {
                    continue;
                }
                // Screen-space methods can only reflect what the camera sees: require the true hit point to
                // be on screen, unoccluded, and away from silhouette pixels.
                const Vec3 hitWorld = pw + reflected * truth.t;
                const Vec3 hitView = world.worldToViewDir(hitWorld - world.position);
                f32 hx = 0.f;
                f32 hy = 0.f;
                if (!camera.project(hitView, hx, hy) || !camera.inside(hx, hy)) {
                    continue;
                }
                const u32 hi = static_cast<u32>(hy) * camera.width + static_cast<u32>(hx);
                if (g.surface[hi] != truth.surface || std::fabs(g.depth[hi] - hitView.z) > 0.01f * hitView.z) {
                    continue;
                }
                ++valid;
                const SsrHit ssr = ssrTracePixel(view, g.color.data(), params, x, y);
                if (!ssr.hit) {
                    continue;
                }
                ++hits;
                const f32 err = (ssr.color - truth.color).length() / std::max(1e-3f, truth.color.length());
                errSum += err;
                worstErr = std::max(worstErr, err);
                errs.push_back(err);
                if (err <= 0.15f) {
                    ++good;
                }
            }
        }
        meanErr = hits > 0 ? errSum / static_cast<f32>(hits) : 1.f;
        within15 = hits > 0 ? static_cast<f32>(good) / static_cast<f32>(hits) : 0.f;
        coverage = valid > 0 ? static_cast<f32>(hits) / static_cast<f32>(valid) : 0.f;
        std::sort(errs.begin(), errs.end());
        const f32 p95 = errs.empty() ? 1.f : errs[static_cast<size_t>(0.95f * static_cast<f32>(errs.size() - 1))];
        std::printf("[B5 gate] SSR %s: valid floor pixels %u, hits %u (coverage %.1f%%), mean rel colour err %.2f%%, "
                    "p95 %.2f%%, within 15%%: %.1f%%, worst %.1f%%\n",
                    label, valid, hits, 100.f * coverage, 100.f * meanErr, 100.f * p95, 100.f * within15,
                    100.f * worstErr);
    };

    SsrParams defaults{};
    f32 meanErr = 0.f;
    f32 within15 = 0.f;
    f32 coverage = 0.f;
    evaluate(defaults, "default (64 steps)", meanErr, within15, coverage);
    expectLess(meanErr, 0.15f, "SSR mean reflected colour error within 15% of analytic mirror");
    expectGreater(within15, 0.9f, "SSR: >= 90% of floor hits within 15% of analytic mirror");
    expectGreater(coverage, 0.9f, "SSR finds the reflection for >= 90% of screen-visible mirror hits");

    SsrParams fine{};
    fine.max_steps = 256;
    evaluate(fine, "fine (256 steps)", meanErr, within15, coverage);
    expectLess(meanErr, 0.15f, "SSR (256 steps) mean colour error within 15%");
    expectGreater(within15, 0.9f, "SSR (256 steps) >= 90% within 15%");

    // A pixel on the pillar-B reflection must pick up the pillar's green, not the wall behind it.
    u32 greenHits = 0;
    for (u32 y = 0; y < camera.height; ++y) {
        for (u32 x = 0; x < camera.width; ++x) {
            const u32 i = y * camera.width + x;
            if (g.surface[i] != kSsrFloor) {
                continue;
            }
            const SsrHit ssr = ssrTracePixel(view, g.color.data(), defaults, x, y);
            if (ssr.hit && ssr.color.y > 0.7f && ssr.color.x < 0.3f) {
                ++greenHits;
            }
        }
    }
    std::printf("[B5 gate] SSR floor pixels reflecting the green pillar: %u\n", greenHits);
    expectGreater(static_cast<f32>(greenHits), 20.f, "SSR reflects the foreground pillar colour");

    // Non-reflective / sky pixels and invalid inputs miss cleanly.
    SsfxGBufferView empty{};
    expectTrue(!ssrTracePixel(empty, g.color.data(), defaults, 0, 0).hit, "SSR rejects invalid view");
    std::vector<f32> mask(camera.width * camera.height, 0.f);
    std::vector<Vec3> out(camera.width * camera.height);
    std::vector<f32> conf(camera.width * camera.height);
    expectTrue(computeSsrCpu(view, g.color.data(), defaults, mask.data(), out.data(), conf.data()),
               "SSR full-frame pass succeeds");
    f32 maskedSum = 0.f;
    for (f32 c : conf) {
        maskedSum += c;
    }
    expectTrue(maskedSum == 0.f, "SSR skips pixels outside the reflective mask");
}

} // namespace

int main() {
    testTaaEliminatesEdgeAliasing();
    testTaaRejectsFastMovingHistory();
    testHbaoConcaveCorner();
    testSsrReflectiveFloor();
    std::printf("[B5 gate] hardware-only rows (not measured on CPU CI): HBAO < 1 ms, SSR < 1.5 ms, TAA < 0.5 ms "
                "(1080p GPU timings)\n");
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_b5_taa_ssfx_gates: all checks passed\n");
    return 0;
}
