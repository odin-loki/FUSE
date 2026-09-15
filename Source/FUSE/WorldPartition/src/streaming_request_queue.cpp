#include <fuse/world_partition/streaming_request_queue.hpp>

#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>

namespace fuse::world_partition {

bool StreamingRequestQueue::submit(StreamingRequest request, StreamingWorkFn work) {
    if (work == nullptr) {
        return false;
    }

    auto& scheduler = fuse::jobs::JobScheduler::instance();
    if (!scheduler.isInitialized()) {
        return false;
    }

    u64 submit_sequence = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_max_pending_submits > 0u && m_inFlight + static_cast<u32>(m_completed.size()) >= m_max_pending_submits) {
            return false;
        }
        submit_sequence = ++m_submit_sequence;
        ++m_inFlight;
    }

    scheduler.submit([this, request = std::move(request), work = std::move(work), submit_sequence]() mutable {
        CompletedStreamingRequest completed{};
        completed.coord = request.coord;
        completed.kind = request.kind;
        completed.priority = request.priority;
        completed.submit_sequence = submit_sequence;
        completed.success = work(request.coord, request.kind);
        push_completed_(std::move(completed));

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inFlight > 0) {
            --m_inFlight;
        }
    });

    return true;
}

u32 StreamingRequestQueue::drain_completed(std::vector<CompletedStreamingRequest>& out) {
    std::vector<CompletedStreamingRequest> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        batch.swap(m_completed);
    }

    std::sort(batch.begin(), batch.end(),
              [](const CompletedStreamingRequest& a, const CompletedStreamingRequest& b) {
                  if (a.priority != b.priority) {
                      return a.priority > b.priority;
                  }
                  return a.submit_sequence < b.submit_sequence;
              });

    out.insert(out.end(), batch.begin(), batch.end());
    return static_cast<u32>(batch.size());
}

u32 StreamingRequestQueue::pending_submit_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight + static_cast<u32>(m_completed.size());
}

u32 StreamingRequestQueue::in_flight_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight;
}

u32 StreamingRequestQueue::completed_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<u32>(m_completed.size());
}

bool StreamingRequestQueue::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight == 0u && m_completed.empty();
}

void StreamingRequestQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inFlight = 0;
    m_submit_sequence = 0;
    m_completed.clear();
}

void StreamingRequestQueue::push_completed_(CompletedStreamingRequest completed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completed.push_back(std::move(completed));
}

} // namespace fuse::world_partition
