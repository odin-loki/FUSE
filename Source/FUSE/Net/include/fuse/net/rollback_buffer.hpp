#pragma once

#include <fuse/net/game_state.hpp>
#include <fuse/types.hpp>

#include <array>
#include <optional>

namespace fuse::net {

struct ReconcileResult;

/// Ring buffer of per-frame rollback data — snapshots plus confirmed inputs (B7.4).
class RollbackBuffer {
public:
    void init(u32 capacity_frames);
    void clear();

    [[nodiscard]] u32 capacity() const { return m_capacity; }
    [[nodiscard]] bool has_capacity() const { return m_capacity > 0; }
    [[nodiscard]] u32 oldest_stored_frame() const { return m_oldest_frame; }
    [[nodiscard]] u32 newest_stored_frame() const { return m_newest_frame; }
    [[nodiscard]] u32 stored_frame_count() const;
    [[nodiscard]] bool empty() const { return stored_frame_count() == 0; }
    [[nodiscard]] bool has_frame(u32 frame) const;
    [[nodiscard]] bool has_local_input(u32 frame) const;

    void store_snapshot(u32 frame, GameSnapshot snapshot);
    /// Evicts the oldest retained snapshot (no-op when empty).
    [[nodiscard]] std::optional<GameSnapshot> evict_oldest_snapshot();
    [[nodiscard]] const GameSnapshot* snapshot(u32 frame) const;
    [[nodiscard]] GameSnapshot* snapshot_mut(u32 frame);

    void store_local_input(u32 frame, const PlayerInput& input);
    void store_remote_input(u32 frame, const PlayerInput& input, bool confirmed);

    [[nodiscard]] PlayerInput local_input(u32 frame) const;
    [[nodiscard]] PlayerInput remote_input(u32 frame) const;
    [[nodiscard]] bool remote_confirmed(u32 frame) const;

    /// True when both local and remote inputs exist and `inputs_equal` returns true.
    [[nodiscard]] bool inputs_match(u32 frame) const;

    /// Record confirmed remote input and compare against the stored local prediction.
    [[nodiscard]] ReconcileResult reconcile_remote_input(u32 frame, const PlayerInput& remote);

private:
    struct FrameSlot {
        u32 frame = 0;
        GameSnapshot snapshot{};
        PlayerInput local_input{};
        PlayerInput remote_input{};
        bool has_snapshot = false;
        bool has_local_input = false;
        bool remote_confirmed = false;
    };

    static constexpr u32 kMaxCapacity = 64;

    std::array<FrameSlot, kMaxCapacity> m_slots{};
    u32 m_capacity = 0;
    u32 m_oldest_frame = 0;
    u32 m_newest_frame = 0;
    bool m_has_any_frame = false;

    [[nodiscard]] std::optional<u32> slot_index_(u32 frame) const;
};

} // namespace fuse::net
