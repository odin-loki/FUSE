#include <fuse/core/init.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/toi_buffer.hpp>
#include <fuse/physics/physics_data.hpp>

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

void expectNear(f32 actual, f32 expected, f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr,
                     "FAIL: %s (expected %.5f, got %.5f)\n",
                     message,
                     expected,
                     actual);
        ++g_failures;
    }
}

void testSweptSphereSphereFindsImpact() {
    const TOIResult result = sweptSphereSphere(
        {0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, 0.5f, {5.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.5f);
    expectTrue(result.valid, "fast sphere detects impact against static sphere");
    expectNear(result.toi, 0.4f, 0.02f, "TOI matches closed-form root");
}

void testSweptSphereSphereRejectsMiss() {
    const TOIResult result = sweptSphereSphere(
        {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.5f, {0.f, 5.f, 0.f}, {0.f, 0.f, 0.f}, 0.5f);
    expectTrue(!result.valid, "parallel miss returns invalid TOI");
}

void testSweptSphereSphereBoundaryAtOne() {
    const TOIResult result = sweptSphereSphere(
        {0.f, 0.f, 0.f}, {9.f, 0.f, 0.f}, 0.5f, {10.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.5f);
    expectTrue(result.valid, "sphere-sphere sweep accepts impact at t=1 boundary");
    expectNear(result.toi, 1.f, 1e-4f, "sphere-sphere TOI lands on segment end");
}

void testSweptSphereSphereRejectsBeyondWindow() {
    const TOIResult result = sweptSphereSphere(
        {0.f, 0.f, 0.f}, {15.f, 0.f, 0.f}, 0.5f, {20.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.5f);
    expectTrue(!result.valid, "sphere-sphere sweep rejects impact beyond t=1");
}

void testSweptSphereSphereAlreadyTouching() {
    const TOIResult result = sweptSphereSphere(
        {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, 0.5f, {1.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.5f);
    expectTrue(result.valid, "touching spheres report immediate TOI");
    expectNear(result.toi, 0.f, 1e-5f, "touching spheres return t=0");
}

void testSweptSpherePlaneFindsWallImpact() {
    const TOIResult result = sweptSpherePlane(
        {0.f, 0.f, 0.f}, {0.f, 0.f, 50.f}, 0.5f, {0.f, 0.f, 1.f}, 5.f);
    expectTrue(result.valid, "fast sphere detects plane wall impact");
    expectNear(result.toi, 0.11f, 0.02f, "plane TOI prevents tunneling through wall at z=5");
}

void testSweptSpherePlaneRejectsMiss() {
    const TOIResult result = sweptSpherePlane(
        {0.f, 0.f, 10.f}, {0.f, 0.f, 50.f}, 0.5f, {0.f, 0.f, 1.f}, 5.f);
    expectTrue(!result.valid, "sphere moving away from plane returns invalid TOI");
}

void testSweptSphereSlabFindsThinWallImpact() {
    const TOIResult result = sweptSphereSlabZ(
        {0.f, 0.f, 0.f}, {0.f, 0.f, 100.f}, 0.5f, 5.f, 0.05f);
    expectTrue(result.valid, "fast sphere detects thin slab wall impact");
    expectTrue(result.toi < 0.15f, "slab TOI occurs before discrete end-of-step tunnel");
}

void testSweptSphereSlabRejectsMiss() {
    const TOIResult result = sweptSphereSlabZ(
        {0.f, 0.f, 10.f}, {0.f, 0.f, 50.f}, 0.5f, 5.f, 0.05f);
    expectTrue(!result.valid, "sphere moving away from Z slab returns invalid TOI");
}

void testSelectEarliestToiPrefersSoonerImpact() {
    TOIResult early{};
    early.valid = true;
    early.toi = 0.2f;

    TOIResult late = early;
    late.toi = 0.8f;

    const TOIResult chosen = selectEarliestToi(late, early);
    expectTrue(chosen.valid, "selectEarliestToi returns valid when both inputs valid");
    expectNear(chosen.toi, 0.2f, 1e-5f, "selectEarliestToi picks smaller TOI");
}

void testSelectEarliestToiHandlesInvalidInput() {
    TOIResult valid{};
    valid.valid = true;
    valid.toi = 0.35f;

    TOIResult invalid{};
    const TOIResult chosen = selectEarliestToi(valid, invalid);
    expectTrue(chosen.valid, "selectEarliestToi returns valid operand when one input invalid");
    expectNear(chosen.toi, 0.35f, 1e-5f, "selectEarliestToi preserves valid TOI");
}

void testSelectEarliestToiBothInvalid() {
    TOIResult invalidA{};
    TOIResult invalidB{};
    const TOIResult chosen = selectEarliestToi(invalidA, invalidB);
    expectTrue(!chosen.valid, "selectEarliestToi returns invalid when both inputs invalid");
}

void testIsToiInWindowBoundary() {
    expectTrue(isToiInWindow(0.f), "TOI window includes t=0");
    expectTrue(isToiInWindow(1.f), "TOI window includes t=1");
    expectTrue(!isToiInWindow(-0.001f), "TOI window rejects negative time");
    expectTrue(!isToiInWindow(1.001f), "TOI window rejects time beyond segment end");
}

void testMakeToiAtContactRejectsOutOfWindow() {
    const TOIResult inWindow =
        makeToiAtContact(0.5f, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 1u, 2u);
    expectTrue(inWindow.valid, "makeToiAtContact accepts in-window TOI");

    const TOIResult atZero =
        makeToiAtContact(0.f, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 1u, 2u);
    expectTrue(atZero.valid, "makeToiAtContact accepts t=0 boundary");

    const TOIResult atOne =
        makeToiAtContact(1.f, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 1u, 2u);
    expectTrue(atOne.valid, "makeToiAtContact accepts t=1 boundary");

    const TOIResult beyondWindow =
        makeToiAtContact(1.5f, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 1u, 2u);
    expectTrue(!beyondWindow.valid, "makeToiAtContact rejects out-of-window TOI");
}

void testSweptSpherePlaneAlreadyOverlapping() {
    const TOIResult result = sweptSpherePlane(
        {0.f, 0.f, 0.f}, {0.f, 0.f, 10.f}, 1.f, {0.f, 0.f, 1.f}, 0.f);
    expectTrue(result.valid, "sphere penetrating plane reports immediate TOI");
    expectNear(result.toi, 0.f, 1e-5f, "penetrating sphere returns t=0");
}

void testSweptSphereAabbRejectsStationaryMiss() {
    const aabb box{{-1.f, -1.f, 4.f}, {1.f, 1.f, 6.f}};
    const TOIResult result =
        sweptSphereAabb({0.f, 0.f, -5.f}, {0.f, 0.f, 0.f}, 0.5f, box);
    expectTrue(!result.valid, "stationary sphere outside AABB returns invalid TOI");
}

void testToiBufferPushSortOrder() {
    ToiBufferSoA buffer;
    buffer.reserve(4u);

    TOIResult late{};
    late.valid = true;
    late.toi = 0.75f;
    late.bodyA = 1u;
    late.bodyB = 2u;

    TOIResult early = late;
    early.toi = 0.1f;
    early.bodyA = 3u;

    TOIResult mid = late;
    mid.toi = 0.4f;
    mid.bodyA = 5u;

    expectTrue(buffer.push(late), "push accepts valid TOI");
    expectTrue(buffer.push(early), "push accepts second TOI");
    expectTrue(buffer.push(mid), "push accepts third TOI");
    expectTrue(buffer.activeCount == 3u, "push grows active count");

    buffer.sortByToi();
    expectNear(buffer.resultAt(0u).toi, 0.1f, 1e-5f, "sort places earliest TOI first");
    expectNear(buffer.resultAt(1u).toi, 0.4f, 1e-5f, "sort orders middle TOI");
    expectNear(buffer.resultAt(2u).toi, 0.75f, 1e-5f, "sort places latest TOI last");
    expectTrue(buffer.resultAt(0u).bodyA == 3u, "sort preserves body metadata");
    expectTrue(buffer.isSortedByToi(), "sort leaves buffer in ascending TOI order");
}

void testToiBufferStableSortTieBreak() {
    ToiBufferSoA buffer;

    TOIResult first{};
    first.valid = true;
    first.toi = 0.5f;
    first.bodyA = 2u;
    first.bodyB = 1u;

    TOIResult second = first;
    second.bodyA = 1u;
    second.bodyB = 3u;

    TOIResult third = first;
    third.bodyA = 1u;
    third.bodyB = 1u;

    buffer.push(first);
    buffer.push(second);
    buffer.push(third);
    buffer.sortByToi();

    expectTrue(buffer.isSortedByToi(), "equal-TOI buffer reports sorted");
    expectTrue(buffer.resultAt(0u).bodyA == 1u && buffer.resultAt(0u).bodyB == 1u,
               "equal TOI tie-break prefers lower bodyA then bodyB");
    expectTrue(buffer.resultAt(1u).bodyA == 1u && buffer.resultAt(1u).bodyB == 3u,
               "equal TOI tie-break orders same bodyA by bodyB");
    expectTrue(buffer.resultAt(2u).bodyA == 2u, "equal TOI tie-break places higher bodyA last");
}

void testToiBufferEmpty() {
    ToiBufferSoA buffer;
    expectTrue(buffer.activeCount == 0u, "default buffer is empty");

    TOIResult invalid{};
    expectTrue(!buffer.push(invalid), "push rejects invalid TOI on empty buffer");
    expectTrue(buffer.resultAt(0u).valid == false, "resultAt on empty buffer is invalid");

    buffer.sortByToi();
    expectTrue(buffer.activeCount == 0u, "sort on empty buffer is no-op");
    expectTrue(buffer.toVector().empty(), "toVector on empty buffer returns empty");
    expectTrue(buffer.isSortedByToi(), "empty buffer reports sorted");
}

void testToiBufferCompactAndSort() {
    ToiBufferSoA buffer;
    buffer.preparePairSlots(3u);

    TOIResult late{};
    late.valid = true;
    late.toi = 0.9f;
    late.bodyA = 1u;
    late.bodyB = 2u;

    TOIResult early = late;
    early.toi = 0.2f;
    early.bodyA = 3u;

    buffer.writeSlot(2u, late);
    buffer.writeSlot(0u, early);

    expectTrue(buffer.compactAndSort() == 2u, "compactAndSort gathers valid slots");
    expectTrue(buffer.isSortedByToi(), "compactAndSort leaves ascending TOI order");
    expectNear(buffer.resultAt(0u).toi, 0.2f, 1e-5f, "compactAndSort places earliest TOI first");
}

void testRunCcdIntoBufferEmptyPairs() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    ToiBufferSoA buffer;

    runCcdIntoBuffer({}, bodies, shapes, 1.f, buffer);

    expectTrue(buffer.isEmpty(), "empty pair list yields empty buffer");
    expectTrue(buffer.isSortedByToi(), "empty runCcdIntoBuffer buffer is sorted");
}

void testToiBufferEarliestToiEmpty() {
    ToiBufferSoA buffer;
    expectTrue(buffer.isEmpty(), "default buffer is empty");

    const TOIResult earliest = buffer.earliestToi();
    expectTrue(!earliest.valid, "earliestToi on empty buffer returns invalid");
}

void testToiBufferCompactPreservesPushMode() {
    ToiBufferSoA buffer;

    TOIResult first{};
    first.valid = true;
    first.toi = 0.3f;
    first.bodyA = 1u;

    TOIResult second = first;
    second.toi = 0.7f;
    second.bodyA = 2u;

    buffer.push(first);
    buffer.push(second);
    expectTrue(buffer.compact() == 2u, "compact preserves push-mode active count");
    expectTrue(buffer.activeCount == 2u, "push-mode compact leaves active count unchanged");
}

void testToiBufferCompactAndSortEmpty() {
    ToiBufferSoA buffer;
    expectTrue(buffer.compactAndSort() == 0u, "compactAndSort on empty buffer returns zero");
    expectTrue(buffer.isEmpty(), "compactAndSort on empty buffer stays empty");
    expectTrue(buffer.isSortedByToi(), "empty compactAndSort buffer reports sorted");

    buffer.preparePairSlots(2u);
    expectTrue(buffer.compactAndSort() == 0u, "compactAndSort on all-invalid slots returns zero");
    expectTrue(buffer.isEmpty(), "compactAndSort clears invalid-only slots");
}

void testToiBufferCompactAndSortClampsInOnePass() {
    ToiBufferSoA buffer;
    buffer.setMaxCapacity(1u);
    buffer.preparePairSlots(3u);

    TOIResult late{};
    late.valid = true;
    late.toi = 0.8f;
    late.bodyA = 1u;

    TOIResult early = late;
    early.toi = 0.2f;
    early.bodyA = 2u;

    buffer.writeSlot(0u, late);
    buffer.writeSlot(2u, early);

    expectTrue(buffer.compactAndSort() == 1u, "compactAndSort clamps to max capacity in one pass");
    expectTrue(buffer.activeCount == 1u, "clamped buffer keeps one impact");
    expectTrue(buffer.droppedCount == 1u, "clamped buffer tracks dropped impacts");
    expectNear(buffer.resultAt(0u).toi, 0.2f, 1e-5f, "compactAndSort keeps earliest TOI after clamp");
    expectTrue(buffer.isSortedByToi(), "clamped compactAndSort buffer remains sorted");
}

void testToiBufferEarliestToi() {
    ToiBufferSoA buffer;
    expectTrue(buffer.isEmpty(), "default buffer reports empty");

    TOIResult first{};
    first.valid = true;
    first.toi = 0.6f;
    first.bodyA = 1u;

    TOIResult second = first;
    second.toi = 0.15f;
    second.bodyA = 2u;

    buffer.push(first);
    buffer.push(second);
    buffer.sortByToi();

    const TOIResult earliest = buffer.earliestToi();
    expectTrue(earliest.valid, "earliestToi returns valid result after sort");
    expectNear(earliest.toi, 0.15f, 1e-5f, "earliestToi matches first sorted slot");
    expectTrue(earliest.bodyA == 2u, "earliestToi preserves body metadata");
}

void testToiBufferCapacityClamp() {
    ToiBufferSoA buffer;
    buffer.setMaxCapacity(2u);

    TOIResult first{};
    first.valid = true;
    first.toi = 0.2f;

    TOIResult second = first;
    second.toi = 0.5f;

    TOIResult third = first;
    third.toi = 0.8f;

    expectTrue(buffer.push(first), "first push fits capacity");
    expectTrue(buffer.push(second), "second push fits capacity");
    expectTrue(!buffer.push(third), "third push is clamped at max capacity");
    expectTrue(buffer.activeCount == 2u, "active count stops at max capacity");
    expectTrue(buffer.droppedCount == 1u, "dropped count tracks clamped pushes");
}

void testToiBufferCompactAlreadyPacked() {
    ToiBufferSoA buffer;
    buffer.preparePairSlots(3u);

    TOIResult first{};
    first.valid = true;
    first.toi = 0.2f;
    first.bodyA = 1u;

    TOIResult second = first;
    second.toi = 0.5f;
    second.bodyA = 2u;

    buffer.writeSlot(0u, first);
    buffer.writeSlot(1u, second);

    expectTrue(buffer.compact() == 2u, "compact on packed slots returns valid count");
    expectTrue(buffer.activeCount == 2u, "compact on packed slots preserves active count");
    expectNear(buffer.resultAt(0u).toi, 0.2f, 1e-5f, "packed compact keeps first TOI");
    expectNear(buffer.resultAt(1u).toi, 0.5f, 1e-5f, "packed compact keeps second TOI");
}

void testToiBufferCompactAllInvalidSlots() {
    ToiBufferSoA buffer;
    buffer.preparePairSlots(4u);

    expectTrue(buffer.compact() == 0u, "compact on all-invalid slots returns zero");
    expectTrue(buffer.isEmpty(), "compact on all-invalid slots clears active count");
}

void testToiBufferSortAlreadySortedEarlyOut() {
    ToiBufferSoA buffer;

    TOIResult first{};
    first.valid = true;
    first.toi = 0.1f;
    first.bodyA = 1u;

    TOIResult second = first;
    second.toi = 0.4f;
    second.bodyA = 2u;

    buffer.push(first);
    buffer.push(second);
    buffer.sortByToi();
    expectTrue(buffer.isSortedByToi(), "buffer sorted once reports sorted");

    buffer.sortByToi();
    expectTrue(buffer.isSortedByToi(), "second sort on sorted buffer is no-op");
    expectNear(buffer.resultAt(0u).toi, 0.1f, 1e-5f, "sorted early-out preserves earliest TOI");
    expectNear(buffer.resultAt(1u).toi, 0.4f, 1e-5f, "sorted early-out preserves later TOI");
}

void testToiBufferApplyMaxCapacityClampEmpty() {
    ToiBufferSoA buffer;
    buffer.setMaxCapacity(2u);

    expectTrue(buffer.applyMaxCapacityClamp() == 0u, "clamp on empty buffer returns zero");
    expectTrue(buffer.isEmpty(), "clamp on empty buffer stays empty");
    expectTrue(buffer.droppedCount == 0u, "clamp on empty buffer does not increment dropped count");
}

void testToiBufferApplyMaxCapacityClampPushMode() {
    ToiBufferSoA buffer;

    TOIResult late{};
    late.valid = true;
    late.toi = 0.9f;
    late.bodyA = 1u;

    TOIResult mid = late;
    mid.toi = 0.5f;
    mid.bodyA = 2u;

    TOIResult early = late;
    early.toi = 0.1f;
    early.bodyA = 3u;

    buffer.push(late);
    buffer.push(mid);
    buffer.push(early);
    expectTrue(buffer.activeCount == 3u, "push mode accumulates three impacts");

    buffer.setMaxCapacity(2u);
    expectTrue(buffer.activeCount == 2u, "setMaxCapacity trims push-mode buffer");
    expectTrue(buffer.droppedCount == 1u, "setMaxCapacity tracks dropped push-mode impacts");
    expectTrue(buffer.toiValues.size() == 2u, "push-mode clamp shrinks SoA storage");
    expectNear(buffer.resultAt(0u).toi, 0.1f, 1e-5f, "push-mode clamp keeps earliest TOI");
    expectNear(buffer.resultAt(1u).toi, 0.5f, 1e-5f, "push-mode clamp keeps next-earliest TOI");
    expectTrue(buffer.isSortedByToi(), "push-mode clamp leaves sorted order");
}

void testToiBufferIsFull() {
    ToiBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    expectTrue(!buffer.isFull(), "empty buffer is not full");

    TOIResult result{};
    result.valid = true;
    result.toi = 0.3f;

    buffer.push(result);
    expectTrue(!buffer.isFull(), "partial buffer is not full");

    buffer.push(result);
    expectTrue(buffer.isFull(), "buffer at max capacity reports full");
    expectTrue(!buffer.push(result), "push on full buffer is rejected");
    expectTrue(buffer.droppedCount == 1u, "push on full buffer increments dropped count");
}

void testToiBufferWriteSlotInvalidSlot() {
    ToiBufferSoA buffer;
    buffer.preparePairSlots(2u);

    TOIResult valid{};
    valid.valid = true;
    valid.toi = 0.4f;
    valid.bodyA = 1u;

    buffer.writeSlot(2u, valid);
    expectTrue(buffer.countValidSlots() == 0u, "out-of-range writeSlot is ignored");
    expectTrue(!buffer.isValidSlot(2u), "isValidSlot rejects out-of-range index");

    buffer.writeSlot(0u, valid);
    expectTrue(buffer.countValidSlots() == 1u, "in-range writeSlot is accepted");
    expectTrue(buffer.isValidSlot(0u), "isValidSlot accepts in-range index");
}

void testToiBufferWriteSlotInvalidResult() {
    ToiBufferSoA buffer;
    buffer.preparePairSlots(2u);

    TOIResult invalid{};
    buffer.writeSlot(0u, invalid);
    expectTrue(buffer.countValidSlots() == 0u, "writeSlot ignores invalid TOI result");
    expectTrue(buffer.validFlags[0u] == 0u, "invalid writeSlot leaves flag cleared");
}

void testToiBufferWriteSlotRequiresPairSlots() {
    ToiBufferSoA buffer;

    TOIResult valid{};
    valid.valid = true;
    valid.toi = 0.3f;

    buffer.writeSlot(0u, valid);
    expectTrue(buffer.isEmpty(), "writeSlot without pair slots is a no-op");
    expectTrue(buffer.countValidSlots() == 0u, "push-mode buffer reports zero valid slots");
}

void testToiBufferPushRejectsPairSlotMode() {
    ToiBufferSoA buffer;
    buffer.preparePairSlots(2u);

    TOIResult valid{};
    valid.valid = true;
    valid.toi = 0.5f;

    expectTrue(!buffer.push(valid), "push rejects pair-slot mode buffer");
    expectTrue(buffer.isEmpty(), "pair-slot mode push does not grow active count");
}

void testToiBufferPreparePairSlotsZero() {
    ToiBufferSoA buffer;
    buffer.preparePairSlots(2u);

    TOIResult valid{};
    valid.valid = true;
    valid.toi = 0.25f;
    buffer.writeSlot(0u, valid);

    buffer.preparePairSlots(0u);
    expectTrue(buffer.pairSlotCount == 0u, "preparePairSlots(0) clears pair slot count");
    expectTrue(buffer.isEmpty(), "preparePairSlots(0) leaves buffer empty");
    expectTrue(buffer.toiValues.empty(), "preparePairSlots(0) clears SoA storage");
}

void testToiBufferSetMaxCapacityUnlimited() {
    ToiBufferSoA buffer;
    buffer.setMaxCapacity(0u);

    TOIResult result{};
    result.valid = true;
    result.toi = 0.2f;

    for (u32 i = 0u; i < 5u; ++i) {
        expectTrue(buffer.push(result), "unlimited capacity accepts pushes");
    }
    expectTrue(buffer.activeCount == 5u, "unlimited capacity keeps all impacts");
    expectTrue(!buffer.isFull(), "zero max capacity means not full");
    expectTrue(buffer.applyMaxCapacityClamp() == 5u, "clamp with zero max is a no-op");
}

void testToiBufferResultAtInvalidIndex() {
    ToiBufferSoA buffer;

    TOIResult valid{};
    valid.valid = true;
    valid.toi = 0.35f;
    buffer.push(valid);

    expectTrue(buffer.resultAt(1u).valid == false, "resultAt beyond active count is invalid");
    expectTrue(buffer.resultAt(0u).valid, "resultAt at zero returns valid entry");
}

void testToiBufferSingleElementSortEarlyOut() {
    ToiBufferSoA buffer;

    TOIResult lone{};
    lone.valid = true;
    lone.toi = 0.55f;
    lone.bodyA = 7u;

    buffer.push(lone);
    buffer.sortByToi();
    expectTrue(buffer.isSortedByToi(), "single-element buffer reports sorted");
    expectNear(buffer.resultAt(0u).toi, 0.55f, 1e-5f, "single-element sort preserves TOI");
}

void testToiBufferCountValidSlots() {
    ToiBufferSoA buffer;
    buffer.preparePairSlots(4u);

    TOIResult first{};
    first.valid = true;
    first.toi = 0.2f;
    first.bodyA = 1u;

    TOIResult second = first;
    second.toi = 0.6f;
    second.bodyA = 2u;

    buffer.writeSlot(1u, first);
    buffer.writeSlot(3u, second);
    expectTrue(buffer.countValidSlots() == 2u, "countValidSlots tallies sparse valid flags");

    buffer.compact();
    expectTrue(buffer.activeCount == 2u, "compact aligns active count with valid slots");
    expectTrue(buffer.countValidSlots() == 2u, "countValidSlots matches compacted active count");
}

void testToiBufferClearResetsDroppedCount() {
    ToiBufferSoA buffer;
    buffer.setMaxCapacity(1u);

    TOIResult result{};
    result.valid = true;
    result.toi = 0.4f;

    buffer.push(result);
    buffer.push(result);
    expectTrue(buffer.droppedCount == 1u, "full buffer tracks dropped pushes");

    buffer.clear();
    expectTrue(buffer.droppedCount == 0u, "clear resets dropped count");
    expectTrue(buffer.isEmpty(), "clear leaves buffer empty");
}

void testToiBufferApplyMaxCapacityClamp() {
    ToiBufferSoA buffer;
    buffer.setMaxCapacity(2u);
    buffer.preparePairSlots(4u);

    TOIResult late{};
    late.valid = true;
    late.toi = 0.9f;
    late.bodyA = 1u;

    TOIResult mid = late;
    mid.toi = 0.5f;
    mid.bodyA = 2u;

    TOIResult early = late;
    early.toi = 0.1f;
    early.bodyA = 3u;

    buffer.writeSlot(0u, late);
    buffer.writeSlot(1u, early);
    buffer.writeSlot(3u, mid);

    buffer.compact();
    buffer.sortByToi();
    expectTrue(buffer.activeCount == 3u, "compact gathers all valid slots before clamp");

    expectTrue(buffer.applyMaxCapacityClamp() == 2u, "applyMaxCapacityClamp truncates to max capacity");
    expectTrue(buffer.activeCount == 2u, "active count stops at max capacity after clamp");
    expectTrue(buffer.droppedCount == 1u, "dropped count tracks truncated TOIs");
    expectNear(buffer.resultAt(0u).toi, 0.1f, 1e-5f, "clamp keeps earliest TOI");
    expectNear(buffer.resultAt(1u).toi, 0.5f, 1e-5f, "clamp keeps next-earliest TOI");
    expectTrue(buffer.isSortedByToi(), "clamped buffer remains sorted");
}

void testSweptSphereAabbFindsImpact() {
    const aabb box{{-1.f, -1.f, 4.f}, {1.f, 1.f, 6.f}};
    const TOIResult result =
        sweptSphereAabb({0.f, 0.f, 0.f}, {0.f, 0.f, 10.f}, 0.5f, box);
    expectTrue(result.valid, "sphere sweep detects AABB entry");
    expectTrue(result.toi < 0.5f, "AABB TOI occurs before end of segment");
}

void testSweptSphereAabbRejectsMiss() {
    const aabb box{{5.f, 5.f, 5.f}, {6.f, 6.f, 6.f}};
    const TOIResult result =
        sweptSphereAabb({0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, 0.5f, box);
    expectTrue(!result.valid, "parallel miss against distant AABB is invalid");
}

void testSweptSphereAabbStartsInsideBox() {
    const aabb box{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const TOIResult result =
        sweptSphereAabb({0.f, 0.f, 0.f}, {0.f, 0.f, 5.f}, 0.25f, box);
    expectTrue(result.valid, "sphere starting inside expanded AABB reports immediate TOI");
    expectNear(result.toi, 0.f, 1e-5f, "inside AABB sweep returns t=0");
}

void testSweptSphereAabbRejectsBeyondWindow() {
    const aabb box{{-1.f, -1.f, 12.f}, {1.f, 1.f, 14.f}};
    const TOIResult result =
        sweptSphereAabb({0.f, 0.f, 0.f}, {0.f, 0.f, 5.f}, 0.5f, box);
    expectTrue(!result.valid, "AABB entry beyond t=1 is invalid");
}

void testToiBufferClearReuse() {
    ToiBufferSoA buffer;
    buffer.reserve(8u);
    buffer.preparePairSlots(2u);
    TOIResult first{};
    first.valid = true;
    first.toi = 0.25f;
    first.bodyA = 0u;
    first.bodyB = 1u;
    buffer.writeSlot(0u, first);
    expectTrue(buffer.compact() == 1u, "compact keeps valid TOI slot");

    buffer.clear();
    expectTrue(buffer.activeCount == 0u, "clear resets active count");
    expectTrue(buffer.pairSlotCount == 0u, "clear resets pair slots");

    buffer.preparePairSlots(4u);
    TOIResult second = first;
    second.bodyB = 2u;
    buffer.writeSlot(1u, second);
    buffer.writeSlot(3u, first);
    expectTrue(buffer.compact() == 2u, "reuse after clear compacts new TOIs");
}

void testRunCcdIntoBufferMaxCapacityKeepsEarliest() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 nearSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 midSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 farSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 target = bodies.addBody({8.f, 0.f, 0.f}, 1.f, 0);

    bodies.linearVelocities[nearSphere] = {25.f, 0.f, 0.f};
    bodies.linearVelocities[midSphere] = {16.f, 0.f, 0.f};
    bodies.linearVelocities[farSphere] = {12.f, 0.f, 0.f};

    shapes.addShape(CollisionShapeType::Sphere, nearSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, midSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, farSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, target, {0.5f, 0.f, 0.f});

    const std::vector<broadphase::CandidatePair> pairs = {
        {farSphere, target},
        {nearSphere, target},
        {midSphere, target},
    };

    ToiBufferSoA buffer;
    buffer.setMaxCapacity(1u);
    runCcdIntoBuffer(pairs, bodies, shapes, 1.f, buffer);

    expectTrue(buffer.activeCount == 1u, "max-capacity CCD keeps one earliest impact");
    expectTrue(buffer.droppedCount == 2u, "max-capacity CCD drops later impacts");
    expectTrue(buffer.resultAt(0u).bodyA == nearSphere, "max-capacity CCD keeps fastest mover");
    expectTrue(buffer.isSortedByToi(), "max-capacity CCD buffer remains sorted");
}

void testRunCcdIntoBufferSortsEarliestFirst() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 nearSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 midSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 farSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 target = bodies.addBody({8.f, 0.f, 0.f}, 1.f, 0);

    bodies.linearVelocities[nearSphere] = {25.f, 0.f, 0.f};
    bodies.linearVelocities[midSphere] = {16.f, 0.f, 0.f};
    bodies.linearVelocities[farSphere] = {12.f, 0.f, 0.f};

    shapes.addShape(CollisionShapeType::Sphere, nearSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, midSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, farSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, target, {0.5f, 0.f, 0.f});

    const std::vector<broadphase::CandidatePair> pairs = {
        {farSphere, target},
        {nearSphere, target},
        {midSphere, target},
    };

    ToiBufferSoA buffer;
    runCcdIntoBuffer(pairs, bodies, shapes, 1.f, buffer);

    expectTrue(buffer.activeCount == 3u, "multi-pair CCD resolves all flagged impacts");
    expectNear(buffer.resultAt(0u).toi, buffer.earliestToi().toi, 1e-5f, "earliestToi matches first slot");
    expectTrue(buffer.resultAt(0u).toi <= buffer.resultAt(1u).toi, "buffer sorted ascending by TOI");
    expectTrue(buffer.resultAt(1u).toi <= buffer.resultAt(2u).toi, "buffer keeps later TOIs ordered");
    expectTrue(buffer.resultAt(0u).bodyA == nearSphere, "earliest impact belongs to fastest mover");
}

void testRunCcdIntoBufferSphereBoxAabb() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 fastSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 boxBody = bodies.addBody({0.f, 0.f, 8.f}, 0.f, RB_STATIC);
    bodies.linearVelocities[fastSphere] = {0.f, 0.f, 40.f};
    shapes.addShape(CollisionShapeType::Sphere, fastSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Box, boxBody, {2.f, 2.f, 1.f});

    const std::vector<broadphase::CandidatePair> pairs = {{fastSphere, boxBody}};

    ToiBufferSoA buffer;
    runCcdIntoBuffer(pairs, bodies, shapes, 1.f, buffer);

    expectTrue(buffer.activeCount == 1u, "sphere-box pair uses AABB sweep dispatch");
    const TOIResult result = buffer.resultAt(0u);
    expectTrue(result.valid, "sphere-box TOI is valid");
    expectTrue(result.toi < 0.25f, "sphere-box TOI catches mover before tunneling");
}

void testRunCcdIntoBufferJobSafe() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 fastSphere = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 wallBody = bodies.addBody({0.f, 0.f, 5.f}, 0.f, RB_STATIC);
    bodies.linearVelocities[fastSphere] = {0.f, 0.f, 50.f};
    shapes.addShape(CollisionShapeType::Sphere, fastSphere, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Box, wallBody, {10.f, 10.f, 0.05f});

    const std::vector<broadphase::CandidatePair> pairs = {{fastSphere, wallBody}};

    ToiBufferSoA buffer;
    buffer.reserve(1u);
    runCcdIntoBuffer(pairs, bodies, shapes, 1.f, buffer);

    expectTrue(buffer.activeCount == 1u, "job-safe CCD resolves sphere-thin-wall pair");
    const TOIResult result = buffer.resultAt(0u);
    expectTrue(result.valid, "buffer TOI valid");
    expectTrue(result.bodyA == fastSphere && result.bodyB == wallBody, "buffer preserves body indices");
    expectTrue(result.toi < 0.15f, "buffer TOI catches fast mover before tunneling");
}

void testCcdPipelineFiltersRbCcdFlag() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, RB_CCD);
    const u32 bodyB = bodies.addBody({5.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[bodyA] = {10.f, 0.f, 0.f};
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.5f, 0.f, 0.f});

    std::vector<broadphase::CandidatePair> pairs = {{bodyA, bodyB}};
    std::vector<TOIResult> results;

    CcdPipeline pipeline;
    const u32 count = pipeline.sweepPairs(bodies, shapes, pairs, 1.f, results);
    expectTrue(count == 1u, "CCD pipeline emits one TOI for flagged fast body");
    expectTrue(!results.empty() && results[0].valid, "pipeline TOI is valid");
    expectNear(results[0].toi, 0.4f, 0.03f, "pipeline TOI matches analytic sweep");
}

void testCcdPipelineSkipsUnflaggedBodies() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 bodyA = bodies.addBody({0.f, 0.f, 0.f}, 1.f, 0);
    const u32 bodyB = bodies.addBody({5.f, 0.f, 0.f}, 1.f, 0);
    bodies.linearVelocities[bodyA] = {10.f, 0.f, 0.f};
    shapes.addShape(CollisionShapeType::Sphere, bodyA, {0.5f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Sphere, bodyB, {0.5f, 0.f, 0.f});

    std::vector<broadphase::CandidatePair> pairs = {{bodyA, bodyB}};
    std::vector<TOIResult> results;

    CcdPipeline pipeline;
    const u32 count = pipeline.sweepPairs(bodies, shapes, pairs, 1.f, results);
    expectTrue(count == 0u, "CCD pipeline skips pairs without RB_CCD flag");
}

void testCcdPipelineSpherePlanePair() {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;

    const u32 fastSphere = bodies.addBody({0.f, 5.f, 0.f}, 1.f, RB_CCD);
    const u32 ground = bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    bodies.linearVelocities[fastSphere] = {0.f, -10.f, 0.f};
    shapes.addShape(CollisionShapeType::Sphere, fastSphere, {1.f, 0.f, 0.f});
    shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);

    std::vector<broadphase::CandidatePair> pairs = {{fastSphere, ground}};
    std::vector<TOIResult> results;

    CcdPipeline pipeline;
    const u32 count = pipeline.sweepPairs(bodies, shapes, pairs, 1.f, results);
    expectTrue(count == 1u, "CCD pipeline handles sphere-plane sweep");
    expectTrue(!results.empty() && results[0].valid, "sphere-plane TOI is valid");
    expectNear(results[0].toi, 0.4f, 0.03f, "sphere-plane TOI matches analytic sweep");
}

} // namespace

int main() {
    fuse::core::initialize();
    testSweptSphereSphereFindsImpact();
    testSweptSphereSphereRejectsMiss();
    testSweptSphereSphereBoundaryAtOne();
    testSweptSphereSphereRejectsBeyondWindow();
    testSweptSphereSphereAlreadyTouching();
    testSweptSpherePlaneFindsWallImpact();
    testSweptSpherePlaneRejectsMiss();
    testSweptSphereSlabFindsThinWallImpact();
    testSweptSphereSlabRejectsMiss();
    testSelectEarliestToiPrefersSoonerImpact();
    testSelectEarliestToiHandlesInvalidInput();
    testSelectEarliestToiBothInvalid();
    testIsToiInWindowBoundary();
    testMakeToiAtContactRejectsOutOfWindow();
    testSweptSpherePlaneAlreadyOverlapping();
    testSweptSphereAabbRejectsStationaryMiss();
    testToiBufferPushSortOrder();
    testToiBufferStableSortTieBreak();
    testToiBufferEmpty();
    testToiBufferCompactAndSort();
    testRunCcdIntoBufferEmptyPairs();
    testToiBufferEarliestToiEmpty();
    testToiBufferCompactPreservesPushMode();
    testToiBufferCompactAndSortEmpty();
    testToiBufferCompactAndSortClampsInOnePass();
    testToiBufferEarliestToi();
    testToiBufferCapacityClamp();
    testToiBufferCompactAlreadyPacked();
    testToiBufferCompactAllInvalidSlots();
    testToiBufferSortAlreadySortedEarlyOut();
    testToiBufferApplyMaxCapacityClampEmpty();
    testToiBufferApplyMaxCapacityClampPushMode();
    testToiBufferIsFull();
    testToiBufferWriteSlotInvalidSlot();
    testToiBufferWriteSlotInvalidResult();
    testToiBufferWriteSlotRequiresPairSlots();
    testToiBufferPushRejectsPairSlotMode();
    testToiBufferPreparePairSlotsZero();
    testToiBufferSetMaxCapacityUnlimited();
    testToiBufferResultAtInvalidIndex();
    testToiBufferSingleElementSortEarlyOut();
    testToiBufferCountValidSlots();
    testToiBufferClearResetsDroppedCount();
    testToiBufferApplyMaxCapacityClamp();
    testSweptSphereAabbFindsImpact();
    testSweptSphereAabbRejectsMiss();
    testSweptSphereAabbStartsInsideBox();
    testSweptSphereAabbRejectsBeyondWindow();
    testToiBufferClearReuse();
    testRunCcdIntoBufferMaxCapacityKeepsEarliest();
    testRunCcdIntoBufferSortsEarliestFirst();
    testRunCcdIntoBufferSphereBoxAabb();
    testRunCcdIntoBufferJobSafe();
    testCcdPipelineFiltersRbCcdFlag();
    testCcdPipelineSkipsUnflaggedBodies();
    testCcdPipelineSpherePlanePair();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_ccd_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_ccd_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
