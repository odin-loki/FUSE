// FUSE Relight RL-1.6: the GPU half of vertex capture inside the vendored DXVK d3d9.dll.
//
// Compiled into the d3d9 tap dispatcher (relight_tap_d3d9, DXVK's private headers as system
// headers, C++17). The dispatcher (Source/FUSE/Relight/tap/dxvk/fuse_tap_dxvk.cpp, a friend of
// D3D9DeviceEx) owns one FuseVertexCapture per device whose tap wants vertex capture and:
//   * at every programmable-VS draw DXVK will draw, binds sliceForDraw() at uniform-buffer slot
//     dxvk_hook::kResourceSlot (the binding FUSE-DXVK patch RL-1.6-01 added to every D3D9 vertex
//     shader): the draw's capture region, or the empty region when it gets none;
//   * at Present, when active(): waits for the GPU (D3D9DeviceEx::WaitForResource on buffer()), then
//     deliver() reports the regions to the tap (onVertexCapture) and recycles the buffer.
// While it exists, its tap is the process's SPIR-V substitutor (dxvk_hook::setSubstitutor).
#pragma once

#include <fuse/relight/capture/vertex_capture/capture_ring.hpp>
#include <fuse/relight/capture/vertex_capture/dxvk_hook.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <memory>

namespace dxvk {

class FuseVertexCapture {
public:
    /// Uniform-buffer slot of the capture binding (dxvk_hook.hpp).
    static constexpr uint32_t kResourceSlot = fuse::relight::capture::vertex_capture::dxvk_hook::kResourceSlot;

    /// Null when the device cannot store from vertex shaders (vertexPipelineStoresAndAtomics).
    /// Otherwise enables vertex capture for the process and registers `tap` as the substitutor.
    static std::unique_ptr<FuseVertexCapture> create(const Rc<DxvkDevice>& device, fuse::relight::tap::IRelightTap* tap);

    ~FuseVertexCapture();
    FuseVertexCapture(const FuseVertexCapture&) = delete;
    FuseVertexCapture& operator=(const FuseVertexCapture&) = delete;

    /// The slice to bind for a programmable-VS draw (`draw`: onDraw ordinal on the device).
    DxvkBufferSlice sliceForDraw(uint64_t draw, const fuse::relight::tap::DrawCall& call);

    /// Draws of this frame asked for regions (captured or dropped).
    bool active() const { return m_ring.pending() || m_ring.dropped() != 0; }
    /// Draws of this frame have regions (the GPU must finish before deliver()).
    bool pending() const { return m_ring.pending(); }
    const Rc<DxvkBuffer>& buffer() const { return m_buffer; }

    /// After the GPU finished the frame: onVertexCapture, then the next frame starts at offset 0
    /// (in a bigger buffer when this frame dropped draws).
    void deliver(fuse::relight::tap::IRelightTap& tap, uint64_t frame);

private:
    FuseVertexCapture(const Rc<DxvkDevice>& device, fuse::relight::tap::IRelightTap* tap);
    void allocateBuffer(size_t capacity);

    Rc<DxvkDevice> m_device;
    fuse::relight::tap::IRelightTap* m_tap = nullptr;
    Rc<DxvkBuffer> m_buffer;
    fuse::relight::capture::vertex_capture::CaptureRing m_ring;
};

} // namespace dxvk
