#include <fuse/net/rollback_buffer.hpp>

#include <fuse/net/input_history.hpp>
#include <fuse/net/reconcile.hpp>

#include <algorithm>

namespace fuse::net {

void RollbackBuffer::init(u32 capacity_frames) {
    clear();
    m_capacity = std::min(std::max(capacity_frames, 1u), kMaxCapacity);
}

void RollbackBuffer::clear() {
    m_slots = {};
    m_capacity = 0;
    m_oldest_frame = 0;
    m_newest_frame = 0;
    m_has_any_frame = false;
}

u32 RollbackBuffer::stored_frame_count() const {
    if (!m_has_any_frame || m_capacity == 0) {
        return 0;
    }
    return m_newest_frame - m_oldest_frame + 1;
}

bool RollbackBuffer::has_frame(u32 frame) const {
    const std::optional<u32> slot = slot_index_(frame);
    return slot.has_value();
}

bool RollbackBuffer::has_local_input(u32 frame) const {
    const std::optional<u32> slot = slot_index_(frame);
    if (!slot.has_value()) {
        return false;
    }
    return m_slots[*slot].has_local_input;
}

std::optional<u32> RollbackBuffer::slot_index_(u32 frame) const {
    if (m_capacity == 0 || !m_has_any_frame) {
        return std::nullopt;
    }

    if (frame < m_oldest_frame || frame > m_newest_frame) {
        return std::nullopt;
    }

    const u32 slot = frame % m_capacity;
    if (!m_slots[slot].has_snapshot || m_slots[slot].frame != frame) {
        return std::nullopt;
    }
    return slot;
}

std::optional<GameSnapshot> RollbackBuffer::evict_oldest_snapshot() {
    if (!m_has_any_frame || m_capacity == 0) {
        return std::nullopt;
    }

    const u32 frame = m_oldest_frame;
    std::optional<GameSnapshot> evicted;
    const std::optional<u32> slot = slot_index_(frame);
    if (slot.has_value()) {
        evicted = m_slots[*slot].snapshot;
        m_slots[*slot] = {};
    }

    if (m_oldest_frame == m_newest_frame) {
        m_has_any_frame = false;
        m_oldest_frame = 0;
        m_newest_frame = 0;
    } else {
        ++m_oldest_frame;
    }

    return evicted;
}

void RollbackBuffer::store_snapshot(u32 frame, GameSnapshot snapshot) {
    if (m_capacity == 0) {
        return;
    }

    if (!m_has_any_frame) {
        m_oldest_frame = frame;
        m_newest_frame = frame;
        m_has_any_frame = true;
    } else {
        if (frame < m_oldest_frame) {
            m_oldest_frame = frame;
        }
        if (frame > m_newest_frame) {
            m_newest_frame = frame;
        }
    }

    while (m_newest_frame - m_oldest_frame + 1 > m_capacity) {
        const u32 evict_slot = m_oldest_frame % m_capacity;
        if (evict_slot != frame % m_capacity) {
            m_slots[evict_slot] = {};
        }
        ++m_oldest_frame;
    }

    snapshot.frame = frame;
    const u32 slot = frame % m_capacity;
    m_slots[slot].frame = frame;
    m_slots[slot].snapshot = std::move(snapshot);
    m_slots[slot].has_snapshot = true;
}

const GameSnapshot* RollbackBuffer::snapshot(u32 frame) const {
    const std::optional<u32> slot = slot_index_(frame);
    if (!slot.has_value()) {
        return nullptr;
    }
    return &m_slots[*slot].snapshot;
}

GameSnapshot* RollbackBuffer::snapshot_mut(u32 frame) {
    const std::optional<u32> slot = slot_index_(frame);
    if (!slot.has_value()) {
        return nullptr;
    }
    return &m_slots[*slot].snapshot;
}

void RollbackBuffer::store_local_input(u32 frame, const PlayerInput& input) {
    if (m_capacity == 0) {
        return;
    }

    const u32 slot = frame % m_capacity;
    m_slots[slot].frame = frame;
    m_slots[slot].local_input = input;
    m_slots[slot].local_input.frame = frame;
    m_slots[slot].has_local_input = true;
}

void RollbackBuffer::store_remote_input(u32 frame, const PlayerInput& input, bool confirmed) {
    if (m_capacity == 0) {
        return;
    }

    const u32 slot = frame % m_capacity;
    m_slots[slot].frame = frame;
    m_slots[slot].remote_input = input;
    m_slots[slot].remote_input.frame = frame;
    m_slots[slot].remote_confirmed = confirmed;
}

PlayerInput RollbackBuffer::local_input(u32 frame) const {
    const std::optional<u32> slot = slot_index_(frame);
    if (!slot.has_value() || !m_slots[*slot].has_local_input) {
        return {};
    }
    return m_slots[*slot].local_input;
}

PlayerInput RollbackBuffer::remote_input(u32 frame) const {
    const std::optional<u32> slot = slot_index_(frame);
    if (!slot.has_value() || !m_slots[*slot].remote_confirmed) {
        return {};
    }
    return m_slots[*slot].remote_input;
}

bool RollbackBuffer::remote_confirmed(u32 frame) const {
    const std::optional<u32> slot = slot_index_(frame);
    if (!slot.has_value()) {
        return false;
    }
    return m_slots[*slot].remote_confirmed;
}

bool RollbackBuffer::inputs_match(u32 frame) const {
    const std::optional<u32> slot = slot_index_(frame);
    if (!slot.has_value()) {
        return false;
    }

    const FrameSlot& entry = m_slots[*slot];
    if (!entry.has_local_input || !entry.remote_confirmed) {
        return false;
    }
    return inputs_equal(entry.local_input, entry.remote_input);
}

ReconcileResult RollbackBuffer::reconcile_remote_input(u32 frame, const PlayerInput& remote) {
    return reconcile_rollback_buffer(*this, frame, remote);
}

} // namespace fuse::net
