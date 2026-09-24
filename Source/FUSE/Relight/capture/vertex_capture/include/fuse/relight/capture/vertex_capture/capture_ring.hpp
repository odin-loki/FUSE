// FUSE Relight RL-1.6: capture region bookkeeping for one device (the CPU half of the dispatcher's
// capture buffer; no Vulkan). See capture_layout.hpp for the region format.
//
// The dispatcher owns one host-visible buffer per device. Bytes [0, kEmptyRegionBytes) hold the
// empty region (vertexCount 0), bound for programmable-VS draws that get no region. Each captured
// draw of a frame gets the next aligned region; at the frame's Present the dispatcher waits for the
// GPU, reads the regions and calls reset(). When a frame asked for more than the buffer holds, the
// draws that did not fit are dropped and wantedCapacity() tells the dispatcher how big the next
// buffer should be.
#pragma once

#include <fuse/relight/capture/vertex_capture/capture_layout.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace fuse::relight::capture::vertex_capture {

/// Where a draw's slot 0 sits in gl_VertexIndex terms, and how many slots it needs.
struct RegionRequest {
    std::int32_t baseVertex = 0;   ///< gl_VertexIndex of slot 0
    std::int32_t vertexOffset = 0; ///< the draw's vertexOffset (index value = gl_VertexIndex - vertexOffset)
    std::uint32_t vertexCount = 0;
};

/// The region a D3D9 draw needs, from how DXVK issues it:
///   DrawPrimitive          draw(firstVertex = StartVertex):      base StartVertex, count vertexCount
///   DrawPrimitiveUP        draw(firstVertex = 0):                base 0, count vertexCount
///   DrawIndexedPrimitive   drawIndexed(vertexOffset = BaseVertexIndex): base BaseVertexIndex + MinVertexIndex,
///                                                                  count NumVertices
///   DrawIndexedPrimitiveUP drawIndexed(vertexOffset = 0):         base MinVertexIndex, count NumVertices
/// (Remix: baseVertex = BaseVertexIndex + minIndex, vertexCount = maxIndex - minIndex + 1 from the
/// scanned indices; D3D9's MinVertexIndex / NumVertices bound those.) Counts above maxVertices are
/// clamped.
[[nodiscard]] RegionRequest regionForDraw(const tap::DrawCall& call, std::uint32_t maxVertices = 1u << 20);

class CaptureRing {
public:
    static constexpr std::size_t kAlignment = 256; ///< >= every minStorageBufferOffsetAlignment
    static constexpr std::size_t kEmptyRegionBytes = kAlignment;

    explicit CaptureRing(std::size_t capacity);

    struct Region {
        std::size_t offset = 0; ///< bytes from the buffer start (aligned)
        std::size_t size = 0;   ///< regionBytes(vertexCount)
        RegionRequest request;
        std::uint64_t draw = 0;
    };

    /// Reserves the draw's region; nullopt (and the draw counted as dropped) when it does not fit.
    std::optional<Region> allocate(std::uint64_t draw, const RegionRequest& request);

    /// Writes the empty region's header at `mapped` (the buffer start).
    static void writeEmptyRegion(void* mapped);
    /// Writes a region's header and clears its slots (fields = 0 marks "not written").
    static void initRegion(void* mapped, const Region& region);

    /// The frame's regions, in draw order.
    [[nodiscard]] const std::vector<Region>& regions() const { return m_regions; }
    [[nodiscard]] std::uint32_t dropped() const { return m_dropped; }
    [[nodiscard]] bool pending() const { return !m_regions.empty(); }
    [[nodiscard]] std::size_t capacity() const { return m_capacity; }
    /// > capacity() when this frame dropped draws: the size that would have held them all.
    [[nodiscard]] std::size_t wantedCapacity() const { return m_wanted; }

    /// Next frame (the GPU is done with every region). A new capacity applies from now on.
    void reset(std::size_t newCapacity = 0);

    /// Builds the tap event entries from the mapped buffer (valid while the buffer is mapped).
    [[nodiscard]] std::vector<tap::VertexCaptureDraw> readBack(const void* mapped) const;

private:
    std::size_t m_capacity = 0;
    std::size_t m_next = kEmptyRegionBytes;
    std::size_t m_wanted = 0;
    std::uint32_t m_dropped = 0;
    std::vector<Region> m_regions;
};

} // namespace fuse::relight::capture::vertex_capture
