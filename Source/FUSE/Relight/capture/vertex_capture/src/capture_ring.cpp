// FUSE Relight RL-1.6: capture region bookkeeping. See capture_ring.hpp.
#include <fuse/relight/capture/vertex_capture/capture_ring.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::capture::vertex_capture {

namespace {
std::size_t alignUp(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }
} // namespace

RegionRequest regionForDraw(const tap::DrawCall& call, std::uint32_t maxVertices) {
    RegionRequest r;
    switch (call.call) {
    case tap::DrawCallType::DrawPrimitive:
        r.baseVertex = std::int32_t(call.startVertex);
        r.vertexCount = call.vertexCount;
        break;
    case tap::DrawCallType::DrawPrimitiveUP:
        r.vertexCount = call.vertexCount;
        break;
    case tap::DrawCallType::DrawIndexedPrimitive:
        r.vertexOffset = call.baseVertex;
        r.baseVertex = call.baseVertex + std::int32_t(call.minIndex);
        r.vertexCount = call.numVertices;
        break;
    case tap::DrawCallType::DrawIndexedPrimitiveUP:
        r.baseVertex = std::int32_t(call.minIndex);
        r.vertexCount = call.numVertices;
        break;
    }
    r.vertexCount = std::min(r.vertexCount, maxVertices);
    return r;
}

CaptureRing::CaptureRing(std::size_t capacity) : m_capacity(std::max(capacity, kEmptyRegionBytes)) {}

std::optional<CaptureRing::Region> CaptureRing::allocate(std::uint64_t draw, const RegionRequest& request) {
    if (request.vertexCount == 0) {
        return std::nullopt;
    }
    const std::size_t size = regionBytes(request.vertexCount);
    // Demand: what a buffer holding every region of the frame needs.
    m_wanted = alignUp(std::max(m_wanted, kEmptyRegionBytes), kAlignment) + size;
    const std::size_t offset = alignUp(m_next, kAlignment);
    if (offset + size > m_capacity) {
        ++m_dropped;
        return std::nullopt;
    }
    Region region{offset, size, request, draw};
    m_next = offset + size;
    m_regions.push_back(region);
    return region;
}

void CaptureRing::writeEmptyRegion(void* mapped) {
    RegionHeader header;
    std::memcpy(mapped, &header, sizeof header);
}

void CaptureRing::initRegion(void* mapped, const Region& region) {
    auto* base = static_cast<std::uint8_t*>(mapped) + region.offset;
    RegionHeader header;
    header.baseVertex = region.request.baseVertex;
    header.vertexCount = region.request.vertexCount;
    header.drawLow = std::uint32_t(region.draw);
    header.drawHigh = std::uint32_t(region.draw >> 32);
    std::memcpy(base, &header, sizeof header);
    std::memset(base + kRegionHeaderSize, 0, region.size - kRegionHeaderSize);
}

void CaptureRing::reset(std::size_t newCapacity) {
    if (newCapacity != 0) {
        m_capacity = std::max(newCapacity, kEmptyRegionBytes);
    }
    m_next = kEmptyRegionBytes;
    m_wanted = 0;
    m_dropped = 0;
    m_regions.clear();
}

std::vector<tap::VertexCaptureDraw> CaptureRing::readBack(const void* mapped) const {
    std::vector<tap::VertexCaptureDraw> out;
    out.reserve(m_regions.size());
    for (const Region& r : m_regions) {
        tap::VertexCaptureDraw d;
        d.draw = r.draw;
        d.baseVertex = r.request.baseVertex;
        d.vertexOffset = r.request.vertexOffset;
        d.vertexCount = r.request.vertexCount;
        d.data = static_cast<const std::uint8_t*>(mapped) + r.offset + kRegionHeaderSize;
        out.push_back(d);
    }
    return out;
}

} // namespace fuse::relight::capture::vertex_capture
