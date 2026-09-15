#pragma once

#include <fuse/ecs/registry.hpp>
#include <fuse/net/transport.hpp>
#include <fuse/types.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace fuse::net {

/// Snapshot of deterministic simulation state at a given frame.
struct GameSnapshot {
    u32 frame = 0;
    std::vector<byte> physics_state;
    std::vector<byte> ecs_state;
    u64 checksum = 0;
};

struct PlayerInput {
    u32 frame = 0;
    u32 player_id = 0;
    u32 buttons = 0;
    std::int16_t axis_lx = 0;
    std::int16_t axis_ly = 0;
    std::int16_t axis_rx = 0;
    std::int16_t axis_ry = 0;
};

/// GGPO-style rollback manager bound to an ECS registry (B7.4).
class RollbackManager {
public:
    void init(u32 max_rollback_frames = 8);
    void destroy();

    void bind_registry(ecs::Registry* registry);

    void save_snapshot(u32 frame);
    bool apply_remote_input(const PlayerInput& input);
    void set_local_input(const PlayerInput& input);

    /// Advance one frame — applies confirmed inputs and integrates simulation.
    void tick(f32 dt);

    [[nodiscard]] u32 current_frame() const { return m_current_frame; }
    [[nodiscard]] u32 confirmed_frame() const { return m_confirmed_frame; }
    [[nodiscard]] bool is_rolling_back() const { return m_rolling_back; }

private:
    static constexpr u32 kMaxFrames = 64;

    std::array<GameSnapshot, kMaxFrames> m_snapshots{};
    std::array<PlayerInput, kMaxFrames> m_local_inputs{};
    std::array<PlayerInput, kMaxFrames> m_remote_inputs{};
    std::array<bool, kMaxFrames> m_remote_confirmed{};

    ecs::Registry* m_registry = nullptr;
    u32 m_current_frame = 0;
    u32 m_confirmed_frame = 0;
    u32 m_max_rollback = 8;
    f32 m_last_dt = 1.f / 60.f;
    bool m_rolling_back = false;

    void capture_registry_state_(GameSnapshot& snapshot) const;
    void restore_registry_state_(const GameSnapshot& snapshot) const;
    void integrate_frame_(u32 frame, f32 dt);
    void rollback_to_(u32 frame);
    void resimulate_to_(u32 target_frame, f32 dt);
    [[nodiscard]] u32 frame_slot_(u32 frame) const { return frame % kMaxFrames; }
    [[nodiscard]] u64 compute_checksum_(const GameSnapshot& snapshot) const;
};

} // namespace fuse::net
