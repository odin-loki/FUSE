#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/worker_context.hpp>

#include <condition_variable>
#include <mutex>

namespace fuse::jobs {

JobCounter::JobCounter(u32 initial) : m_remaining(initial) {}

void JobCounter::reset(u32 value) {
    std::lock_guard<std::mutex> lock(m_waitMutex);
    m_remaining.store(value, std::memory_order_release);
}

void JobCounter::add(u32 delta) {
    m_remaining.fetch_add(delta, std::memory_order_acq_rel);
}

void JobCounter::signal() {
    std::lock_guard<std::mutex> lock(m_waitMutex);
    // acq_rel: a waiter that observes zero (even on the lock-free fast path) must see every write
    // the signalling jobs made before signal().
    const u32 prev = m_remaining.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        m_waitCv.notify_all();
    }
}

void JobCounter::synchronizeCompletion() {
    std::lock_guard<std::mutex> lock(m_waitMutex);
}

bool JobCounter::isComplete() const {
    return m_remaining.load(std::memory_order_acquire) == 0;
}

u32 JobCounter::remaining() const {
    return m_remaining.load(std::memory_order_acquire);
}

void JobCounter::wait() {
    if (isComplete()) {
        // The last signaller may still be inside signal() holding m_waitMutex; returning now would
        // let the caller destroy this counter under it. Acquire the mutex once to wait it out.
        synchronizeCompletion();
        return;
    }

    if (detail::workerWaitOnCounter(this)) {
        synchronizeCompletion();
        return;
    }

    std::unique_lock<std::mutex> lock(m_waitMutex);
    m_waitCv.wait(lock, [this] { return m_remaining.load(std::memory_order_acquire) == 0; });
}

} // namespace fuse::jobs
