#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/net/snapshot_delta.hpp>
#include <fuse/types.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::net {

/// Authoritative entity state for non-rollback replication.
struct EntityNetState {
    ecs::EntityID entity = ecs::EntityID::null();
    ecs::vec3 position{};
    ecs::quat rotation{};
    ecs::vec3 linear_velocity{};
    u64 timestamp = 0;
    u32 sequence = 0;
};

enum class EntityStateField : u8 {
    Position = 1 << 0,
    Rotation = 1 << 1,
    LinearVelocity = 1 << 2,
};

/// Partial entity update for bandwidth-conscious authoritative sync.
struct EntityStateDelta {
    ecs::EntityID entity = ecs::EntityID::null();
    u8 changed_fields = 0;
    ecs::vec3 position{};
    ecs::quat rotation{};
    ecs::vec3 linear_velocity{};
    u32 sequence = 0;
    u64 timestamp = 0;
};

/// Bundled authoritative snapshot used by delta broadcasters.
struct StateSyncSnapshot {
    u32 frame = 0;
    u64 timestamp_us = 0;
    std::vector<EntityNetState> entities;
};

/// Computes entity/state deltas between authoritative snapshots (B7.4 stub).
class StateSyncDeltaBroadcaster {
public:
    void set_base_snapshot(StateSyncSnapshot snapshot);
    [[nodiscard]] SnapshotDelta compute_world_delta(const StateSyncSnapshot& target) const;
    [[nodiscard]] EntityStateDelta compute_entity_delta(const EntityNetState& base,
                                                        const EntityNetState& target) const;
    [[nodiscard]] EntityNetState apply_entity_delta(const EntityNetState& base,
                                                    const EntityStateDelta& delta) const;

private:
    StateSyncSnapshot m_base{};
    bool m_has_base = false;
};

/// Client-side interpolation between received authoritative states (B7.4).
///
/// Keeps a per-entity jitter buffer ordered by server timestamp and renders at
/// `current_time - interpolation_delay`, interpolating between the two buffered states that
/// bracket the render time. Late, duplicated or reordered states are merged into the timeline,
/// so the rendered position is continuous regardless of snapshot rate vs. delay.
class ClientInterpolator {
public:
    static constexpr usize kMaxBufferedStates = 32;

    void set_interpolation_delay_ms(u32 delay_ms) { m_interpolation_delay_ms = delay_ms; }
    [[nodiscard]] u32 interpolation_delay_ms() const { return m_interpolation_delay_ms; }

    void receive_state(const EntityNetState& state);
    void update(ecs::Registry& registry, u64 current_time_us);

    /// Number of states currently buffered for `entity` (0 when unknown).
    [[nodiscard]] usize buffered_state_count(ecs::EntityID entity) const;

private:
    struct StateBuffer {
        std::vector<EntityNetState> states; ///< Sorted by (timestamp, sequence).
        ecs::EntityID entity = ecs::EntityID::null();
    };

    std::unordered_map<u32, StateBuffer> m_buffers;
    u32 m_interpolation_delay_ms = 100;

    static f32 lerp_f32_(f32 a, f32 b, f32 t);
    static ecs::vec3 lerp_vec3_(const ecs::vec3& a, const ecs::vec3& b, f32 t);
    static ecs::quat slerp_quat_(const ecs::quat& a, const ecs::quat& b, f32 t);
};

} // namespace fuse::net
