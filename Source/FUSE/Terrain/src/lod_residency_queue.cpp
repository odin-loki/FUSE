#include <fuse/terrain/lod_residency_queue.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/terrain/lod_residency_budget.hpp>
#include <fuse/terrain/lod.hpp>

#include <algorithm>

namespace fuse::terrain {

f32 promote_residency_priority(f32 current, f32 incoming) {
    return std::max(current, incoming);
}

f32 demote_residency_priority(f32 current, f32 scale) {
    return current * std::clamp(scale, 0.f, 1.f);
}

LodResidencyMorphSnapshot capture_morph_snapshot(u32 lod, f32 morph_factor) {
    LodResidencyMorphSnapshot snapshot{};
    snapshot.lod = lod;
    snapshot.morph_factor = clamp_morph_factor(morph_factor);
    return snapshot;
}

void sync_morph_after_residency(TerrainChunk& chunk, const LodResidencyMorphSnapshot& snapshot) {
    if (chunk.lod == snapshot.lod) {
        chunk.morph_factor = snapshot.morph_factor;
    }
    chunk.dirty = chunk.loaded && (chunk.morph_factor > 0.f);
}

int compare_lod_residency_request_order(f32 priority_a, LodResidencyRequestKind kind_a, u64 sequence_a,
                                        f32 priority_b, LodResidencyRequestKind kind_b, u64 sequence_b) {
    if (priority_a != priority_b) {
        return priority_a > priority_b ? 1 : -1;
    }
    if (kind_a != kind_b) {
        return kind_a == LodResidencyRequestKind::Unload ? 1 : -1;
    }
    if (sequence_a != sequence_b) {
        return sequence_a < sequence_b ? 1 : -1;
    }
    return 0;
}

void LodResidencyQueue::sort_pending_by_priority_() {
    std::sort(m_pending.begin(), m_pending.end(),
              [](const PendingLodResidencyRequest& a, const PendingLodResidencyRequest& b) {
                  return compare_lod_residency_request_order(a.request.priority, a.request.kind,
                                                             a.enqueue_sequence, b.request.priority,
                                                             b.request.kind, b.enqueue_sequence) > 0;
              });
}

bool LodResidencyQueue::enqueue(LodResidencyRequest request) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&](const PendingLodResidencyRequest& pending) {
                                           return pending.request.chunk_index == request.chunk_index &&
                                                  pending.request.kind == request.kind;
                                       });
    if (existing != m_pending.end()) {
        existing->request.priority = promote_residency_priority(existing->request.priority, request.priority);
        existing->request.morph_snapshot = request.morph_snapshot;
        return true;
    }

    PendingLodResidencyRequest pending{};
    pending.request = std::move(request);
    pending.enqueue_sequence = ++m_enqueue_sequence;
    m_pending.push_back(std::move(pending));
    return true;
}

bool LodResidencyQueue::dequeue(LodResidencyRequest& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.empty()) {
        return false;
    }

    sort_pending_by_priority_();
    out = std::move(m_pending.front().request);
    m_pending.erase(m_pending.begin());
    return true;
}

bool LodResidencyQueue::has_pending_enqueue() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_pending.empty();
}

bool LodResidencyQueue::peek_pending(LodResidencyRequest& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pending.empty()) {
        return false;
    }

    const PendingLodResidencyRequest& best = *std::max_element(
        m_pending.begin(), m_pending.end(),
        [](const PendingLodResidencyRequest& a, const PendingLodResidencyRequest& b) {
            return compare_lod_residency_request_order(a.request.priority, a.request.kind, a.enqueue_sequence,
                                                       b.request.priority, b.request.kind, b.enqueue_sequence) < 0;
        });
    out = best.request;
    return true;
}

bool LodResidencyQueue::demote(u32 chunk_index, LodResidencyRequestKind kind, f32 scale) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&](const PendingLodResidencyRequest& pending) {
                                           return pending.request.chunk_index == chunk_index &&
                                                  pending.request.kind == kind;
                                       });
    if (existing == m_pending.end()) {
        return false;
    }

    existing->request.priority = demote_residency_priority(existing->request.priority, scale);
    return true;
}

u32 LodResidencyQueue::flush(u32 budget, LodResidencyWorkFn work) {
    if (work == nullptr || budget == 0u) {
        return 0u;
    }

    std::vector<PendingLodResidencyRequest> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.empty()) {
            return 0u;
        }

        sort_pending_by_priority_();

        const u32 count = std::min(budget, static_cast<u32>(m_pending.size()));
        batch.assign(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
    }

    u32 submitted = 0u;
    for (PendingLodResidencyRequest& pending : batch) {
        LodResidencyRequest request = pending.request;
        const u64 enqueue_sequence = pending.enqueue_sequence;
        if (submit(std::move(request), work)) {
            ++submitted;
            continue;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        PendingLodResidencyRequest restored{};
        restored.request = pending.request;
        restored.enqueue_sequence = enqueue_sequence;
        m_pending.push_back(std::move(restored));
    }
    return submitted;
}

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
        if (would_exceed_budget_()) {
            return false;
        }
        ++m_inFlight;
    }

    scheduler.submit([this, request = std::move(request), work = std::move(work)]() mutable {
        CompletedLodResidencyRequest completed{};
        completed.chunk_index = request.chunk_index;
        completed.kind = request.kind;
        completed.priority = request.priority;
        completed.morph_snapshot = request.morph_snapshot;
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

    std::sort(batch.begin(), batch.end(),
              [](const CompletedLodResidencyRequest& a, const CompletedLodResidencyRequest& b) {
                  return a.priority > b.priority;
              });

    out.insert(out.end(), batch.begin(), batch.end());
    return static_cast<u32>(batch.size());
}

u32 LodResidencyQueue::pending_enqueue_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<u32>(m_pending.size());
}

u32 LodResidencyQueue::pending_submit_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight + static_cast<u32>(m_completed.size());
}

f32 LodResidencyQueue::pending_priority_for(u32 chunk_index, LodResidencyRequestKind kind) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&](const PendingLodResidencyRequest& pending) {
                                           return pending.request.chunk_index == chunk_index &&
                                                  pending.request.kind == kind;
                                       });
    return existing != m_pending.end() ? existing->request.priority : -1.f;
}

u32 LodResidencyQueue::in_flight_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight;
}

u32 LodResidencyQueue::completed_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<u32>(m_completed.size());
}

bool LodResidencyQueue::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.empty() && m_inFlight == 0u && m_completed.empty();
}

void LodResidencyQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inFlight = 0;
    m_enqueue_sequence = 0;
    m_pending.clear();
    m_completed.clear();
}

bool LodResidencyQueue::would_exceed_budget_() const {
    return would_exceed_pending_submit_budget(m_inFlight, static_cast<u32>(m_completed.size()),
                                             m_max_pending_submits);
}

void LodResidencyQueue::push_completed_(CompletedLodResidencyRequest completed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completed.push_back(std::move(completed));
}

} // namespace fuse::terrain
