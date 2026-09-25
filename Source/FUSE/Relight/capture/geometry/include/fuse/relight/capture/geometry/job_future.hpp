// FUSE Relight RL-1.3: results of geometry jobs run on FUSE's JobScheduler.
//
// Upstream (dxvk-remix @0867d3c, src/d3d9/d3d9_rtx*.cpp) schedules the geometry hashes, the
// bounding box and the skinning data on its own GeometryProcessor thread pool and hands the draw a
// dxvk::Future. JobFuture is the FUSE equivalent: the job runs on fuse::jobs::JobScheduler (inline
// when the scheduler is not initialised or single-threaded) and get() waits on a JobCounter, which
// yields instead of blocking when called from a worker with fibers.
#pragma once

#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <memory>
#include <type_traits>
#include <utility>

namespace fuse::relight::capture::geometry {

template <typename T>
class JobFuture {
public:
    JobFuture() = default;

    /// False for an empty future (upstream's default-constructed Future: nothing was scheduled).
    [[nodiscard]] bool valid() const noexcept { return m_state != nullptr; }

    /// The job has finished. Requires valid().
    [[nodiscard]] bool isReady() const { return m_state->counter.isComplete(); }

    /// Waits for the job and returns its result. Requires valid().
    [[nodiscard]] const T& get() const {
        m_state->counter.wait();
        return m_state->value;
    }

    /// Runs `fn` (returning T) as a job on JobScheduler::instance(), or inline when `async` is false.
    /// `fn` must be copyable (std::function); capture shared data by shared_ptr.
    template <typename Fn>
    [[nodiscard]] static JobFuture schedule(Fn&& fn, bool async) {
        static_assert(std::is_convertible_v<std::invoke_result_t<Fn&>, T>, "job must return T");
        JobFuture future;
        future.m_state = std::make_shared<State>();
        if (!async) {
            future.m_state->value = fn();
            future.m_state->counter.signal();
            return future;
        }
        jobs::JobScheduler::instance().submit(
            [state = future.m_state, job = std::forward<Fn>(fn)]() mutable {
                state->value = job();
                state->counter.signal();
            });
        return future;
    }

    /// A future that is already complete.
    [[nodiscard]] static JobFuture fromValue(T value) {
        JobFuture future;
        future.m_state = std::make_shared<State>();
        future.m_state->value = std::move(value);
        future.m_state->counter.signal();
        return future;
    }

private:
    struct State {
        jobs::JobCounter counter{1};
        T value{};
    };
    std::shared_ptr<State> m_state;
};

} // namespace fuse::relight::capture::geometry
