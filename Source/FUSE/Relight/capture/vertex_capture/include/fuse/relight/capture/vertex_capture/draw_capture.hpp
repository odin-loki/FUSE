// FUSE Relight RL-1.6: one draw's captured vertices on the CPU side (the tap consumer's half).
//
// At onDraw a consumer calls beginDrawCapture() with the draw's state: it keeps the back-transform
// of that moment (processRenderState + prepareVertexCapture, back_transform.hpp). When the
// dispatcher delivers the draw's region (IRelightTap::onVertexCapture), completeDrawCapture()
// copies the raw slots and back-transforms every written one to Remix's CapturedVertex.
#pragma once

#include <fuse/relight/capture/vertex_capture/back_transform.hpp>
#include <fuse/relight/capture/vertex_capture/capture_layout.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <cstdint>
#include <vector>

namespace fuse::relight::capture::vertex_capture {

/// rtx.* options of vertex capture (d3d9_rtx.h names and defaults; also answer to relight.*).
struct VertexCaptureOptions {
    bool useWorldMatricesForShaders = true; ///< rtx.useWorldMatricesForShaders
    bool useVertexCapturedNormals = true;   ///< rtx.useVertexCapturedNormals
    bool useVertexCapturedTexcoords = false; ///< rtx.useVertexCapturedTexcoords

    [[nodiscard]] static VertexCaptureOptions fromOptions();
};

struct DrawVertexCapture {
    BackTransform backTransform;   ///< of the draw's transforms at onDraw
    bool captured = false;         ///< the region came back
    std::int32_t baseVertex = 0;   ///< gl_VertexIndex of slot 0
    std::int32_t vertexOffset = 0; ///< index value of slot k = baseVertex + k - vertexOffset
    std::uint32_t written = 0;     ///< slots the shader wrote
    std::uint32_t fields = 0;      ///< fields::* of the written slots (the shader writes the same set everywhere)
    std::vector<RawCapturedVertex> raw;    ///< every slot as the GPU left it
    std::vector<CapturedVertex> vertices;  ///< back-transformed (written slots; others zero with fields 0)
};

/// processRenderState for a programmable-VS draw with vertex capture on: D3DTS_WORLD (or identity
/// without rtx.useWorldMatricesForShaders), D3DTS_VIEW, D3DTS_PROJECTION of `state`.
[[nodiscard]] DrawVertexCapture beginDrawCapture(const tap::DrawState& state, const VertexCaptureOptions& options);

/// Copies and back-transforms the delivered region.
void completeDrawCapture(DrawVertexCapture& capture, const tap::VertexCaptureDraw& region);

} // namespace fuse::relight::capture::vertex_capture
