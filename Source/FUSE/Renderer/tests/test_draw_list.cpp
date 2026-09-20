#include <fuse/renderer/draw_list.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testEmptyList() {
    fuse::renderer::DrawList list;
    expectTrue(list.count() == 0u, "empty count is 0");
    expectTrue(list.data() == nullptr, "empty data is nullptr");
    expectTrue(list.totalIndexCount() == 0u, "empty totalIndexCount is 0");
}

void testPushRejectsZeroIndexCount() {
    fuse::renderer::DrawList list;
    fuse::renderer::DrawCall call{};
    call.indexCount = 0;
    call.instanceCount = 1;
    expectTrue(!list.push(call), "push rejects indexCount 0");
    expectTrue(list.count() == 0u, "rejected push does not grow the list");
}

void testPushRejectsZeroInstanceCount() {
    fuse::renderer::DrawList list;
    fuse::renderer::DrawCall call{};
    call.indexCount = 3;
    call.instanceCount = 0;
    expectTrue(!list.push(call), "push rejects instanceCount 0");
    expectTrue(list.count() == 0u, "rejected instanceCount push does not grow the list");
}

void testPushTwoDraws() {
    fuse::renderer::DrawList list;

    fuse::renderer::DrawCall first{};
    first.vertexBuffer = fuse::renderer::BufferHandle(1u, 1u);
    first.indexBuffer = fuse::renderer::BufferHandle(2u, 1u);
    first.indexCount = 36;
    first.firstIndex = 0;
    first.vertexOffset = 0;
    first.instanceCount = 1;
    first.materialId = 7;
    expectTrue(list.push(first), "first draw accepted");

    fuse::renderer::DrawCall second{};
    second.vertexBuffer = fuse::renderer::BufferHandle(3u, 1u);
    second.indexBuffer = fuse::renderer::BufferHandle(4u, 1u);
    second.indexCount = 12;
    second.firstIndex = 36;
    second.vertexOffset = 12;
    second.instanceCount = 2;
    second.materialId = 9;
    expectTrue(list.push(second), "second draw accepted");

    expectTrue(list.count() == 2u, "two draws stored");
    expectTrue(list.data() != nullptr, "data is non-null after push");
    expectTrue(list.totalIndexCount() == 60u, "totalIndexCount is 36*1 + 12*2");
    expectTrue(list.at(0).materialId == 7u, "at(0) matches first materialId");
    expectTrue(list.at(1).indexCount == 12u, "at(1) matches second indexCount");
}

void testResetClears() {
    fuse::renderer::DrawList list;
    fuse::renderer::DrawCall call{};
    call.indexCount = 3;
    call.materialId = 4;
    expectTrue(list.push(call), "draw accepted before reset");

    list.reset();
    expectTrue(list.count() == 0u, "reset clears count");
    expectTrue(list.data() == nullptr, "reset data is nullptr");
    expectTrue(list.totalIndexCount() == 0u, "reset totalIndexCount is 0");
}

void testSortByMaterialIsStable() {
    fuse::renderer::DrawList list;

    fuse::renderer::DrawCall first{};
    first.indexCount = 10;
    first.materialId = 2;
    expectTrue(list.push(first), "first material 2 accepted");

    fuse::renderer::DrawCall second{};
    second.indexCount = 20;
    second.materialId = 1;
    expectTrue(list.push(second), "material 1 accepted");

    fuse::renderer::DrawCall third{};
    third.indexCount = 30;
    third.materialId = 2;
    expectTrue(list.push(third), "second material 2 accepted");

    list.sortByMaterial();

    expectTrue(list.at(0).materialId == 1u, "lowest materialId sorts first");
    expectTrue(list.at(0).indexCount == 20u, "material 1 keeps its indexCount");
    expectTrue(list.at(1).materialId == 2u, "first remaining call is material 2");
    expectTrue(list.at(2).materialId == 2u, "second remaining call is material 2");
    expectTrue(list.at(1).indexCount == 10u, "first-pushed material 2 stays before second");
    expectTrue(list.at(2).indexCount == 30u, "second-pushed material 2 stays last among ties");
}

void testRecordWithoutBeginReturnsZero() {
    fuse::renderer::DrawList list;
    fuse::renderer::DrawCall call{};
    call.indexCount = 6;
    call.materialId = 2;
    expectTrue(list.push(call), "draw accepted for unrecorded list");

    fuse::renderer::CommandBufferRecorder recorder;
    expectTrue(list.record(recorder) == 0u, "record without beginRecording returns 0");
    expectTrue(recorder.recordCount() == 0u, "no records when not recording");
}

void testRecordDrawIndexedPerCall() {
    fuse::renderer::DrawList list;

    fuse::renderer::DrawCall first{};
    first.indexCount = 10;
    first.materialId = 2;
    expectTrue(list.push(first), "record first call accepted");

    fuse::renderer::DrawCall second{};
    second.indexCount = 20;
    second.materialId = 1;
    expectTrue(list.push(second), "record second call accepted");

    fuse::renderer::DrawCall third{};
    third.indexCount = 30;
    third.materialId = 2;
    expectTrue(list.push(third), "record third call accepted");

    fuse::renderer::CommandBufferRecorder recorder;
    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds");
    expectTrue(list.record(recorder) == 3u, "record returns 3");
    expectTrue(recorder.recordCount() == 3u, "recorder has 3 records");
    expectTrue(recorder.records()[0].kind == fuse::renderer::CommandRecordKind::DrawIndexed,
               "first record is DrawIndexed");
    expectTrue(recorder.records()[1].kind == fuse::renderer::CommandRecordKind::DrawIndexed,
               "second record is DrawIndexed");
    expectTrue(recorder.records()[2].kind == fuse::renderer::CommandRecordKind::DrawIndexed,
               "third record is DrawIndexed");
}

} // namespace

int main() {
    testEmptyList();
    testPushRejectsZeroIndexCount();
    testPushRejectsZeroInstanceCount();
    testPushTwoDraws();
    testResetClears();
    testSortByMaterialIsStable();
    testRecordWithoutBeginReturnsZero();
    testRecordDrawIndexedPerCall();

    if (g_failures == 0) {
        std::printf("fuse_draw_list: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_draw_list: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
