#include <fuse/net/state_sync.hpp>

#include <fuse/ecs/components/transform.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace fuse::net {

namespace {

f32 clamp01(f32 t) {
    if (t < 0.f) {
        return 0.f;
    }
    if (t > 1.f) {
        return 1.f;
    }
    return t;
}

GameSnapshot make_game_snapshot_from_sync(const StateSyncSnapshot& sync_snapshot) {
    GameSnapshot snapshot;
    snapshot.frame = sync_snapshot.frame;

    NetSerializer ecs_writer;
    NetSerializer physics_writer;
    for (const EntityNetState& entity : sync_snapshot.entities) {
        ecs_writer.write_u32(entity.entity.index);
        ecs_writer.write_u32(entity.entity.generation);
        ecs_writer.write_vec3(entity.position);
        ecs_writer.write_quat(entity.rotation);
        ecs_writer.write_vec3({1.f, 1.f, 1.f, 0.f});

        physics_writer.write_u32(entity.entity.index);
        physics_writer.write_u32(entity.entity.generation);
        physics_writer.write_vec3(entity.linear_velocity);
        physics_writer.write_vec3({0.f, 0.f, 0.f, 0.f});
        physics_writer.write_f32(1.f);
    }

    snapshot.ecs_state = std::move(ecs_writer.buffer);
    snapshot.physics_state = std::move(physics_writer.buffer);
    snapshot.checksum = 0;
    return snapshot;
}

} // namespace

void StateSyncDeltaBroadcaster::set_base_snapshot(StateSyncSnapshot snapshot) {
    m_base = std::move(snapshot);
    m_has_base = true;
}

SnapshotDelta StateSyncDeltaBroadcaster::compute_world_delta(const StateSyncSnapshot& target) const {
    if (!m_has_base) {
        SnapshotDelta delta;
        delta.base_frame = target.frame;
        delta.target_frame = target.frame;
        delta.kind = SnapshotDeltaKind::Full;
        const GameSnapshot target_snapshot = make_game_snapshot_from_sync(target);
        delta.full_ecs_state = target_snapshot.ecs_state;
        delta.full_physics_state = target_snapshot.physics_state;
        return delta;
    }

    const GameSnapshot base_snapshot = make_game_snapshot_from_sync(m_base);
    const GameSnapshot target_snapshot = make_game_snapshot_from_sync(target);
    return compute_snapshot_delta(base_snapshot, target_snapshot);
}

EntityStateDelta StateSyncDeltaBroadcaster::compute_entity_delta(const EntityNetState& base,
                                                                 const EntityNetState& target) const {
    EntityStateDelta delta;
    delta.entity = target.entity;
    delta.sequence = target.sequence;
    delta.timestamp = target.timestamp;

    if (base.position.x != target.position.x || base.position.y != target.position.y ||
        base.position.z != target.position.z) {
        delta.changed_fields |= static_cast<u8>(EntityStateField::Position);
        delta.position = target.position;
    }
    if (base.rotation.x != target.rotation.x || base.rotation.y != target.rotation.y ||
        base.rotation.z != target.rotation.z || base.rotation.w != target.rotation.w) {
        delta.changed_fields |= static_cast<u8>(EntityStateField::Rotation);
        delta.rotation = target.rotation;
    }
    if (base.linear_velocity.x != target.linear_velocity.x || base.linear_velocity.y != target.linear_velocity.y ||
        base.linear_velocity.z != target.linear_velocity.z) {
        delta.changed_fields |= static_cast<u8>(EntityStateField::LinearVelocity);
        delta.linear_velocity = target.linear_velocity;
    }

    return delta;
}

EntityNetState StateSyncDeltaBroadcaster::apply_entity_delta(const EntityNetState& base,
                                                             const EntityStateDelta& delta) const {
    EntityNetState result = base;
    result.sequence = delta.sequence;
    result.timestamp = delta.timestamp;

    if ((delta.changed_fields & static_cast<u8>(EntityStateField::Position)) != 0) {
        result.position = delta.position;
    }
    if ((delta.changed_fields & static_cast<u8>(EntityStateField::Rotation)) != 0) {
        result.rotation = delta.rotation;
    }
    if ((delta.changed_fields & static_cast<u8>(EntityStateField::LinearVelocity)) != 0) {
        result.linear_velocity = delta.linear_velocity;
    }

    return result;
}

void ClientInterpolator::receive_state(const EntityNetState& state) {
    if (!state.entity.valid()) {
        return;
    }

    StateBuffer& buffer = m_buffers[state.entity.index];
    if (buffer.entity.generation != state.entity.generation && !buffer.states.empty() &&
        state.entity.generation > buffer.entity.generation) {
        buffer.states.clear(); // Slot reused by a newer entity generation.
    } else if (!buffer.states.empty() && state.entity.generation < buffer.entity.generation) {
        return; // Stale state for a destroyed generation.
    }
    buffer.entity = state.entity;

    std::vector<EntityNetState>& states = buffer.states;
    const auto order = [](const EntityNetState& a, const EntityNetState& b) {
        if (a.timestamp != b.timestamp) {
            return a.timestamp < b.timestamp;
        }
        return a.sequence < b.sequence;
    };

    for (const EntityNetState& existing : states) {
        if (existing.sequence == state.sequence && existing.timestamp == state.timestamp) {
            return; // Duplicate delivery.
        }
    }

    // Older than everything retained once the buffer is full: it can no longer be rendered.
    if (states.size() >= kMaxBufferedStates && order(state, states.front())) {
        return;
    }

    states.insert(std::upper_bound(states.begin(), states.end(), state, order), state);
    if (states.size() > kMaxBufferedStates) {
        states.erase(states.begin());
    }
}

usize ClientInterpolator::buffered_state_count(ecs::EntityID entity) const {
    const auto it = m_buffers.find(entity.index);
    if (it == m_buffers.end() || it->second.entity.generation != entity.generation) {
        return 0;
    }
    return it->second.states.size();
}

void ClientInterpolator::update(ecs::Registry& registry, u64 current_time_us) {
    const u64 delay_us = static_cast<u64>(m_interpolation_delay_ms) * 1000ull;
    const u64 render_time = current_time_us > delay_us ? current_time_us - delay_us : 0ull;

    for (auto& [entity_index, buffer] : m_buffers) {
        std::vector<EntityNetState>& states = buffer.states;
        if (states.empty()) {
            continue;
        }

        // First state strictly after render time; its predecessor is the lower bracket.
        const auto upper = std::upper_bound(states.begin(), states.end(), render_time,
                                            [](u64 t, const EntityNetState& s) { return t < s.timestamp; });

        ecs::vec3 position{};
        ecs::quat rotation{};
        if (upper == states.begin()) {
            position = states.front().position; // Render time precedes all data — hold first.
            rotation = states.front().rotation;
        } else if (upper == states.end()) {
            position = states.back().position; // Starved — hold last (no extrapolation).
            rotation = states.back().rotation;
        } else {
            const EntityNetState& prev = *(upper - 1);
            const EntityNetState& next = *upper;
            const u64 span = next.timestamp - prev.timestamp; // > 0: upper is strictly later.
            const f32 alpha =
                clamp01(static_cast<f32>(static_cast<double>(render_time - prev.timestamp) /
                                         static_cast<double>(span)));
            position = lerp_vec3_(prev.position, next.position, alpha);
            rotation = slerp_quat_(prev.rotation, next.rotation, alpha);
        }

        // Drop states that can never bracket a future render time (keep the lower bracket).
        if (upper != states.begin()) {
            const auto keep_from = upper - 1;
            states.erase(states.begin(), keep_from);
        }

        ecs::EntityID id = buffer.entity;
        id.index = entity_index;
        if (!registry.alive(id)) {
            continue;
        }

        ecs::Transform* transform = registry.get<ecs::Transform>(id);
        if (transform == nullptr) {
            continue;
        }

        transform->position.x = position.x;
        transform->position.y = position.y;
        transform->position.z = position.z;
        transform->rotation = rotation;
        transform->dirty = true;
    }
}

f32 ClientInterpolator::lerp_f32_(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

ecs::vec3 ClientInterpolator::lerp_vec3_(const ecs::vec3& a, const ecs::vec3& b, f32 t) {
    return {lerp_f32_(a.x, b.x, t), lerp_f32_(a.y, b.y, t), lerp_f32_(a.z, b.z, t), 1.f};
}

ecs::quat ClientInterpolator::slerp_quat_(const ecs::quat& a, const ecs::quat& b, f32 t) {
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    ecs::quat end = b;
    if (dot < 0.f) {
        end.x = -end.x;
        end.y = -end.y;
        end.z = -end.z;
        end.w = -end.w;
        dot = -dot;
    }

    if (dot > 0.9995f) {
        // Nearly parallel: normalised lerp (a raw lerp would shrink the quaternion off unit length).
        ecs::quat q{lerp_f32_(a.x, end.x, t), lerp_f32_(a.y, end.y, t), lerp_f32_(a.z, end.z, t),
                    lerp_f32_(a.w, end.w, t)};
        const f32 length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (length > 1e-12f) {
            q.x /= length;
            q.y /= length;
            q.z /= length;
            q.w /= length;
        }
        return q;
    }

    const f32 theta = std::acos(clamp01(dot));
    const f32 sin_theta = std::sin(theta);
    if (std::fabs(sin_theta) < 1e-6f) {
        return a;
    }

    const f32 wa = std::sin((1.f - t) * theta) / sin_theta;
    const f32 wb = std::sin(t * theta) / sin_theta;
    return {wa * a.x + wb * end.x, wa * a.y + wb * end.y, wa * a.z + wb * end.z, wa * a.w + wb * end.w};
}

} // namespace fuse::net
