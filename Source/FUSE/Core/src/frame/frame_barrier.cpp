#include <fuse/frame/frame_barrier.hpp>

namespace fuse::frame {

void FrameBarrier::beginTick(u32 frameIndex) {
    m_frameIndex = frameIndex;
    m_tickJobsComplete = false;
}

void FrameBarrier::signalTickJobsComplete() {
    m_tickJobsComplete = true;
}

void FrameBarrier::waitForTickComplete() {
    // v1: tick jobs are joined via JobCounter before signalTickJobsComplete().
    // This hook exists for future async tick lanes (I/O, deferred physics).
}

} // namespace fuse::frame
