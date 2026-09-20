#pragma once

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
/// No Vulkan submit; this is an unsorted per-frame list only.
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

private:
    std::vector<DrawCall> m_calls;
};

} // namespace fuse::renderer
