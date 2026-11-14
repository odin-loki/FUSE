#pragma once

// Pipeline builds on worker threads while a renderer brings up many subsystems at once.
//
// Creating a pipeline is CPU work in the driver (SPIR-V -> driver IR, optimisation and, for some drivers, the
// machine-code compile). A FrameComposer creates about a hundred of them, one subsystem after the other, which on
// a software rasteriser (Lavapipe) is most of its bring-up. vkCreate*Pipelines may be called on one VkDevice from
// several threads at once (the device is not externally synchronised for it), so the builds can overlap.
//
// A PipelineBatch opened on a thread collects the builds the code on that thread queues with buildPipeline()
// and runs them on worker threads; wait() (or the destructor) blocks until they are all done. Without an open
// batch buildPipeline() runs the build at once, so a subsystem initialised on its own behaves as before.
//
// A queued build must own (capture by value) everything it reads, and may write only memory that outlives the
// batch's wait() (the owning subsystem's pipeline members). The owning subsystem must call
// waitPipelineBuilds(this) before it destroys those members (destroy() on a failed init).
// Results are identical to a serial build: each pipeline is created from the same create info.

#include <fuse/types.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace fuse::renderer {

class PipelineBatch {
public:
    /// Opens the batch on the calling thread. A batch opened while another one is open on the same thread is
    /// inactive: its builds go to the outer batch, and its wait() waits for that one.
    /// threads == 0: one worker per hardware thread (at most 16).
    explicit PipelineBatch(u32 threads = 0);
    ~PipelineBatch();
    PipelineBatch(const PipelineBatch&) = delete;
    PipelineBatch& operator=(const PipelineBatch&) = delete;

    /// Blocks until every queued build has finished (the calling thread runs queued builds too). Returns false
    /// when a build since the batch was opened failed; failed(owner) tells whose.
    bool wait();
    /// Blocks until the builds queued with `owner` have finished (runs the queued ones on the calling thread).
    void waitFor(const void* owner);
    /// A build queued with this owner returned false (valid after wait()).
    [[nodiscard]] bool failed(const void* owner) const;
    /// Builds queued so far (including ones still running).
    [[nodiscard]] u32 queued() const;

    /// The batch open on the calling thread, or nullptr.
    [[nodiscard]] static PipelineBatch* current();

    /// Queues `build` (see buildPipeline). Workers take the queued build with the largest `cost` first (the
    /// SPIR-V size: the longest builds start early, so the batch does not end on one long build started last).
    void enqueue(const void* owner, std::function<bool()> build, u64 cost = 0);

private:
    struct Task {
        const void* owner = nullptr;
        std::function<bool()> build;
        u64 cost = 0;
    };
    void workerLoop();
    bool runOne(std::unique_lock<std::mutex>& lock, const void* owner, bool anyOwner);

    PipelineBatch* m_outer = nullptr; // the batch that was current when this one opened (this one is inactive)
    u32 m_threadLimit = 0;
    mutable std::mutex m_mutex;
    std::condition_variable m_work;
    std::condition_variable m_done;
    std::deque<Task> m_tasks;
    std::vector<std::thread> m_threads;
    std::vector<const void*> m_failed;
    std::vector<const void*> m_runningOwners;
    u32 m_queued = 0;
    bool m_stop = false;
};

/// Runs `build` now and returns its result, or, while a PipelineBatch is open on the calling thread, queues it on
/// that batch and returns true (the batch's wait() reports a failure, keyed by `owner`).
/// `cost` orders the queued builds (PipelineBatch::enqueue): the SPIR-V byte size of the stages.
bool buildPipeline(const void* owner, std::function<bool()> build, u64 cost = 0);

/// Waits for the builds `owner` queued on the batch open on the calling thread (no-op without one). A subsystem
/// calls it before it destroys pipelines it may have queued (destroy() of a failed or repeated init).
void waitPipelineBuilds(const void* owner);

/// vkCreateShaderModule + vkCreateComputePipelines (entry point "main") + vkDestroyShaderModule: the compute
/// pipeline most renderer kernels are. device / layout / *out are Vk handles; `words` must stay valid until the
/// build has run (the kernels are static SPIR-V arrays). *out is written only on success.
bool createComputePipelineSpirv(void* device, const u32* words, usize bytes, void* layout, u32 createFlags, void** out);
/// buildPipeline() of createComputePipelineSpirv.
bool buildComputePipeline(const void* owner, void* device, const u32* words, usize bytes, void* layout, u32 createFlags,
                          void** out);

} // namespace fuse::renderer
