#pragma once

#include <fuse/types.hpp>

namespace fuse::frame {

/// Sync point at end of tick — all jobs for frame N complete before render record.
class FrameBarrier {
public:
    void beginTick(u32 frameIndex);
    void signalTickJobsComplete();
    void waitForTickComplete();

    u32 frameIndex() const { return m_frameIndex; }
    bool tickJobsComplete() const { return m_tickJobsComplete; }

private:
    u32 m_frameIndex = 0;
    bool m_tickJobsComplete = false;
};

} // namespace fuse::frame
