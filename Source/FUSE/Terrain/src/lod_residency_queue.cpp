#include <fuse/terrain/lod_residency_queue.hpp>

#include <fuse/jobs/job_scheduler.hpp>

namespace fuse::terrain {

bool LodResidencyQueue::submit(LodResidencyRequest request, LodResidencyWorkFn work) {
    if (work == nullptr) {
        return false;
    }

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    if (!scheduler.isInitialized()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_inFlight;
    }

    scheduler.submit([this, request = std::move(request), work = std::move(work)]() mutable {
        CompletedLodResidencyRequest completed{};
        completed.chunk_index = request.chunk_index;
        completed.kind = request.kind;
        completed.success = work(request.chunk_index, request.kind);
        push_completed_(std::move(completed));

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inFlight > 0) {
            --m_inFlight;
        }
    });

    return true;
}

u32 LodResidencyQueue::drain_completed(std::vector<CompletedLodResidencyRequest>& out) {
    std::vector<CompletedLodResidencyRequest> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        batch.swap(m_completed);
    }

    out.insert(out.end(), batch.begin(), batch.end());
    return static_cast<u32>(batch.size());
}

u32 LodResidencyQueue::in_flight_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight;
}

u32 LodResidencyQueue::completed_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<u32>(m_completed.size());
}

void LodResidencyQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inFlight = 0;
    m_completed.clear();
}

void LodResidencyQueue::push_completed_(CompletedLodResidencyRequest completed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completed.push_back(std::move(completed));
}

} // namespace fuse::terrain
