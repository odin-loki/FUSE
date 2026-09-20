#include <fuse/renderer/draw_list.hpp>
#include <fuse/renderer/resource_manager.hpp>

#include <cstdint>
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

void testRecordEncodesDrawParams() {
    fuse::renderer::DrawList list;

    fuse::renderer::DrawCall first{};
    first.indexCount = 36;
    first.firstIndex = 0;
    first.vertexOffset = 0;
    first.instanceCount = 1;
    first.materialId = 7;
    expectTrue(list.push(first), "param encode first call accepted");

    fuse::renderer::DrawCall second{};
    second.indexCount = 12;
    second.firstIndex = 36;
    second.vertexOffset = 12;
    second.instanceCount = 2;
    second.materialId = 9;
    expectTrue(list.push(second), "param encode second call accepted");

    fuse::renderer::CommandBufferRecorder recorder;
    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds for param encode");
    expectTrue(list.record(recorder) == 2u, "param encode record returns 2");
    expectTrue(recorder.recordCount() == 2u, "param encode recorder has 2 records");

    const fuse::renderer::CommandRecord& a = recorder.records()[0];
    expectTrue(a.kind == fuse::renderer::CommandRecordKind::DrawIndexed, "first param record is DrawIndexed");
    expectTrue(a.indexCount == 36u, "first indexCount encoded");
    expectTrue(a.instanceCount == 1u, "first instanceCount encoded");
    expectTrue(a.firstIndex == 0u, "first firstIndex encoded");
    expectTrue(a.vertexOffset == 0, "first vertexOffset encoded");
    expectTrue(a.materialId == 7u, "first materialId encoded");

    const fuse::renderer::CommandRecord& b = recorder.records()[1];
    expectTrue(b.kind == fuse::renderer::CommandRecordKind::DrawIndexed, "second param record is DrawIndexed");
    expectTrue(b.indexCount == 12u, "second indexCount encoded");
    expectTrue(b.instanceCount == 2u, "second instanceCount encoded");
    expectTrue(b.firstIndex == 36u, "second firstIndex encoded");
    expectTrue(b.vertexOffset == 12, "second vertexOffset encoded");
    expectTrue(b.materialId == 9u, "second materialId encoded");
}

void testDispatchRecordsWithoutDevice() {
    fuse::renderer::CommandBufferRecorder recorder;
    recorder.dispatch(4u, 2u, 1u);
    expectTrue(recorder.recordCount() == 0u, "dispatch without beginRecording is a no-op");

    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds without device");
    recorder.dispatch(8u, 4u, 2u);
    expectTrue(recorder.recordCount() == 1u, "logical dispatch records without device");
    expectTrue(recorder.records()[0].kind == fuse::renderer::CommandRecordKind::Dispatch,
               "record kind is Dispatch");
    expectTrue(recorder.records()[0].dispatchX == 8u, "dispatchX recorded");
    expectTrue(recorder.records()[0].dispatchY == 4u, "dispatchY recorded");
    expectTrue(recorder.records()[0].dispatchZ == 2u, "dispatchZ recorded");
    expectTrue(recorder.vulkanDispatchCount() == 0u, "no GPU dispatch without encode context");
    expectTrue(recorder.endRecording(), "endRecording succeeds after logical dispatch");
}

void testClearDepthAndFillBufferRecordsWithoutDevice() {
    fuse::renderer::CommandBufferRecorder recorder;
    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds for clearDepth/fillBuffer");
    recorder.clearDepth(0.5f);
    recorder.fillBuffer(1u, 0xFFFFFFFFu);

    expectTrue(recorder.recordCount() == 2u, "clearDepth and fillBuffer each record one command");
    expectTrue(recorder.records()[0].kind == fuse::renderer::CommandRecordKind::ClearDepth,
               "first record is ClearDepth");
    expectTrue(recorder.records()[0].clearDepth == 0.5f, "clearDepth stores 0.5");
    expectTrue(recorder.records()[1].kind == fuse::renderer::CommandRecordKind::FillBuffer,
               "second record is FillBuffer");
    expectTrue(recorder.records()[1].bufferId == 1u, "fillBuffer stores bufferId 1");
    expectTrue(recorder.records()[1].fillValue == 0xFFFFFFFFu, "fillBuffer stores fill value");
    expectTrue(recorder.vulkanFillBufferCount() == 0u, "logical-only fillBuffer does not encode vkCmdFillBuffer");
    expectTrue(recorder.endRecording(), "endRecording succeeds after logical clearDepth/fillBuffer");
}

void testDrawIndexedStoresNativeBuffers() {
    fuse::renderer::CommandBufferRecorder recorder;
    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds for native buffers");

    void* vertex = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000));
    void* index = reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000));
    recorder.drawIndexed(12u, 1u, 0u, 0, 3u, vertex, index);

    expectTrue(recorder.recordCount() == 1u, "drawIndexed records one command");
    expectTrue(recorder.records()[0].nativeVertexBuffer == vertex, "native vertex buffer stored");
    expectTrue(recorder.records()[0].nativeIndexBuffer == index, "native index buffer stored");
}

void testRecordWithNullResourcesLeavesNativeNull() {
    fuse::renderer::DrawList list;
    fuse::renderer::DrawCall call{};
    call.vertexBuffer = fuse::renderer::BufferHandle(1u, 1u);
    call.indexBuffer = fuse::renderer::BufferHandle(2u, 1u);
    call.indexCount = 6;
    expectTrue(list.push(call), "call with handles accepted");

    fuse::renderer::CommandBufferRecorder recorder;
    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds for null resources");
    expectTrue(list.record(recorder) == 1u, "record(recorder) records one call");
    expectTrue(recorder.records()[0].nativeVertexBuffer == nullptr,
               "null resources leave native vertex default");
    expectTrue(recorder.records()[0].nativeIndexBuffer == nullptr,
               "null resources leave native index default");
}

void testRecordWithInvalidResourceHandlesLeavesNativeNull() {
    fuse::renderer::DrawList list;
    fuse::renderer::DrawCall call{};
    call.vertexBuffer = fuse::renderer::BufferHandle(1u, 1u);
    call.indexBuffer = fuse::renderer::BufferHandle(2u, 1u);
    call.indexCount = 6;
    expectTrue(list.push(call), "call with stale handles accepted");

    fuse::renderer::ResourceManager resources;
    fuse::renderer::CommandBufferRecorder recorder;
    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds for invalid handles");
    expectTrue(list.record(recorder, &resources) == 1u, "record with resources records one call");
    expectTrue(recorder.records()[0].nativeVertexBuffer == nullptr,
               "invalid vertex handle uses context default");
    expectTrue(recorder.records()[0].nativeIndexBuffer == nullptr,
               "invalid index handle uses context default");
}

void testDrawIndexedIndirectRecordsWithoutDevice() {
    fuse::renderer::CommandBufferRecorder recorder;
    recorder.drawIndexedIndirect(nullptr);
    expectTrue(recorder.recordCount() == 0u, "drawIndexedIndirect without beginRecording is a no-op");

    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds for drawIndexedIndirect");
    void* indirect = reinterpret_cast<void*>(static_cast<uintptr_t>(0x3000));
    recorder.drawIndexedIndirect(indirect, 40u, 2u, 20u);
    recorder.drawIndexedIndirect(nullptr);

    expectTrue(recorder.recordCount() == 2u, "logical drawIndexedIndirect records with null or fake handles");
    expectTrue(recorder.records()[0].kind == fuse::renderer::CommandRecordKind::DrawIndexedIndirect,
               "first record is DrawIndexedIndirect");
    expectTrue(recorder.records()[0].nativeIndirectBuffer == indirect, "indirect buffer stored");
    expectTrue(recorder.records()[0].bufferOffset == 40u, "indirect offset stored");
    expectTrue(recorder.records()[0].drawCount == 2u, "indirect drawCount stored");
    expectTrue(recorder.records()[0].stride == 20u, "indirect stride stored");
    expectTrue(recorder.records()[1].kind == fuse::renderer::CommandRecordKind::DrawIndexedIndirect,
               "null-handle indirect still records");
    expectTrue(recorder.records()[1].nativeIndirectBuffer == nullptr, "null indirect handle stored");
    expectTrue(recorder.vulkanDrawIndexedIndirectCount() == 0u,
               "logical-only drawIndexedIndirect does not encode vkCmdDrawIndexedIndirect");
    expectTrue(recorder.endRecording(), "endRecording succeeds after logical drawIndexedIndirect");
}

void testUpdateBufferRecordsWithoutDevice() {
    fuse::renderer::CommandBufferRecorder recorder;
    recorder.updateBuffer(nullptr, 1u);
    expectTrue(recorder.recordCount() == 0u, "updateBuffer without beginRecording is a no-op");

    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds for updateBuffer");
    void* dst = reinterpret_cast<void*>(static_cast<uintptr_t>(0x4000));
    recorder.updateBuffer(dst, 0xABCDu);
    recorder.updateBuffer(nullptr, 7u);

    expectTrue(recorder.recordCount() == 2u, "logical updateBuffer records with null or fake handles");
    expectTrue(recorder.records()[0].kind == fuse::renderer::CommandRecordKind::UpdateBuffer,
               "first record is UpdateBuffer");
    expectTrue(recorder.records()[0].nativeDstBuffer == dst, "updateBuffer dst stored");
    expectTrue(recorder.records()[0].fillValue == 0xABCDu, "updateBuffer data stored");
    expectTrue(recorder.records()[1].kind == fuse::renderer::CommandRecordKind::UpdateBuffer,
               "null-handle updateBuffer still records");
    expectTrue(recorder.records()[1].nativeDstBuffer == nullptr, "null dst stored");
    expectTrue(recorder.records()[1].fillValue == 7u, "null-handle data stored");
    expectTrue(recorder.vulkanUpdateBufferCount() == 0u,
               "logical-only updateBuffer does not encode vkCmdUpdateBuffer");
    expectTrue(recorder.endRecording(), "endRecording succeeds after logical updateBuffer");
}

void testCopyBufferRecordsWithoutDevice() {
    fuse::renderer::CommandBufferRecorder recorder;
    recorder.copyBuffer(nullptr, nullptr, 16u);
    expectTrue(recorder.recordCount() == 0u, "copyBuffer without beginRecording is a no-op");

    expectTrue(recorder.beginRecording(nullptr), "beginRecording succeeds for copyBuffer");
    void* src = reinterpret_cast<void*>(static_cast<uintptr_t>(0x5000));
    void* dst = reinterpret_cast<void*>(static_cast<uintptr_t>(0x6000));
    recorder.copyBuffer(src, dst, 64u);
    recorder.copyBuffer(nullptr, nullptr, 8u);

    expectTrue(recorder.recordCount() == 2u, "logical copyBuffer records with null or fake handles");
    expectTrue(recorder.records()[0].kind == fuse::renderer::CommandRecordKind::CopyBuffer,
               "first record is CopyBuffer");
    expectTrue(recorder.records()[0].nativeSrcBuffer == src, "copyBuffer src stored");
    expectTrue(recorder.records()[0].nativeDstBuffer == dst, "copyBuffer dst stored");
    expectTrue(recorder.records()[0].copySize == 64u, "copyBuffer size stored");
    expectTrue(recorder.records()[1].kind == fuse::renderer::CommandRecordKind::CopyBuffer,
               "null-handle copyBuffer still records");
    expectTrue(recorder.records()[1].nativeSrcBuffer == nullptr, "null src stored");
    expectTrue(recorder.records()[1].nativeDstBuffer == nullptr, "null dst stored");
    expectTrue(recorder.vulkanCopyBufferCount() == 0u,
               "logical-only copyBuffer does not encode vkCmdCopyBuffer");
    expectTrue(recorder.endRecording(), "endRecording succeeds after logical copyBuffer");
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
    testRecordEncodesDrawParams();
    testDispatchRecordsWithoutDevice();
    testClearDepthAndFillBufferRecordsWithoutDevice();
    testDrawIndexedStoresNativeBuffers();
    testRecordWithNullResourcesLeavesNativeNull();
    testRecordWithInvalidResourceHandlesLeavesNativeNull();
    testDrawIndexedIndirectRecordsWithoutDevice();
    testUpdateBufferRecordsWithoutDevice();
    testCopyBufferRecordsWithoutDevice();

    if (g_failures == 0) {
        std::printf("fuse_draw_list: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_draw_list: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
