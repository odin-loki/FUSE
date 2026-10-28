#include <fuse/mechanics/proximity_component.hpp>
#include <fuse/mechanics/proximity_m.hpp>
#include <fuse/mechanics/random_component.hpp>
#include <fuse/mechanics/scale_component.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testRandomDeterminism() {
    fuse::mechanics::RandomComponent a("seq_a", 1u);
    fuse::mechanics::RandomComponent b("seq_b", 1u);

    const float first = a.next();
    const float second = a.next();
    expectTrue(first == (1056.0f / 16777216.0f), "xorshift32 first sample");
    expectTrue(second == (264198.0f / 16777216.0f), "xorshift32 second sample");
    expectTrue(first >= 0.f && first < 1.f, "first sample in [0,1)");
    expectTrue(second >= 0.f && second < 1.f, "second sample in [0,1)");
    expectTrue(first != second, "xorshift sequence advances");
    expectTrue(b.next() == first, "same seed matches first sample");
    expectTrue(b.next() == second, "same seed matches second sample");

    bool matched = true;
    bool inRange = true;
    for (int i = 0; i < 16; ++i) {
        const float av = a.next();
        const float bv = b.next();
        if (av != bv) {
            matched = false;
        }
        if (!(av >= 0.f && av < 1.f)) {
            inRange = false;
        }
    }
    expectTrue(matched, "same seed same sequence");
    expectTrue(inRange, "samples stay in [0,1)");

    fuse::mechanics::RandomComponent other("other", 2u);
    expectTrue(other.next() != first, "different seed diverges");

    a.setSeed(1u);
    expectTrue(a.seed() == 1u, "setSeed stores the seed");
    expectTrue(a.next() == first, "setSeed restarts sequence");

    fuse::mechanics::RandomComponent zeroA("zero_a", 0u);
    fuse::mechanics::RandomComponent zeroB("zero_b", 0u);
    const float z0 = zeroA.next();
    const float z1 = zeroA.next();
    expectTrue(zeroA.seed() == 0u, "seed 0 is stored");
    expectTrue(z0 == zeroB.next(), "seed 0 is deterministic");
    expectTrue(z1 == zeroB.next(), "seed 0 sequence matches");
    expectTrue(z0 != z1, "seed 0 still advances");
    expectTrue(z0 >= 0.f && z0 < 1.f, "seed 0 sample in [0,1)");
    expectTrue(z0 != first, "seed 0 differs from seed 1");
}

void testRandomTickGate() {
    fuse::mechanics::RandomComponent idle("idle", 99u);
    fuse::mechanics::RandomComponent clean("clean", 99u);
    expectTrue(!idle.stepOnTick(), "stepOnTick defaults off");
    idle.tick();
    idle.tick();
    expectTrue(idle.drawCount() == 0u, "tick does not draw when stepOnTick is off");
    expectTrue(idle.value() == 0.f, "idle tick leaves value unchanged");
    expectTrue(idle.next() == clean.next(), "idle ticks do not consume the sequence");

    fuse::mechanics::RandomComponent stepped("stepped", 99u);
    fuse::mechanics::RandomComponent manual("manual", 99u);
    stepped.setStepOnTick(true);
    expectTrue(stepped.stepOnTick(), "stepOnTick can be set");
    stepped.tick();
    expectTrue(stepped.drawCount() == 1u, "stepOnTick draws one sample");
    expectTrue(stepped.tickCount() == 1u, "stepping tick is counted");
    expectTrue(stepped.value() == manual.next(), "stepOnTick matches next");

    stepped.setStepOnTick(false);
    stepped.tick();
    expectTrue(stepped.drawCount() == 1u, "clearing stepOnTick stops draws");
    expectTrue(stepped.tickCount() == 1u, "non-stepping tick is not counted");
    expectTrue(stepped.next() == manual.next(), "sequence resumes where tick left it");
}

void testScaleArrivesOnce() {
    fuse::u32 fires = 0;
    fuse::mechanics::ScaleComponent idle("idle", 1.f);
    idle.setOnArrive([&]() { ++fires; });
    idle.tick(1.f);
    expectTrue(fires == 0u, "scale already at target does not arrive");
    expectTrue(idle.arriveCount() == 0u, "arrive count stays 0 while already there");

    fuse::mechanics::ScaleComponent grow("grow", 2.f);
    grow.setUniform(true);
    grow.setScale(0.f);
    grow.setTarget(5.f);
    grow.setOnArrive([&]() { ++fires; });
    expectTrue(!grow.arrived(), "uniform scale starts short of target");
    expectTrue(fires == 0u, "scale callback waits for arrival");

    grow.tick(1.f);
    expectNear(grow.x(), 2.f, 0.0001f, "uniform scale steps at rate");
    expectNear(grow.y(), 2.f, 0.0001f, "uniform scale writes y");
    expectNear(grow.z(), 2.f, 0.0001f, "uniform scale writes z");
    expectTrue(!grow.arrived(), "uniform scale has not arrived yet");
    expectTrue(fires == 0u, "partial step does not arrive");

    grow.tick(1.f);
    expectNear(grow.x(), 4.f, 0.0001f, "uniform scale second step");
    expectTrue(fires == 0u, "still short of target");

    grow.tick(1.f);
    expectNear(grow.x(), 5.f, 0.0001f, "uniform scale clamps onto target");
    expectTrue(grow.arrived(), "uniform scale arrives");
    expectTrue(grow.arriveCount() == 1u, "uniform arrive count");
    expectTrue(fires == 1u, "uniform arrival fires once");

    grow.tick(1.f);
    grow.tick(2.f);
    expectTrue(fires == 1u, "sitting on target does not fire again");
    expectTrue(grow.arriveCount() == 1u, "arrive count stays 1 while sitting");

    grow.setTarget(9.f);
    expectTrue(!grow.arrived(), "new target clears arrival");
    grow.tick(1.f);
    expectTrue(fires == 1u, "new journey does not fire early");
    grow.tick(10.f);
    expectNear(grow.x(), 9.f, 0.0001f, "second journey reaches target");
    expectTrue(fires == 2u, "second arrival fires once");
    grow.tick(1.f);
    expectTrue(fires == 2u, "second arrival does not repeat");

    fuse::mechanics::ScaleComponent placed("placed", 4.f);
    fuse::u32 placedFires = 0;
    placed.setOnArrive([&]() { ++placedFires; });
    placed.setTarget(3.f);
    placed.setScale(3.f);
    placed.tick(1.f);
    expectTrue(placed.arrived(), "scale placed on target is arrived");
    expectTrue(placedFires == 0u, "placing on target is not an arrival");
}

void testScaleXyzAndZeroRate() {
    fuse::u32 fires = 0;
    fuse::mechanics::ScaleComponent box("box", 2.f);
    box.setUniform(false);
    box.setScale(0.f, 0.f, 0.f);
    box.setTarget(2.f, 4.f, 1.f);
    box.setOnArrive([&]() { ++fires; });

    box.tick(1.f);
    expectNear(box.x(), 2.f, 0.0001f, "xyz x reaches early");
    expectNear(box.y(), 2.f, 0.0001f, "xyz y steps at rate");
    expectNear(box.z(), 1.f, 0.0001f, "xyz z clamps when step exceeds gap");
    expectTrue(!box.arrived(), "xyz waits for every axis");
    expectTrue(fires == 0u, "early axes do not arrive");

    box.tick(1.f);
    expectNear(box.x(), 2.f, 0.0001f, "xyz x stays on target");
    expectNear(box.y(), 4.f, 0.0001f, "xyz y arrives");
    expectNear(box.z(), 1.f, 0.0001f, "xyz z stays on target");
    expectTrue(box.arrived(), "xyz arrives when the last axis arrives");
    expectTrue(fires == 1u, "xyz arrival fires once");

    box.tick(3.f);
    expectTrue(fires == 1u, "xyz arrival does not repeat");
    expectTrue(box.arriveCount() == 1u, "xyz arrive count stays 1");

    fuse::mechanics::ScaleComponent snap("snap", 0.f);
    snap.setUniform(false);
    snap.setSnapWhenZero(true);
    snap.setScale(1.f, 2.f, 3.f);
    snap.setTarget(4.f, 5.f, 6.f);
    fuse::u32 snapFires = 0;
    snap.setOnArrive([&]() { ++snapFires; });
    snap.tick(0.f);
    expectNear(snap.x(), 4.f, 0.0001f, "zero rate snap x");
    expectNear(snap.y(), 5.f, 0.0001f, "zero rate snap y");
    expectNear(snap.z(), 6.f, 0.0001f, "zero rate snap z");
    expectTrue(snap.arrived(), "zero rate snap arrives");
    expectTrue(snapFires == 1u, "zero rate snap fires once");
    snap.tick(1.f);
    expectTrue(snapFires == 1u, "zero rate snap does not repeat");

    fuse::mechanics::ScaleComponent hold("hold", 0.f);
    hold.setUniform(false);
    hold.setSnapWhenZero(false);
    hold.setScale(1.f, 2.f, 3.f);
    hold.setTarget(4.f, 5.f, 6.f);
    fuse::u32 holdFires = 0;
    hold.setOnArrive([&]() { ++holdFires; });
    hold.tick(5.f);
    expectNear(hold.x(), 1.f, 0.0001f, "zero rate without snap holds x");
    expectNear(hold.y(), 2.f, 0.0001f, "zero rate without snap holds y");
    expectNear(hold.z(), 3.f, 0.0001f, "zero rate without snap holds z");
    expectTrue(!hold.arrived(), "zero rate without snap does not arrive");
    expectTrue(holdFires == 0u, "zero rate without snap does not fire");
}

void testProximityEnterLeave() {
    fuse::mechanics::ProximityComponent zone("camp", 5.f);
    zone.setCenter(10.f, 0.f, 0.f);

    fuse::u32 enters = 0;
    fuse::u32 leaves = 0;
    fuse::u32 enteredId = 0;
    fuse::u32 leftId = 0;
    zone.setOnEnter([&](fuse::u32 id) {
        ++enters;
        enteredId = id;
    });
    zone.setOnLeave([&](fuse::u32 id) {
        ++leaves;
        leftId = id;
    });

    zone.testObject(42, 100.f, 0.f, 0.f);
    zone.testObject(42, 100.f, 0.f, 0.f);
    expectTrue(enters == 0u, "outside point does not enter");
    expectTrue(leaves == 0u, "outside point does not leave");

    zone.testObject(42, 10.f, 0.f, 0.f);
    expectTrue(zone.enterCount() == 1u, "proximity enter fires once");
    expectTrue(enters == 1u, "proximity enter callback fires once");
    expectTrue(enteredId == 42u, "proximity enter records object id");

    zone.testObject(42, 12.f, 0.f, 0.f);
    zone.testObject(42, 13.f, 0.f, 0.f);
    zone.testObject(42, 10.f, 3.f, 4.f);
    expectTrue(zone.enterCount() == 1u, "staying inside does not repeat enter");
    expectTrue(enters == 1u, "staying inside does not repeat callback");
    expectTrue(zone.containsPoint(10.f, 3.f, 4.f), "sphere surface is inside");

    zone.testObject(42, 16.f, 0.f, 0.f);
    expectTrue(zone.leaveCount() == 1u, "proximity leave fires on exit");
    expectTrue(leaves == 1u, "proximity leave callback fires once");
    expectTrue(leftId == 42u, "proximity leave records object id");

    zone.testObject(42, 20.f, 0.f, 0.f);
    zone.testObject(42, 30.f, 4.f, 0.f);
    expectTrue(zone.leaveCount() == 1u, "staying outside does not repeat leave");
    expectTrue(leaves == 1u, "staying outside does not repeat callback");

    zone.testObject(42, 10.f, 0.f, 0.f);
    expectTrue(zone.enterCount() == 2u, "re-enter is a new crossing");
    expectTrue(enters == 2u, "re-enter callback fires once");
    zone.testObject(42, 10.f, 0.f, 0.f);
    expectTrue(enters == 2u, "re-enter does not repeat while inside");

    zone.testObject(7, 10.f, 0.f, 0.f);
    expectTrue(enters == 3u, "second object enters on its own crossing");
    expectTrue(enteredId == 7u, "second object id is recorded");
    zone.testObject(7, 10.f, 0.f, 0.f);
    zone.testObject(42, 10.f, 0.f, 0.f);
    expectTrue(enters == 3u, "neither object repeats enter while inside");

    fuse::mechanics::ProximityComponent bare("bare", 1.f);
    bare.testObject(1, 0.f, 0.f, 0.f);
    bare.testObject(1, 0.f, 0.f, 0.f);
    bare.testObject(1, 2.f, 0.f, 0.f);
    bare.testObject(1, 3.f, 0.f, 0.f);
    expectTrue(bare.enterCount() == 1u, "missing callback still counts one enter");
    expectTrue(bare.leaveCount() == 1u, "missing callback still counts one leave");
}

void testProximityMOk() {
    expectTrue(fuse::mechanics::proximity_m_ok(0.5f), "proximity_m_ok(0.5) is true");
    expectTrue(fuse::mechanics::proximity_m_ok(100.f), "proximity_m_ok(100) is true");
    expectTrue(!fuse::mechanics::proximity_m_ok(0.f), "proximity_m_ok(0) is false");
    expectTrue(!fuse::mechanics::proximity_m_ok(-1.f), "proximity_m_ok(-1) is false");
    expectTrue(!fuse::mechanics::proximity_m_ok(100.1f), "proximity_m_ok(100.1) is false");
    const float nonFinite = std::numeric_limits<float>::quiet_NaN();
    expectTrue(!fuse::mechanics::proximity_m_ok(nonFinite), "proximity_m_ok(non-finite) is false");
}

} // namespace

int main() {
    testProximityMOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mechanics_random_scale_proximity: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    fuse::core::initialize();

    testRandomDeterminism();
    testRandomTickGate();
    testScaleArrivesOnce();
    testScaleXyzAndZeroRate();
    testProximityEnterLeave();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_random_scale_proximity: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_random_scale_proximity: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
