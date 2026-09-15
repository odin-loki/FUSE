#include <fuse/renderer/command_buffer.hpp>

namespace fuse::renderer {

void CommandBufferRecorder::reset() {
    m_nativeCommandBuffer = nullptr;
    m_recording = false;
    m_records.clear();
}

bool CommandBufferRecorder::beginRecording(void* nativeCommandBuffer) {
    if (m_recording) {
        return false;
    }

    m_nativeCommandBuffer = nativeCommandBuffer;
    m_recording = true;
    return true;
}

bool CommandBufferRecorder::endRecording() {
    if (!m_recording) {
        return false;
    }

    m_recording = false;
    return true;
}

void CommandBufferRecorder::push(CommandRecordKind kind) {
    CommandRecord record;
    record.kind = kind;
    m_records.push_back(record);
}

void CommandBufferRecorder::beginPass(const char* name) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::BeginPass;
    record.passName = name;
    m_records.push_back(record);
}

void CommandBufferRecorder::endPass() {
    if (!m_recording) {
        return;
    }
    push(CommandRecordKind::EndPass);
}

void CommandBufferRecorder::pipelineBarrier(u32 textureId, u32 fromLayout, u32 toLayout) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::PipelineBarrier;
    record.textureId = textureId;
    record.fromLayout = fromLayout;
    record.toLayout = toLayout;
    m_records.push_back(record);
}

void CommandBufferRecorder::clearColor(float r, float g, float b) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::ClearColor;
    record.clearR = r;
    record.clearG = g;
    record.clearB = b;
    m_records.push_back(record);
}

void CommandBufferRecorder::draw(u32 instanceCount) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::Draw;
    record.drawCount = instanceCount;
    m_records.push_back(record);
}

void CommandBufferRecorder::drawIndexed(u32 indexCount) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::DrawIndexed;
    record.drawCount = indexCount;
    m_records.push_back(record);
}

void CommandBufferRecorder::present() {
    if (!m_recording) {
        return;
    }
    push(CommandRecordKind::Present);
}

} // namespace fuse::renderer
