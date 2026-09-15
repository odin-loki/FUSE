#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

enum class CommandRecordKind : u8 {
    BeginPass = 1,
    EndPass = 2,
    PipelineBarrier = 3,
    ClearColor = 4,
    DrawIndexed = 5,
    Draw = 6,
    Present = 7,
    Composite = 8,
};

struct CommandRecord {
    CommandRecordKind kind = CommandRecordKind::BeginPass;
    const char* passName = nullptr;
    float clearR = 0.f;
    float clearG = 0.f;
    float clearB = 0.f;
    float compositeBlend = 0.f;
    u32 textureId = 0;
    u32 fromLayout = 0;
    u32 toLayout = 0;
    u32 drawCount = 0;
};

/// Stub command-buffer recorder — records logical commands for tests and future vkCmd* wiring.
class CommandBufferRecorder {
public:
    void reset();
    bool beginRecording(void* nativeCommandBuffer);
    bool endRecording();

    void beginPass(const char* name);
    void endPass();
    void pipelineBarrier(u32 textureId, u32 fromLayout, u32 toLayout);
    void clearColor(float r, float g, float b);
    void draw(u32 instanceCount);
    void drawIndexed(u32 indexCount);
    void composite(float blend);
    void present();

    bool isRecording() const { return m_recording; }
    void* nativeHandle() const { return m_nativeCommandBuffer; }
    const std::vector<CommandRecord>& records() const { return m_records; }
    u32 recordCount() const { return static_cast<u32>(m_records.size()); }

private:
    void push(CommandRecordKind kind);

    void* m_nativeCommandBuffer = nullptr;
    bool m_recording = false;
    std::vector<CommandRecord> m_records;
};

} // namespace fuse::renderer
