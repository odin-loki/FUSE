// Bounded CPU ReSTIR PT initial reservoir (restir_pt.hpp): a 4x4 frame of the lambert-plane fixture inside a
// closed unlit box built from the path-tracer quad helper. Two frames, static camera, jitter off.
//   pixels whose diffuse continuation hits: temporal_reuse M == 2
//   pixels with no second-vertex hit (the primary misses, or it hits the unlit box): M stays 0
//   no NaN. M is also clamped by the cap argument.
//
// Usage: fuse_relight_restir_pt_tests <initial|all>. Exit 0 pass, 1 fail.
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/pathtrace/restir_pt.hpp>
#include <fuse/relight/render/pathtrace/restir_pt_count.hpp>

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace {

using namespace pt_test;
namespace opt = fuse::relight::options;

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

/// rtx.sceneScale belongs to scene/instances (linked in the runtime); the light set reads it by name.
struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

bool finiteReservoir(const pt::RestirPtReservoir& r) {
    if (!std::isfinite(r.hitDistance) || !std::isfinite(r.M) || !std::isfinite(r.pdf)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(r.direction[i]) || !std::isfinite(r.pHat[i]) || !std::isfinite(r.camera[i]) ||
            !std::isfinite(r.primaryPos[i])) {
            return false;
        }
    }
    return true;
}

float lum(const pt::RestirPtReservoir& r) {
    return 0.2126f * r.pHat[0] + 0.7152f * r.pHat[1] + 0.0722f * r.pHat[2];
}

/// Lambert receiver (the PT lambert-plane fixture, shrunk) inside a closed unlit box.
/// The camera sits inside and looks at the receiver. Corner primary rays hit the unlit box (no diffuse
/// continuation, M = 0). Centre rays hit the receiver; any cosine direction then hits the box.
pt::PtScene enclosureScene() {
    pt::PtScene s;
    s.materials = {unlit(4.f, 3.f, 2.f), lambert(0.5f, 0.4f, 0.3f)};
    auto add = [&](pt::PtMesh mesh) {
        pt::PtInstance inst;
        inst.mesh = static_cast<u32>(s.meshes.size());
        s.meshes.push_back(std::move(mesh));
        s.instances.push_back(inst);
    };
    const float h = 3.f;
    {
        const float c[3] = {0.f, -h, 0.f}, u[3] = {0.f, 0.f, h}, v[3] = {h, 0.f, 0.f};
        add(quad(c, u, v, 0)); // floor, normal +Y
    }
    {
        const float c[3] = {0.f, h, 0.f}, u[3] = {h, 0.f, 0.f}, v[3] = {0.f, 0.f, h};
        add(quad(c, u, v, 0)); // ceiling, normal -Y
    }
    {
        const float c[3] = {0.f, 0.f, h}, u[3] = {0.f, h, 0.f}, v[3] = {h, 0.f, 0.f};
        add(quad(c, u, v, 0)); // +Z wall, normal -Z
    }
    {
        const float c[3] = {-h, 0.f, 0.f}, u[3] = {0.f, h, 0.f}, v[3] = {0.f, 0.f, h};
        add(quad(c, u, v, 0)); // left, normal +X
    }
    {
        const float c[3] = {h, 0.f, 0.f}, u[3] = {0.f, 0.f, h}, v[3] = {0.f, h, 0.f};
        add(quad(c, u, v, 0)); // right, normal -X
    }
    {
        const float c[3] = {0.f, 0.f, -h}, u[3] = {h, 0.f, 0.f}, v[3] = {0.f, h, 0.f};
        add(quad(c, u, v, 0)); // -Z wall, normal +Z
    }
    {
        const float c[3] = {0.f, 0.f, -1.f}, u[3] = {0.4f, 0.f, 0.f}, v[3] = {0.f, 0.4f, 0.f};
        add(quad(c, u, v, 1)); // diffuse receiver, normal +Z
    }
    s.camera = camera(0.f, 0.f, 1.5f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 50.f);
    return s;
}

void checkReuseRule() {
    pt::RestirPtReservoir previous;
    pt::RestirPtReservoir current;
    previous.secondHit = current.secondHit = true;
    previous.M = current.M = 1.f;
    previous.pdf = current.pdf = 0.3f;
    previous.pHat[1] = current.pHat[1] = 1.f;
    previous.direction[2] = current.direction[2] = 1.f;
    previous.hitDistance = current.hitDistance = 1.f;
    previous.primaryPos[0] = current.primaryPos[0] = 1.f;
    // No primary id: accept only while the camera and the primary hit stay under 1e-3.
    current.camera[2] = 1e-4f;
    const pt::RestirPtReservoir near = pt::temporal_reuse(previous, current, 0u, 8.f);
    check(near.M == 2.f, "epsilon: camera motion 1e-4 reuses (M = 2)");
    current.camera[2] = 1e-2f;
    const pt::RestirPtReservoir far = pt::temporal_reuse(previous, current, 0u, 8.f);
    check(far.M == 1.f, "epsilon: camera motion 1e-2 does not reuse (M = 1)");
    previous.hasPrimaryId = current.hasPrimaryId = true;
    previous.primaryInstance = current.primaryInstance = 1u;
    previous.primaryPrimitive = current.primaryPrimitive = 4u;
    current.camera[2] = 100.f;
    const pt::RestirPtReservoir same = pt::temporal_reuse(previous, current, 0u, 8.f);
    check(same.M == 2.f, "primary hit id: matching (instance, primitive) reuses despite camera motion");
    current.primaryPrimitive = 5u;
    const pt::RestirPtReservoir different = pt::temporal_reuse(previous, current, 0u, 8.f);
    check(different.M == 1.f, "primary hit id: a different primitive does not reuse");
    current.M = 5.f;
    const pt::RestirPtReservoir capped = pt::temporal_reuse(previous, current, 0u, 2.f);
    check(capped.M == 2.f, "M cap clamps a sample that is not reused");
    current.M = 1.f;
    current.primaryPrimitive = 4u;
    const pt::RestirPtReservoir cappedMerge = pt::temporal_reuse(previous, current, 3u, 1.f);
    check(cappedMerge.M == 1.f, "M cap clamps a merge of two hits to 1");
}

void suiteInitial() {
    checkReuseRule();
    pt::PtCompiledScene compiled;
    std::string error;
    const bool compiledOk = compiled.compile(enclosureScene(), {}, &error);
    check(compiledOk, "compile enclosure");
    if (!compiledOk) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return;
    }
    pt::PtSettings settings;
    settings.flags &= ~pt::kPtFlagJitter;
    constexpr u32 kWidth = 4u;
    constexpr u32 kHeight = 4u;
    pt::RestirPtCpu first;
    pt::RestirPtCpu second;
    check(first.initial(compiled, settings, kWidth, kHeight, 1u, 0u), "frame 0 initial");
    check(second.initial(compiled, settings, kWidth, kHeight, 2u, 0u), "frame 1 initial");
    if (first.reservoirs().size() != 16u || second.reservoirs().size() != 16u) {
        check(false, "4x4 reservoirs");
        return;
    }
    u32 hits = 0;
    u32 misses = 0;
    for (u32 i = 0; i < 16u; ++i) {
        const pt::RestirPtReservoir& prev = first.reservoirs()[i];
        const pt::RestirPtReservoir& cur = second.reservoirs()[i];
        check(finiteReservoir(prev), "frame 0 finite");
        check(finiteReservoir(cur), "frame 1 finite");
        const pt::RestirPtReservoir merged = pt::temporal_reuse(prev, cur, i, 8.f);
        check(finiteReservoir(merged), "reused reservoir finite");
        if (!(cur.M > 0.f)) {
            check(merged.M == 0.f, "a miss stays M=0");
            ++misses;
            continue;
        }
        check(prev.M > 0.f, "static camera: a hit pixel also hit on the previous frame");
        check(cur.hasPrimaryId && prev.hasPrimaryId && cur.primaryInstance == prev.primaryInstance &&
                  cur.primaryPrimitive == prev.primaryPrimitive,
              "static camera: primary hit id is stable");
        check(merged.M == 2.f, "reused reservoir M is 2 on a hit");
        check(merged.hitDistance > 0.f && lum(merged) > 0.f, "a hit stores a positive p_hat and distance");
        check(merged.M <= 8.f, "merged M is within the cap");
        ++hits;
    }
    std::printf("restir_pt initial 4x4: hits %u misses %u\n", hits, misses);
    check(hits > 0u, "at least one pixel hits");
    check(misses > 0u, "at least one pixel misses");
}

void testRestirPtCountOk() {
    check(fuse::relight::restir_pt_count_ok(1), "restir_pt_count_ok(1) is true");
    check(fuse::relight::restir_pt_count_ok(32), "restir_pt_count_ok(32) is true");
    check(!fuse::relight::restir_pt_count_ok(0), "restir_pt_count_ok(0) is false");
    check(!fuse::relight::restir_pt_count_ok(33), "restir_pt_count_ok(33) is false");
}

} // namespace

int main(int argc, char** argv) {
    testRestirPtCountOk();
    if (g_failures != 0) {
        return 1;
    }
    opt::setEnvironmentVariable(opt::kDxvkConfEnvVar, "");
    opt::setEnvironmentVariable(opt::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    opt::OptionManager::applyPendingValues(nullptr, false);
    const std::string suite = argc > 1 ? argv[1] : "all";
    if (suite != "all" && suite != "initial") {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    suiteInitial();
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s) (%s)\n", g_failures, suite.c_str());
        return 1;
    }
    std::printf("PASS: rl_restir_pt %s\n", suite.c_str());
    return 0;
}
