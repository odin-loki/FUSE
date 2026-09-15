#include <fuse/world_partition/streaming_request_queue.hpp>

#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>

namespace fuse::world_partition {

f32 promote_streaming_priority(f32 current, f32 incoming) {
    return std::max(current, incoming);
}

f32 demote_streaming_priority(f32 current, f32 scale) {
    return current * std::clamp(scale, 0.f, 1.f);
}

int compare_streaming_request_order(f32 priority_a, StreamingRequestKind kind_a, u64 sequence_a,
                                    f32 priority_b, StreamingRequestKind kind_b, u64 sequence_b) {
    if (priority_a != priority_b) {
        return priority_a > priority_b ? 1 : -1;
    }
    if (kind_a != kind_b) {
        return kind_a == StreamingRequestKind::Unload ? 1 : -1;
    }
    if (sequence_a != sequence_b) {
        return sequence_a < sequence_b ? 1 : -1;
    }
    return 0;
}

bool StreamingRequestQueue::enqueue(StreamingRequest request) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&](const PendingStreamingRequest& pending) {
                                           return pending.request.coord == request.coord &&
                                                  pending.request.kind == request.kind;
                                       });
    if (existing != m_pending.end()) {
        existing->request.priority =
            promote_streaming_priority(existing->request.priority, request.priority);
        return true;
    }

    PendingStreamingRequest pending{};
    pending.request = std::move(request);
    pending.enqueue_sequence = ++m_enqueue_sequence;
    m_pending.push_back(std::move(pending));
    return true;
}

bool StreamingRequestQueue::demote(GridCoord coord, StreamingRequestKind kind, f32 scale) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&](const PendingStreamingRequest& pending) {
                                           return pending.request.coord == coord &&
                                                  pending.request.kind == kind;
                                       });
    if (existing == m_pending.end()) {
        return false;
    }

    existing->request.priority = demote_streaming_priority(existing->request.priority, scale);
    return true;
}

u32 StreamingRequestQueue::flush(u32 budget, StreamingWorkFn work) {
    if (work == nullptr || budget == 0u) {
        return 0u;
    }

    std::vector<PendingStreamingRequest> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.empty()) {
            return 0u;
        }

        std::sort(m_pending.begin(), m_pending.end(),
                  [](const PendingStreamingRequest& a, const PendingStreamingRequest& b) {
                      return compare_streaming_request_order(a.request.priority, a.request.kind,
                                                             a.enqueue_sequence, b.request.priority,
                                                             b.request.kind, b.enqueue_sequence) > 0;
                  });

        const u32 count = std::min(budget, static_cast<u32>(m_pending.size()));
        batch.assign(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
    }

    u32 submitted = 0u;
    for (PendingStreamingRequest& pending : batch) {
        StreamingRequest request = pending.request;
        const u64 enqueue_sequence = pending.enqueue_sequence;
        if (submit(std::move(request), work)) {
            ++submitted;
            continue;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        PendingStreamingRequest restored{};
        restored.request = pending.request;
        restored.enqueue_sequence = enqueue_sequence;
        m_pending.push_back(std::move(restored));
    }
    return submitted;
}

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
        if (would_exceed_budget_()) {
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
                  return compare_streaming_request_order(a.priority, a.kind, a.submit_sequence,
                                                         b.priority, b.kind, b.submit_sequence) > 0;
              });

    out.insert(out.end(), batch.begin(), batch.end());
    return static_cast<u32>(batch.size());
}

u32 StreamingRequestQueue::pending_enqueue_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<u32>(m_pending.size());
}

u32 StreamingRequestQueue::pending_submit_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight + static_cast<u32>(m_completed.size());
}

f32 StreamingRequestQueue::pending_priority_for(GridCoord coord, StreamingRequestKind kind) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&](const PendingStreamingRequest& pending) {
                                           return pending.request.coord == coord &&
                                                  pending.request.kind == kind;
                                       });
    return existing != m_pending.end() ? existing->request.priority : -1.f;
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
    return m_pending.empty() && m_inFlight == 0u && m_completed.empty();
}

void StreamingRequestQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inFlight = 0;
    m_submit_sequence = 0;
    m_enqueue_sequence = 0;
    m_pending.clear();
    m_completed.clear();
}

bool StreamingRequestQueue::would_exceed_budget_() const {
    if (m_max_pending_submits == 0u) {
        return false;
    }
    return m_inFlight + static_cast<u32>(m_completed.size()) >= m_max_pending_submits;
}

void StreamingRequestQueue::push_completed_(CompletedStreamingRequest completed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completed.push_back(std::move(completed));
}

} // namespace fuse::world_partition
