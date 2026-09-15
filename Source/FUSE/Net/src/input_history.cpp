#include <fuse/net/input_history.hpp>

#include <fuse/net/reconcile.hpp>

#include <algorithm>

namespace fuse::net {

bool inputs_equal(const PlayerInput& a, const PlayerInput& b) {
    return a.player_id == b.player_id && a.buttons == b.buttons && a.axis_lx == b.axis_lx &&
           a.axis_ly == b.axis_ly && a.axis_rx == b.axis_rx && a.axis_ry == b.axis_ry;
}

void InputHistoryBuffer::init(u32 capacity_frames) {
    clear();
    m_capacity = std::min(std::max(capacity_frames, 1u), kMaxCapacity);
}

void InputHistoryBuffer::clear() {
    m_slots = {};
    m_capacity = 0;
    m_oldest_frame = 0;
    m_newest_frame = 0;
    m_has_any_frame = false;
}

bool InputHistoryBuffer::has_frame(u32 frame) const {
    return slot_(frame) != nullptr;
}

u32 InputHistoryBuffer::stored_frame_count() const {
    if (!m_has_any_frame) {
        return 0;
    }
    return m_newest_frame - m_oldest_frame + 1;
}

void InputHistoryBuffer::push_frame(u32 frame, const PlayerInput& predicted) {
    store_predicted(frame, predicted);
}

std::optional<InputHistoryFrame> InputHistoryBuffer::pop_oldest() {
    if (!m_has_any_frame || m_capacity == 0) {
        return std::nullopt;
    }

    const u32 frame = m_oldest_frame;
    InputHistoryFrame out{};
    out.frame = frame;

    const InputSlot* slot = slot_(frame);
    if (slot != nullptr) {
        out.has_predicted = slot->has_predicted;
        out.has_confirmed = slot->has_confirmed;
        out.predicted = slot->predicted;
        out.confirmed = slot->confirmed;
    }

    m_slots[frame % m_capacity] = {};

    if (m_oldest_frame == m_newest_frame) {
        m_has_any_frame = false;
        m_oldest_frame = 0;
        m_newest_frame = 0;
    } else {
        ++m_oldest_frame;
    }

    return out;
}

const InputHistoryBuffer::InputSlot* InputHistoryBuffer::slot_(u32 frame) const {
    if (m_capacity == 0) {
        return nullptr;
    }

    const u32 index = frame % m_capacity;
    const InputSlot& slot = m_slots[index];
    if (slot.frame != frame || (!slot.has_predicted && !slot.has_confirmed)) {
        return nullptr;
    }
    return &slot;
}

InputHistoryBuffer::InputSlot* InputHistoryBuffer::slot_mut_(u32 frame) {
    if (m_capacity == 0) {
        return nullptr;
    }

    const u32 index = frame % m_capacity;
    InputSlot& slot = m_slots[index];
    if (slot.frame != frame) {
        slot = {};
        slot.frame = frame;
    }
    return &slot;
}

void InputHistoryBuffer::touch_frame_(u32 frame) {
    if (!m_has_any_frame) {
        m_oldest_frame = frame;
        m_newest_frame = frame;
        m_has_any_frame = true;
        return;
    }

    if (frame < m_oldest_frame) {
        m_oldest_frame = frame;
    }
    if (frame > m_newest_frame) {
        m_newest_frame = frame;
    }

    while (m_newest_frame - m_oldest_frame + 1 > m_capacity) {
        ++m_oldest_frame;
    }
}

void InputHistoryBuffer::store_predicted(u32 frame, const PlayerInput& input) {
    InputSlot* slot = slot_mut_(frame);
    if (slot == nullptr) {
        return;
    }

    slot->predicted = input;
    slot->predicted.frame = frame;
    slot->has_predicted = true;
    touch_frame_(frame);
}

void InputHistoryBuffer::store_confirmed(u32 frame, const PlayerInput& input) {
    InputSlot* slot = slot_mut_(frame);
    if (slot == nullptr) {
        return;
    }

    slot->confirmed = input;
    slot->confirmed.frame = frame;
    slot->has_confirmed = true;
    touch_frame_(frame);
}

bool InputHistoryBuffer::has_predicted(u32 frame) const {
    const InputSlot* slot = slot_(frame);
    return slot != nullptr && slot->has_predicted;
}

bool InputHistoryBuffer::has_confirmed(u32 frame) const {
    const InputSlot* slot = slot_(frame);
    return slot != nullptr && slot->has_confirmed;
}

PlayerInput InputHistoryBuffer::predicted(u32 frame) const {
    const InputSlot* slot = slot_(frame);
    if (slot == nullptr || !slot->has_predicted) {
        return {};
    }
    return slot->predicted;
}

PlayerInput InputHistoryBuffer::confirmed(u32 frame) const {
    const InputSlot* slot = slot_(frame);
    if (slot == nullptr || !slot->has_confirmed) {
        return {};
    }
    return slot->confirmed;
}

bool InputHistoryBuffer::prediction_matches(u32 frame) const {
    const InputSlot* slot = slot_(frame);
    if (slot == nullptr || !slot->has_predicted || !slot->has_confirmed) {
        return false;
    }
    return inputs_equal(slot->predicted, slot->confirmed);
}

ReconcileResult InputHistoryBuffer::reconcile_authoritative(u32 frame, const PlayerInput& authoritative) {
    return reconcile_predicted_input(*this, frame, authoritative);
}

} // namespace fuse::net
