#pragma once

#include <fuse/types.hpp>

namespace fuse::jobs {

/// Dependency counter for fork-join jobs (stub — U1/WP-03).
class JobCounter {
public:
    explicit JobCounter(u32 initial = 0) : m_remaining(initial) {}

    void reset(u32 value) { m_remaining = value; }
    void signal() { if (m_remaining > 0) { --m_remaining; } }
    bool isComplete() const { return m_remaining == 0; }
    u32 remaining() const { return m_remaining; }

    /// Yield until complete (stub: no-op spin check).
    void wait();

private:
    u32 m_remaining;
};

} // namespace fuse::jobs
