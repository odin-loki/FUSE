#include <fuse/net/rollback_buffer.hpp>

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

bool RollbackBuffer::has_frame(u32 frame) const {
    const std::optional<u32> slot = slot_index_(frame);
    return slot.has_value();
}

std::optional<u32> RollbackBuffer::slot_index_(u32 frame) const {
    if (m_capacity == 0) {
        return std::nullopt;
    }

    const u32 slot = frame % m_capacity;
    if (!m_slots[slot].has_snapshot || m_slots[slot].snapshot.frame != frame) {
        return std::nullopt;
    }
    return slot;
}

void RollbackBuffer::store_snapshot(u32 frame, GameSnapshot snapshot) {
    if (m_capacity == 0) {
        return;
    }

    snapshot.frame = frame;
    const u32 slot = frame % m_capacity;
    m_slots[slot].snapshot = std::move(snapshot);
    m_slots[slot].has_snapshot = true;

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
        ++m_oldest_frame;
    }
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
    m_slots[frame % m_capacity].local_input = input;
}

void RollbackBuffer::store_remote_input(u32 frame, const PlayerInput& input, bool confirmed) {
    if (m_capacity == 0) {
        return;
    }
    const u32 slot = frame % m_capacity;
    m_slots[slot].remote_input = input;
    m_slots[slot].remote_confirmed = confirmed;
}

PlayerInput RollbackBuffer::local_input(u32 frame) const {
    if (m_capacity == 0) {
        return {};
    }
    return m_slots[frame % m_capacity].local_input;
}

PlayerInput RollbackBuffer::remote_input(u32 frame) const {
    if (m_capacity == 0) {
        return {};
    }
    return m_slots[frame % m_capacity].remote_input;
}

bool RollbackBuffer::remote_confirmed(u32 frame) const {
    if (m_capacity == 0) {
        return false;
    }
    return m_slots[frame % m_capacity].remote_confirmed;
}

} // namespace fuse::net
