#include <fuse/net/rollback.hpp>

#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/net/checksum.hpp>
#include <fuse/net/reconcile.hpp>
#include <fuse/net/serializer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::net {

namespace {

f32 axis_to_float(std::int16_t axis) {
    return static_cast<f32>(axis) / 32767.f;
}

} // namespace

void RollbackManager::init(u32 max_rollback_frames) {
    destroy();
    m_max_rollback = std::min(max_rollback_frames, kMaxFrames - 1u);
    m_buffer.init(kMaxFrames);
    m_input_history.init(kInputHistoryCapacity);
    m_current_frame = 0;
    m_confirmed_frame = 0;
    m_rolling_back = false;
    m_last_dt = 1.f / 60.f;
}

void RollbackManager::destroy() {
    m_registry = nullptr;
    m_current_frame = 0;
    m_confirmed_frame = 0;
    m_rolling_back = false;
    m_last_dt = 1.f / 60.f;
    m_buffer.clear();
    m_input_history.clear();
}

void RollbackManager::bind_registry(ecs::Registry* registry) { m_registry = registry; }

void RollbackManager::capture_registry_state_(GameSnapshot& snapshot) const {
    snapshot.physics_state.clear();
    snapshot.ecs_state.clear();

    if (m_registry == nullptr) {
        return;
    }

    NetSerializer ecs_writer;
    NetSerializer physics_writer;

    m_registry->each<ecs::Transform, ecs::RigidBody>([&](ecs::EntityID entity, ecs::Transform& transform,
                                                         ecs::RigidBody& body) {
        ecs_writer.write_u32(entity.index);
        ecs_writer.write_u32(entity.generation);
        ecs_writer.write_vec3(transform.position);
        ecs_writer.write_quat(transform.rotation);
        ecs_writer.write_vec3(transform.scale);

        physics_writer.write_u32(entity.index);
        physics_writer.write_u32(entity.generation);
        physics_writer.write_vec3(body.velocity);
        physics_writer.write_vec3(body.angular_velocity);
        physics_writer.write_f32(body.mass);
    });

    snapshot.ecs_state = std::move(ecs_writer.buffer);
    snapshot.physics_state = std::move(physics_writer.buffer);
    snapshot.checksum = compute_snapshot_checksum(snapshot);
}

void RollbackManager::restore_registry_state_(const GameSnapshot& snapshot) const {
    if (m_registry == nullptr) {
        return;
    }

    NetSerializer ecs_reader;
    ecs_reader.buffer = snapshot.ecs_state;
    ecs_reader.reset_read();

    NetSerializer physics_reader;
    physics_reader.buffer = snapshot.physics_state;
    physics_reader.reset_read();

    while (!ecs_reader.read_complete()) {
        const u32 index = ecs_reader.read_u32();
        const u32 generation = ecs_reader.read_u32();
        ecs::EntityID id{index, generation};
        if (!m_registry->alive(id)) {
            (void)ecs_reader.read_vec3();
            (void)ecs_reader.read_quat();
            (void)ecs_reader.read_vec3();
            continue;
        }

        ecs::Transform* transform = m_registry->get<ecs::Transform>(id);
        if (transform == nullptr) {
            (void)ecs_reader.read_vec3();
            (void)ecs_reader.read_quat();
            (void)ecs_reader.read_vec3();
            continue;
        }

        transform->position = ecs_reader.read_vec3();
        transform->rotation = ecs_reader.read_quat();
        transform->scale = ecs_reader.read_vec3();
        transform->dirty = true;
    }

    while (!physics_reader.read_complete()) {
        const u32 index = physics_reader.read_u32();
        const u32 generation = physics_reader.read_u32();
        ecs::EntityID id{index, generation};
        if (!m_registry->alive(id)) {
            (void)physics_reader.read_vec3();
            (void)physics_reader.read_vec3();
            (void)physics_reader.read_f32();
            continue;
        }

        ecs::RigidBody* body = m_registry->get<ecs::RigidBody>(id);
        if (body == nullptr) {
            (void)physics_reader.read_vec3();
            (void)physics_reader.read_vec3();
            (void)physics_reader.read_f32();
            continue;
        }

        body->velocity = physics_reader.read_vec3();
        body->angular_velocity = physics_reader.read_vec3();
        body->mass = physics_reader.read_f32();
        body->inv_mass = body->mass > 0.f ? 1.f / body->mass : 0.f;
    }
}

void RollbackManager::integrate_frame_(u32 frame, f32 dt) {
    if (m_registry == nullptr) {
        return;
    }

    const PlayerInput input =
        m_buffer.remote_confirmed(frame) ? m_buffer.remote_input(frame) : m_buffer.local_input(frame);
    const f32 dx = axis_to_float(input.axis_lx) * dt;
    const f32 dy = axis_to_float(input.axis_ly) * dt;

    m_registry->each<ecs::Transform>([&](ecs::EntityID /*entity*/, ecs::Transform& transform) {
        transform.position.x += dx;
        transform.position.y += dy;
        transform.dirty = true;
    });

    m_registry->each<ecs::RigidBody>([&](ecs::EntityID /*entity*/, ecs::RigidBody& body) {
        if (!body.is_static) {
            body.velocity.x += axis_to_float(input.axis_rx) * dt;
            body.velocity.y += axis_to_float(input.axis_ry) * dt;
        }
    });
}

void RollbackManager::save_snapshot(u32 frame) {
    GameSnapshot snapshot;
    snapshot.frame = frame;
    capture_registry_state_(snapshot);
    m_buffer.store_snapshot(frame, std::move(snapshot));
}

bool RollbackManager::apply_remote_input(const PlayerInput& input) {
    if (input.frame > m_current_frame) {
        return false;
    }

    (void)reconcile_predicted_input(m_input_history, input.frame, input);
    m_buffer.store_remote_input(input.frame, input, true);

    if (input.frame < m_current_frame) {
        if (m_current_frame - input.frame > m_max_rollback) {
            return false;
        }

        m_rolling_back = true;
        const u32 target_frame = m_current_frame;
        rollback_to_(input.frame);
        m_current_frame = input.frame;
        resimulate_to_(target_frame, m_last_dt);
        m_rolling_back = false;
        return true;
    }

    if (input.frame >= m_confirmed_frame) {
        m_confirmed_frame = input.frame + 1;
    }
    return false;
}

void RollbackManager::set_local_input(const PlayerInput& input) {
    m_buffer.store_local_input(input.frame, input);
    m_input_history.store_predicted(input.frame, input);
}

void RollbackManager::tick(f32 dt) {
    m_last_dt = dt;
    save_snapshot(m_current_frame);
    integrate_frame_(m_current_frame, dt);
    ++m_current_frame;
}

void RollbackManager::rollback_to_(u32 frame) {
    const GameSnapshot* snapshot = m_buffer.snapshot(frame);
    if (snapshot != nullptr) {
        restore_registry_state_(*snapshot);
    }
}

void RollbackManager::resimulate_to_(u32 target_frame, f32 dt) {
    while (m_current_frame < target_frame) {
        integrate_frame_(m_current_frame, dt);
        save_snapshot(m_current_frame);
        ++m_current_frame;
    }
}

} // namespace fuse::net
