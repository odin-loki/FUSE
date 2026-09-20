#include <fuse/hybrid/placeholder_renderer.hpp>

#include <cmath>
#include <cstring>

namespace fuse::hybrid {

void PlaceholderRenderer::beginFrame(u32 width, u32 height) {
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;
    m_pixels.assign(static_cast<size_t>(m_width) * m_height * 4u, 0u);
}

void PlaceholderRenderer::setPixel(u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) {
    if (x >= m_width || y >= m_height) {
        return;
    }
    const size_t offset = (static_cast<size_t>(y) * m_width + x) * 4u;
    m_pixels[offset + 0] = r;
    m_pixels[offset + 1] = g;
    m_pixels[offset + 2] = b;
    m_pixels[offset + 3] = a;
}

void PlaceholderRenderer::clear3D(float r, float g, float b) {
    const u8 cr = static_cast<u8>(r * 255.f);
    const u8 cg = static_cast<u8>(g * 255.f);
    const u8 cb = static_cast<u8>(b * 255.f);

    for (u32 y = 0; y < m_height; ++y) {
        for (u32 x = 0; x < m_width; ++x) {
            setPixel(x, y, cr, cg, cb, 255);
        }
    }
}

void PlaceholderRenderer::drawMeshPreviewStub(float x, float y, float z, u8 r, u8 g, u8 b) {
    const float cx = x + static_cast<float>(m_width) * 0.5f;
    const float cy = y - z * 0.25f + static_cast<float>(m_height) * 0.5f;
    const float size = 10.f;

    for (int dy = -10; dy <= 10; ++dy) {
        for (int dx = -10; dx <= 10; ++dx) {
            if (std::abs(dx) == 10 || std::abs(dy) == 10 || std::abs(dx + dy) <= 2 || std::abs(dx - dy) <= 2) {
                const u32 px = static_cast<u32>(cx + static_cast<float>(dx));
                const u32 py = static_cast<u32>(cy + static_cast<float>(dy));
                if (px < m_width && py < m_height) {
                    setPixel(px, py, r, g, b, 255);
                }
            }
        }
    }
}

void PlaceholderRenderer::drawSdfPreviewStub(float x, float y, float z, bool boxPrimitive, u8 r, u8 g,
                                             u8 b) {
    const float cx = x + static_cast<float>(m_width) * 0.5f;
    const float cy = y - z * 0.25f + static_cast<float>(m_height) * 0.5f;
    const int radius = boxPrimitive ? 0 : 8;

    for (int dy = -10; dy <= 10; ++dy) {
        for (int dx = -10; dx <= 10; ++dx) {
            bool inside = false;
            if (boxPrimitive) {
                inside = std::abs(dx) <= 8 && std::abs(dy) <= 8;
            } else {
                inside = (dx * dx + dy * dy) <= (radius * radius + radius);
            }

            if (inside) {
                const u32 px = static_cast<u32>(cx + static_cast<float>(dx));
                const u32 py = static_cast<u32>(cy + static_cast<float>(dy));
                if (px < m_width && py < m_height) {
                    setPixel(px, py, r, g, b, 220);
                }
            }
        }
    }
}

void PlaceholderRenderer::drawSprite2D(float x, float y, float rotation, u8 r, u8 g, u8 b) {
    const float cx = x + static_cast<float>(m_width) * 0.5f;
    const float cy = y + static_cast<float>(m_height) * 0.5f;
    const float size = 12.f;
    const float cosR = std::cos(rotation);
    const float sinR = std::sin(rotation);

    for (int dy = -12; dy <= 12; ++dy) {
        for (int dx = -12; dx <= 12; ++dx) {
            const float rx = static_cast<float>(dx) * cosR - static_cast<float>(dy) * sinR;
            const float ry = static_cast<float>(dx) * sinR + static_cast<float>(dy) * cosR;
            const u32 px = static_cast<u32>(cx + rx);
            const u32 py = static_cast<u32>(cy + ry);
            if (px < m_width && py < m_height) {
                setPixel(px, py, r, g, b, 255);
            }
        }
    }
}

u8 PlaceholderRenderer::sample(u32 x, u32 y) const {
    if (x >= m_width || y >= m_height || m_pixels.empty()) {
        return 0;
    }
    const size_t offset = (static_cast<size_t>(y) * m_width + x) * 4u;
    return m_pixels[offset];
}

} // namespace fuse::hybrid
