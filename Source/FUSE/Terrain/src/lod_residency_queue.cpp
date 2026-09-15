#include <fuse/terrain/lod_residency_queue.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/terrain/lod.hpp>
#include <fuse/terrain/lod_residency_budget.hpp>

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

bool LodResidencyQueue::enqueue(LodResidencyRequest request) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&](const LodResidencyRequest& pending) {
                                           return pending.chunk_index == request.chunk_index &&
                                                  pending.kind == request.kind;
                                       });
    if (existing != m_pending.end()) {
        existing->priority = promote_residency_priority(existing->priority, request.priority);
        existing->morph_snapshot = request.morph_snapshot;
        return true;
    }

    m_pending.push_back(std::move(request));
    return true;
}

bool LodResidencyQueue::demote(u32 chunk_index, LodResidencyRequestKind kind, f32 scale) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = std::find_if(m_pending.begin(), m_pending.end(),
                                       [&](const LodResidencyRequest& pending) {
                                           return pending.chunk_index == chunk_index && pending.kind == kind;
                                       });
    if (existing == m_pending.end()) {
        return false;
    }

    existing->priority = demote_residency_priority(existing->priority, scale);
    return true;
}

u32 LodResidencyQueue::flush(u32 budget, LodResidencyWorkFn work) {
    if (work == nullptr || budget == 0u) {
        return 0u;
    }

    std::vector<LodResidencyRequest> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.empty()) {
            return 0u;
        }

        std::sort(m_pending.begin(), m_pending.end(),
                  [](const LodResidencyRequest& a, const LodResidencyRequest& b) {
                      return a.priority > b.priority;
                  });

        const u32 async_budget =
            clamp_residency_flush_budget(budget, m_inFlight, m_max_async_in_flight);
        const u32 count = std::min(async_budget, static_cast<u32>(m_pending.size()));
        batch.assign(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
    }

    u32 submitted = 0u;
    for (LodResidencyRequest& request : batch) {
        if (submit(std::move(request), work)) {
            ++submitted;
        }
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
        if (would_exceed_budget_() || would_exceed_async_in_flight_()) {
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
    m_pending.clear();
    m_completed.clear();
}

bool LodResidencyQueue::would_exceed_budget_() const {
    if (m_max_pending_submits == 0u) {
        return false;
    }
    return m_inFlight + static_cast<u32>(m_completed.size()) >= m_max_pending_submits;
}

bool LodResidencyQueue::would_exceed_async_in_flight_() const {
    return !can_submit_async_load(m_inFlight, m_max_async_in_flight);
}

void LodResidencyQueue::push_completed_(CompletedLodResidencyRequest completed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completed.push_back(std::move(completed));
}

} // namespace fuse::terrain
