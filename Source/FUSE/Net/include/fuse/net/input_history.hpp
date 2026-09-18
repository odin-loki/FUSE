#pragma once

#include <fuse/net/game_state.hpp>
#include <fuse/types.hpp>

#include <array>
#include <optional>

namespace fuse::net {

struct ReconcileResult;
struct InputReconcilePreflight;
enum class ReconcileAction : u8;

/// One frame of predicted and/or confirmed local input retained in the history ring.
struct InputHistoryFrame {
    u32 frame = 0;
    PlayerInput predicted{};
    PlayerInput confirmed{};
    bool has_predicted = false;
    bool has_confirmed = false;
};

/// Ring buffer of predicted and confirmed player inputs — wider than snapshot history (B7.4).
class InputHistoryBuffer {
public:
    void init(u32 capacity_frames);
    void clear();

    [[nodiscard]] u32 capacity() const { return m_capacity; }
    [[nodiscard]] u32 oldest_stored_frame() const { return m_oldest_frame; }
    [[nodiscard]] u32 newest_stored_frame() const { return m_newest_frame; }
    [[nodiscard]] u32 stored_frame_count() const;
    [[nodiscard]] u32 remaining_capacity() const;
    [[nodiscard]] bool empty() const { return stored_frame_count() == 0; }
    [[nodiscard]] bool has_frame(u32 frame) const;

    /// Push a predicted local input for `frame` (ring may evict the oldest retained frame).
    void push_frame(u32 frame, const PlayerInput& predicted);
    /// Remove and return the oldest retained frame; empty when the ring has no frames.
    [[nodiscard]] std::optional<InputHistoryFrame> pop_oldest();

    void store_predicted(u32 frame, const PlayerInput& input);
    void store_confirmed(u32 frame, const PlayerInput& input);

    [[nodiscard]] bool has_predicted(u32 frame) const;
    [[nodiscard]] bool has_confirmed(u32 frame) const;
    [[nodiscard]] PlayerInput predicted(u32 frame) const;
    [[nodiscard]] PlayerInput confirmed(u32 frame) const;

    /// True when both predicted and confirmed slots exist and `inputs_equal` returns true.
    [[nodiscard]] bool prediction_matches(u32 frame) const;

    /// Preflight reconcile for `frame` without mutating the ring (B7.4 deepen follow-up).
    [[nodiscard]] InputReconcilePreflight preflight_reconcile(u32 frame) const;

    /// True when reconcile would be rejected before recording (B7.4 deepen follow-up).
    [[nodiscard]] bool should_skip_reconcile(u32 frame) const;

    /// Record authoritative input and compare against the predicted local history entry.
    [[nodiscard]] ReconcileResult reconcile_authoritative(u32 frame, const PlayerInput& authoritative);

private:
    struct InputSlot {
        u32 frame = 0;
        PlayerInput predicted{};
        PlayerInput confirmed{};
        bool has_predicted = false;
        bool has_confirmed = false;
    };

    static constexpr u32 kMaxCapacity = 128;

    std::array<InputSlot, kMaxCapacity> m_slots{};
    u32 m_capacity = 0;
    u32 m_oldest_frame = 0;
    u32 m_newest_frame = 0;
    bool m_has_any_frame = false;

    [[nodiscard]] const InputSlot* slot_(u32 frame) const;
    [[nodiscard]] InputSlot* slot_mut_(u32 frame);
    void touch_frame_(u32 frame);
};

/// Bitwise equality over all `PlayerInput` payload fields (frame id excluded).
[[nodiscard]] bool inputs_equal(const PlayerInput& a, const PlayerInput& b);

} // namespace fuse::net
