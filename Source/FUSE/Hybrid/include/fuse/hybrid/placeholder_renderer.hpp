#pragma once

#include <fuse/hybrid/mesh_sdf_preview_stub.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::hybrid {

/// Software RGBA framebuffer — honest placeholder until real GL/Vulkan RHI (Track B).
class PlaceholderRenderer {
public:
    void beginFrame(u32 width, u32 height);
    void clear3D(float r, float g, float b);
    void drawSprite2D(float x, float y, float rotation, u8 r, u8 g, u8 b);
    void drawMeshPreviewStub(float x, float y, float z, u8 r, u8 g, u8 b);
    void drawSdfPreviewStub(float x, float y, float z, SdfPreviewPrimitive primitive, float param0,
                            float param1, float param2, u8 r, u8 g, u8 b);

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const u8* pixels() const { return m_pixels.data(); }
    u32 pixelCount() const { return static_cast<u32>(m_pixels.size()); }

    u8 sample(u32 x, u32 y) const;

private:
    void setPixel(u32 x, u32 y, u8 r, u8 g, u8 b, u8 a);

    u32 m_width = 0;
    u32 m_height = 0;
    std::vector<u8> m_pixels;
};

} // namespace fuse::hybrid
