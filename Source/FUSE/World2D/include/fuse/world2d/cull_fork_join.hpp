#pragma once

#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/types.hpp>

#include <type_traits>

namespace fuse::world2d {

/// Heap-free fork-join over [0, count) for the per-frame dimension cull passes (gate 3.5).
///
/// Same chunking and serial fallback as `jobs::parallel_for`, but `parallel_for` allocates a shared
/// counter plus one `std::function` per chunk (its captures overflow the small-object buffer). Here
/// the counter lives in the owner and each chunk job captures only `{this, chunk}`, which every
/// standard library stores inline, so a steady-state run never touches the heap.
///
/// Not reentrant: one `run()` per instance at a time, and the body must not call back into it.
class CullForkJoin {
public:
    CullForkJoin() = default;
    CullForkJoin(const CullForkJoin&) = delete;
    CullForkJoin& operator=(const CullForkJoin&) = delete;

    template <typename Body>
    void run(u32 count, u32 grainSize, const Body& body) {
        if (grainSize == 0) {
            grainSize = 1;
        }

        jobs::JobScheduler& scheduler = jobs::JobScheduler::instance();
        if (!scheduler.isInitialized() || scheduler.isSingleThreaded() || count == 0) {
            for (u32 i = 0; i < count; ++i) {
                body(i);
            }
            return;
        }

        m_body = &body;
        m_invoke = [](const void* bodyPtr, u32 begin, u32 end) {
            const Body& typedBody = *static_cast<const Body*>(bodyPtr);
            for (u32 i = begin; i < end; ++i) {
                typedBody(i);
            }
        };
        m_count = count;
        m_grainSize = grainSize;

        for (u32 chunk = 0; chunk < count; chunk += grainSize) {
            m_counter.add(1);
            auto job = [this, chunk]() { runChunk(chunk); };
            static_assert(std::is_trivially_copyable<decltype(job)>::value && sizeof(job) <= 2 * sizeof(void*),
                          "chunk job must fit std::function's inline storage");
            scheduler.submit(job);
        }
        m_counter.wait();

        m_body = nullptr;
        m_invoke = nullptr;
    }

private:
    void runChunk(u32 chunk) {
        const u32 chunkEnd = (chunk + m_grainSize < m_count) ? (chunk + m_grainSize) : m_count;
        m_invoke(m_body, chunk, chunkEnd);
        m_counter.signal();
    }

    jobs::JobCounter m_counter{0};
    const void* m_body = nullptr;
    void (*m_invoke)(const void*, u32, u32) = nullptr;
    u32 m_count = 0;
    u32 m_grainSize = 1;
};

} // namespace fuse::world2d
