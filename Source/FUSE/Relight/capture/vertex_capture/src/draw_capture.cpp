// FUSE Relight RL-1.6: one draw's captured vertices on the CPU side. See draw_capture.hpp.
//
// The rtx.* options below are owned by vertex capture (names, defaults and descriptions as
// dxvk-remix @0867d3c src/d3d9/d3d9_rtx.h); each also answers to its relight.* twin (RL-0.6).
#include <fuse/relight/capture/vertex_capture/draw_capture.hpp>

#include <fuse/relight/options/options.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::capture::vertex_capture {

namespace {

struct VertexCaptureRtxOptions {
    FUSE_RELIGHT_OPTION("rtx", bool, useVertexCapturedNormals, true,
                        "When enabled, vertex normals are read from the input assembler and used in raytracing.  This "
                        "doesn't always work as normals can be in any coordinate space, but can help sometimes.");
    FUSE_RELIGHT_OPTION("rtx", bool, useVertexCapturedTexcoords, false,
                        "When enabled, vertex shader output texcoords always override input texcoords from the vertex "
                        "declaration. Enable for games where the vertex shader applies meaningful UV transformations "
                        "that should be used for ray tracing (e.g. animated UVs via shader constants).");
    FUSE_RELIGHT_OPTION("rtx", bool, useWorldMatricesForShaders, true,
                        "When enabled, Remix will utilize the world matrices being passed from the game via D3D9 fixed "
                        "function API, even when running with shaders.  Sometimes games pass these matrices and they are "
                        "useful, however for some games they are very unreliable, and should be filtered out.  If you're "
                        "seeing precision related issues with shader vertex capture, try disabling this setting.");
};

} // namespace

VertexCaptureOptions VertexCaptureOptions::fromOptions() {
    VertexCaptureOptions o;
    o.useWorldMatricesForShaders = VertexCaptureRtxOptions::useWorldMatricesForShaders();
    o.useVertexCapturedNormals = VertexCaptureRtxOptions::useVertexCapturedNormals();
    o.useVertexCapturedTexcoords = VertexCaptureRtxOptions::useVertexCapturedTexcoords();
    return o;
}

DrawVertexCapture beginDrawCapture(const tap::DrawState& state, const VertexCaptureOptions& options) {
    DrawVertexCapture c;
    const float* world = state.transforms ? state.transforms[tap::kTransformWorld0] : nullptr;
    const float* view = state.transforms ? state.transforms[tap::kTransformView] : nullptr;
    const float* projection = state.transforms ? state.transforms[tap::kTransformProjection] : nullptr;
    // processRenderState with UseProgrammableVS() and useVertexCapture() both true.
    c.backTransform = backTransformFor(drawTransforms(world, view, projection, options.useWorldMatricesForShaders));
    return c;
}

void completeDrawCapture(DrawVertexCapture& c, const tap::VertexCaptureDraw& region) {
    c.captured = true;
    c.baseVertex = region.baseVertex;
    c.vertexOffset = region.vertexOffset;
    c.raw.resize(region.vertexCount);
    if (region.vertexCount != 0 && region.data) {
        std::memcpy(c.raw.data(), region.data, std::size_t(region.vertexCount) * kCapturedVertexSize);
    }
    c.vertices.assign(region.vertexCount, CapturedVertex{});
    c.written = 0;
    c.fields = 0;
    for (std::size_t k = 0; k < c.raw.size(); ++k) {
        const RawCapturedVertex& raw = c.raw[k];
        if ((raw.fields & fields::kWritten) == 0) {
            c.vertices[k].color0 = 0;
            continue;
        }
        ++c.written;
        c.fields |= raw.fields;
        c.vertices[k] = backTransform(c.backTransform, raw);
    }
}

} // namespace fuse::relight::capture::vertex_capture
