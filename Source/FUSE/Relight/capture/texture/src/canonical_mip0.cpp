// FUSE Relight RL-1.4: canonical mip-0 shadow (see canonical_mip0.hpp).
#include <fuse/relight/capture/texture/canonical_mip0.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::capture::texture {

CanonicalMip0::CanonicalMip0(hash::D3DFormat format, std::uint32_t width, std::uint32_t height,
                             const hash::FormatTableOptions& options)
    : m_info(hash::textureFormatInfo(format, options)),
      m_layout(hash::textureMip0Layout(format, width, height, 1, options)) {}

void CanonicalMip0::allocate() {
    if (m_bytes.empty() && m_layout.size != 0) {
        m_bytes.assign(std::size_t(m_layout.size), 0);
    }
}

void CanonicalMip0::release() {
    std::vector<std::uint8_t>().swap(m_bytes);
}

void CanonicalMip0::writeFull(const void* data, std::size_t rowPitch, std::uint64_t rows) {
    allocate();
    if (data == nullptr || m_bytes.empty()) {
        return;
    }
    const auto* src = static_cast<const std::uint8_t*>(data);
    const std::size_t rowBytes = std::size_t(m_layout.rowBytes);
    const std::size_t copy = std::min(rowPitch, rowBytes);
    const std::uint64_t count = std::min(rows, m_layout.rowCount);
    if (rowPitch == rowBytes) {
        std::memcpy(m_bytes.data(), src, std::size_t(count) * rowBytes);
        return;
    }
    for (std::uint64_t r = 0; r < count; ++r) {
        std::memcpy(m_bytes.data() + std::size_t(r) * rowBytes, src + std::size_t(r) * rowPitch, copy);
    }
}

bool CanonicalMip0::writeRect(const void* data, std::size_t rowPitch, const TexelRect& rect) {
    if (data == nullptr || rect.right <= rect.left || rect.bottom <= rect.top || m_info.elementSize == 0) {
        return false;
    }
    const std::uint32_t bw = std::max(1u, m_info.blockWidth);
    const std::uint32_t bh = std::max(1u, m_info.blockHeight);
    const std::uint32_t bx0 = rect.left / bw;
    const std::uint32_t by0 = rect.top / bh;
    const std::uint32_t bx1 = std::min(m_layout.blocksWide, (rect.right + bw - 1) / bw);
    const std::uint32_t by1 = std::min(m_layout.blocksHigh, (rect.bottom + bh - 1) / bh);
    if (bx0 >= bx1 || by0 >= by1) {
        return false;
    }
    allocate();
    const auto* src = static_cast<const std::uint8_t*>(data);
    const std::size_t rowBytes = std::size_t(m_layout.rowBytes);
    const std::size_t offset = std::size_t(bx0) * m_info.elementSize;
    const std::size_t bytes = std::min<std::size_t>(std::size_t(bx1 - bx0) * m_info.elementSize, rowPitch);
    for (std::uint32_t by = by0; by < by1; ++by) {
        std::memcpy(m_bytes.data() + std::size_t(by) * rowBytes + offset, src + std::size_t(by - by0) * rowPitch,
                    bytes);
    }
    return true;
}

hash::Hash64 CanonicalMip0::hash(bool obsolete) {
    allocate();
    return obsolete ? hash::hashTextureMip0Obsolete(m_bytes.data(), m_bytes.size())
                    : hash::hashTextureMip0(m_bytes.data(), m_bytes.size());
}

} // namespace fuse::relight::capture::texture
