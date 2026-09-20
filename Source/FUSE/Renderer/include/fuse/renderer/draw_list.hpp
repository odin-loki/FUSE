#pragma once

#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

struct DrawCall {
    BufferHandle vertexBuffer{};
    BufferHandle indexBuffer{};
    u32 indexCount = 0;
    u32 firstIndex = 0;
    u32 vertexOffset = 0;
    u32 instanceCount = 1;
    u32 materialId = 0;
};

/// CPU mesh draw submission container — RasterPath / G-buffer consume this later (B2.8).
/// No Vulkan submit; push-only until `sortByMaterial()`, then recorded into CommandBufferRecorder.
class DrawList {
public:
    void reset();
    /// Returns false if indexCount == 0 or instanceCount == 0.
    bool push(const DrawCall& call);
    u32 count() const;
    /// nullptr if empty.
    const DrawCall* data() const;
    /// Returns a static empty DrawCall if index is out of range.
    const DrawCall& at(u32 index) const;
    /// Sum of indexCount * instanceCount across all calls.
    u32 totalIndexCount() const;

    /// Stable sort by materialId (then original order).
    void sortByMaterial();

    /// For each call, recorder.drawIndexed(call.indexCount). Returns number of calls recorded.
    /// No-op (return 0) if recorder is not recording.
    u32 record(CommandBufferRecorder& recorder) const;

private:
    std::vector<DrawCall> m_calls;
};

} // namespace fuse::renderer
