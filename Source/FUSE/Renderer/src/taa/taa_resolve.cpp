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

    m_stats.first_frame = !history.hasValidHistory();
    history.markResolved();
    history.swap();

    m_stats.resolved = true;
    m_stats.width = desc.width;
    m_stats.height = desc.height;
    m_stats.last_blend = desc.params.blend_factor;
    m_stats.history_swapped = true;
    m_stats.has_valid_history = history.hasValidHistory();
    m_stats.accumulated_frames = history.accumulatedFrames();

#if defined(FUSE_HAS_CUDA)
    m_message = m_stats.first_frame ? "TAA resolve recorded — first frame (CUDA kernel deferred)"
                                    : "TAA resolve recorded (CUDA kernel deferred)";
#else
    m_message = m_stats.first_frame ? "TAA resolve recorded — first frame (CPU stub — no CUDA toolkit)"
                                    : "TAA resolve recorded (CPU stub — no CUDA toolkit)";
#endif

    return true;
}

} // namespace fuse::renderer
