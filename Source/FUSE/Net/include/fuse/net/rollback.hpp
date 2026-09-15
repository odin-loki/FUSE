#pragma once

#include <fuse/ecs/registry.hpp>
#include <fuse/net/game_state.hpp>
#include <fuse/net/input_history.hpp>
#include <fuse/net/rollback_buffer.hpp>
#include <fuse/types.hpp>

namespace fuse::net {

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
    [[nodiscard]] const RollbackBuffer& buffer() const { return m_buffer; }
    [[nodiscard]] const InputHistoryBuffer& input_history() const { return m_input_history; }

private:
    static constexpr u32 kMaxFrames = 64;
    static constexpr u32 kInputHistoryCapacity = 128;

    RollbackBuffer m_buffer;
    InputHistoryBuffer m_input_history;
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
};

} // namespace fuse::net
