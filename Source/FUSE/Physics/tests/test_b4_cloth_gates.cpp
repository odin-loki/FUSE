// B4.11 cloth gate rows (master plan):
//  - 32x32 cloth simulates under gravity without instability at dt = 1/60
//  - pinned corners hold position exactly
//  - wind force deflects the cloth in the correct direction
//  - cloth-sphere collision resolves without interpenetration
//  - 64x64 cloth simulation < 1 ms per frame (optimised builds)
#include <fuse/core/sanitizer.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/physics/softbody/cloth_simulator.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

using namespace fuse::physics;
using fuse::u32;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr f32 kDt = 1.f / 60.f;
const vec3 kGravity{0.f, -9.81f, 0.f};

bool allFinite(const ClothSimulator& cloth) {
    for (u32 i = 0; i < cloth.particleCount(); ++i) {
        const vec3 p = cloth.position(i);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            return false;
        }
    }
    return true;
}

vec3 centroid(const ClothSimulator& cloth) {
    vec3 sum{};
    for (u32 i = 0; i < cloth.particleCount(); ++i) {
        sum += cloth.position(i);
    }
    return sum * (1.f / static_cast<f32>(cloth.particleCount()));
}

void testHangingClothStableAndPinned() {
    ClothSimulator cloth;
    ClothDesc desc{};
    desc.rows = 32;
    desc.cols = 32;
    desc.pinnedCorners = 0b0011u; // the two corners of row 0
    cloth.init(desc, {0.f, 5.f, 0.f});
    const vec3 pinA = cloth.position(cloth.index(0, 0));
    const vec3 pinB = cloth.position(cloth.index(0, 31));

    bool pinsExact = true;
    f32 worstStretch = 0.f;
    f32 peakEarly = 0.f; // first 5 s
    f32 peakLate = 0.f;  // last 5 s
    for (int frame = 0; frame < 600; ++frame) { // 10 s
        cloth.step(kDt, kGravity);
        const vec3 a = cloth.position(cloth.index(0, 0));
        const vec3 b = cloth.position(cloth.index(0, 31));
        pinsExact = pinsExact && a.x == pinA.x && a.y == pinA.y && a.z == pinA.z && b.x == pinB.x && b.y == pinB.y &&
                    b.z == pinB.z;
        worstStretch = std::max(worstStretch, cloth.maxStretchRatio());
        f32& peak = frame < 300 ? peakEarly : peakLate;
        for (u32 i = 0; i < cloth.particleCount(); ++i) {
            peak = std::max(peak, cloth.velocity(i).length());
        }
    }
    std::printf("cloth 32x32: %u constraints, worst stretch %.3f, peak speed %.3f m/s (0-5 s) -> %.3f m/s (5-10 s), "
                "centroid y %.3f\n",
                cloth.constraintCount(), worstStretch, peakEarly, peakLate, centroid(cloth).y);
    expectTrue(allFinite(cloth), "no NaN/inf after 10 s");
    expectTrue(worstStretch < 1.15f, "edges never over-stretch (no instability)");
    // A 3.1 m sheet pinned at two corners swings like a pendulum (its free corner whips on the
    // first swing); unstable integration shows up as energy growth or runaway speeds.
    expectTrue(peakLate <= peakEarly, "swing decays instead of gaining energy");
    expectTrue(peakEarly < 20.f, "no runaway particle speeds");
    expectTrue(centroid(cloth).y < 5.f && centroid(cloth).y > 5.f - 3.2f, "cloth hangs below its pins, not beyond length");
    expectTrue(pinsExact, "pinned corners hold position exactly");
}

void testWindDeflectsDownwind() {
    // A hanging curtain (pinned at the row-0 corners) settles, then wind blows along +z / -z.
    auto settle = [](ClothSimulator& cloth) {
        ClothDesc desc{};
        desc.rows = 16;
        desc.cols = 16;
        desc.pinnedCorners = 0b0011u;
        cloth.init(desc, {0.f, 3.f, 0.f});
        for (int frame = 0; frame < 240; ++frame) {
            cloth.step(kDt, kGravity);
        }
    };
    // The curtain hangs in the x-y plane, so wind along +-z hits it face on.
    const vec3 winds[2] = {{0.f, 0.f, 8.f}, {0.f, 0.f, -8.f}};
    for (const vec3& wind : winds) {
        ClothSimulator calm;
        ClothSimulator windy;
        settle(calm);
        settle(windy);
        windy.applyWind(wind);
        for (int frame = 0; frame < 120; ++frame) {
            calm.step(kDt, kGravity);
            windy.step(kDt, kGravity);
        }
        const vec3 shift = centroid(windy) - centroid(calm);
        const f32 along = shift.dot(wind.normalized());
        std::printf("cloth wind (%.0f, %.0f, %.0f): centroid shift (%.3f, %.3f, %.3f), %.3f m downwind\n", wind.x,
                    wind.y, wind.z, shift.x, shift.y, shift.z, along);
        expectTrue(along > 0.05f, "wind pushes the cloth downwind");
        expectTrue(allFinite(windy), "windy cloth stays finite");
    }
}

void testClothDrapesOverSphere() {
    ClothSimulator cloth;
    ClothDesc desc{};
    desc.rows = 32;
    desc.cols = 32;
    desc.pinnedCorners = 0u;
    cloth.init(desc, {-1.55f, 1.2f, -1.55f}); // 3.1 m square centred over the sphere
    const vec3 center{0.f, 0.f, 0.f};
    const f32 radius = 0.6f;
    cloth.addSphereCollider(center, radius);

    f32 worstPenetration = 0.f;
    for (int frame = 0; frame < 180; ++frame) {
        cloth.step(kDt, kGravity);
        for (u32 i = 0; i < cloth.particleCount(); ++i) {
            worstPenetration = std::max(worstPenetration, radius - (cloth.position(i) - center).length());
        }
    }
    const vec3 middle = cloth.position(cloth.index(16, 16));
    std::printf("cloth over sphere: worst penetration %.5f m, middle particle y %.3f (sphere top %.2f)\n",
                worstPenetration, middle.y, radius);
    expectTrue(worstPenetration <= 0.f, "no particle ever ends a frame inside the sphere");
    expectTrue(middle.y > radius && middle.y < radius + 0.1f, "cloth rests on top of the sphere");
    expectTrue(allFinite(cloth), "draped cloth stays finite");
}

void testClothRestsOnBox() {
    ClothSimulator cloth;
    ClothDesc desc{};
    desc.rows = 16;
    desc.cols = 16;
    desc.pinnedCorners = 0u;
    cloth.init(desc, {-0.75f, 1.2f, -0.75f});
    const vec3 half{0.5f, 0.2f, 0.5f};
    cloth.addBoxCollider({0.f, 0.f, 0.f}, half);

    f32 worstPenetration = 0.f;
    for (int frame = 0; frame < 180; ++frame) {
        cloth.step(kDt, kGravity);
        for (u32 i = 0; i < cloth.particleCount(); ++i) {
            const vec3 p = cloth.position(i);
            if (std::fabs(p.x) < half.x && std::fabs(p.z) < half.z) {
                worstPenetration = std::max(worstPenetration, half.y - p.y);
            }
        }
    }
    const vec3 middle = cloth.position(cloth.index(8, 8));
    expectTrue(worstPenetration <= 0.f, "no particle ends a frame inside the box");
    expectTrue(middle.y > half.y && middle.y < half.y + 0.15f, "cloth rests on top of the box");
    expectTrue(allFinite(cloth), "cloth on a box stays finite");
}

void testLargeClothBudget() {
    ClothSimulator cloth;
    ClothDesc desc{};
    desc.rows = 64;
    desc.cols = 64;
    cloth.init(desc, {0.f, 5.f, 0.f});
    std::vector<double> samples;
    for (int frame = 0; frame < 61; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        cloth.step(kDt, kGravity);
        samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    std::printf("cloth 64x64: median step %.3f ms (%u substeps, %u constraints)\n", samples[30], desc.substeps,
                cloth.constraintCount());
    expectTrue(allFinite(cloth), "64x64 cloth stays finite");
#if defined(NDEBUG)
    if (fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(samples[30] < 1.0, "64x64 cloth simulation < 1 ms");
    }
#endif
}

} // namespace

int main() {
    fuse::jobs::JobScheduler::instance().initialize(3u); // bands solve on workers + caller
    testHangingClothStableAndPinned();
    testWindDeflectsDownwind();
    testClothDrapesOverSphere();
    testClothRestsOnBox();
    testLargeClothBudget();
    fuse::jobs::JobScheduler::instance().shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_cloth_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_cloth_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
