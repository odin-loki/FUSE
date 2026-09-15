#include <fuse/renderer/taa/taa_resolve.hpp>

namespace fuse::renderer {

bool TaaResolve::resolve(const TaaResolveDesc& desc, TaaHistoryBuffer& history, void* /*cudaStream*/) {
    m_stats = {};
    m_message.clear();

    if (!history.isReady()) {
        m_message = "TAA resolve skipped — history buffer not ready";
        return false;
    }

    if (desc.width == 0u || desc.height == 0u) {
        m_message = "TAA resolve skipped — invalid dimensions";
        return false;
    }

    if (desc.surfaces.current_frame == nullptr || desc.surfaces.output == nullptr) {
        m_message = "TAA resolve skipped — missing current/output surfaces";
        return false;
    }

    m_stats.resolved = true;
    m_stats.width = desc.width;
    m_stats.height = desc.height;
    m_stats.last_blend = desc.params.blend_factor;
    m_stats.history_swapped = true;
    history.swap();

#if defined(FUSE_HAS_CUDA)
    m_message = "TAA resolve recorded (CUDA kernel deferred)";
#else
    m_message = "TAA resolve recorded (CPU stub — no CUDA toolkit)";
#endif

    return true;
}

} // namespace fuse::renderer
