// FUSE Relight RL-1.6: the GPU half of vertex capture inside d3d9.dll. See fuse_vertex_capture_dxvk.h.
#include <d3d9_device.h>

#include "fuse_vertex_capture_dxvk.h"

#include <fuse/relight/capture/vertex_capture/dxvk_hook.hpp>

#include <algorithm>
#include <cstdio>

namespace dxvk {

namespace vc = fuse::relight::capture::vertex_capture;
namespace rt = fuse::relight::tap;

namespace {

constexpr size_t kInitialCapacity = size_t(4) << 20;  // 4 MiB: ~87k vertices per frame
constexpr size_t kMaxCapacity = size_t(256) << 20;

bool substitute(void* owner, const rt::ShaderModule& module, std::vector<uint32_t>& out) {
    return static_cast<rt::IRelightTap*>(owner)->substituteVertexShader(module, out);
}

} // namespace

std::unique_ptr<FuseVertexCapture> FuseVertexCapture::create(const Rc<DxvkDevice>& device, rt::IRelightTap* tap) {
    if (!device->features().core.features.vertexPipelineStoresAndAtomics) {
        std::fprintf(stderr, "fuse-relight: vertex capture off (the device has no vertexPipelineStoresAndAtomics)\n");
        return nullptr;
    }
    return std::unique_ptr<FuseVertexCapture>(new FuseVertexCapture(device, tap));
}

FuseVertexCapture::FuseVertexCapture(const Rc<DxvkDevice>& device, rt::IRelightTap* tap)
    : m_device(device), m_tap(tap), m_ring(kInitialCapacity) {
    allocateBuffer(kInitialCapacity);
    vc::dxvk_hook::enable();
    vc::dxvk_hook::setSubstitutor(m_tap, &substitute);
}

FuseVertexCapture::~FuseVertexCapture() {
    // Waits for an offer in flight on a compile thread, so the tap can go away after this.
    vc::dxvk_hook::clearSubstitutor(m_tap);
}

void FuseVertexCapture::allocateBuffer(size_t capacity) {
    DxvkBufferCreateInfo info;
    info.size = capacity;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    info.access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    info.debugName = "FUSE Relight vertex capture";
    m_buffer = m_device->createBuffer(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vc::CaptureRing::writeEmptyRegion(m_buffer->mapPtr(0));
}

DxvkBufferSlice FuseVertexCapture::sliceForDraw(uint64_t draw, const rt::DrawCall& call) {
    const vc::RegionRequest request = vc::regionForDraw(call);
    if (const std::optional<vc::CaptureRing::Region> region = m_ring.allocate(draw, request)) {
        vc::CaptureRing::initRegion(m_buffer->mapPtr(0), *region);
        return DxvkBufferSlice(m_buffer, region->offset, region->size);
    }
    return DxvkBufferSlice(m_buffer, 0, vc::CaptureRing::kEmptyRegionBytes);
}

void FuseVertexCapture::deliver(rt::IRelightTap& tap, uint64_t frame) {
    const std::vector<rt::VertexCaptureDraw> draws = m_ring.readBack(m_buffer->mapPtr(0));
    rt::VertexCaptureFrame f;
    f.frame = frame;
    f.draws = draws.empty() ? nullptr : draws.data();
    f.drawCount = uint32_t(draws.size());
    f.dropped = m_ring.dropped();
    tap.onVertexCapture(f);

    const size_t wanted = m_ring.wantedCapacity();
    if (wanted > m_ring.capacity() && m_ring.capacity() < kMaxCapacity) {
        size_t capacity = m_ring.capacity();
        while (capacity < wanted && capacity < kMaxCapacity) {
            capacity *= 2;
        }
        // The old buffer stays alive while DXVK still tracks it for in-flight draws.
        allocateBuffer(capacity);
        m_ring.reset(capacity);
    } else {
        m_ring.reset();
    }
}

} // namespace dxvk
