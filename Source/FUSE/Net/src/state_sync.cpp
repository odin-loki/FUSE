#include <fuse/net/state_sync.hpp>

#include <fuse/ecs/components/transform.hpp>

#include <cmath>

namespace fuse::net {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

f32 clamp01(f32 t) {
    if (t < 0.f) {
        return 0.f;
    }
    if (t > 1.f) {
        return 1.f;
    }
    return t;
}

} // namespace

void ClientInterpolator::receive_state(const EntityNetState& state) {
    if (!state.entity.valid()) {
        return;
    }

    StateBuffer& buffer = m_buffers[state.entity.index];
    if (!buffer.has_next) {
        buffer.next = state;
        buffer.has_next = true;
        if (!buffer.has_prev) {
            buffer.prev = state;
            buffer.has_prev = true;
        }
        return;
    }

    if (state.sequence >= buffer.next.sequence) {
        buffer.prev = buffer.next;
        buffer.has_prev = true;
        buffer.next = state;
        buffer.has_next = true;
    } else if (state.sequence > buffer.prev.sequence) {
        buffer.prev = state;
        buffer.has_prev = true;
    }
}

void ClientInterpolator::update(ecs::Registry& registry, u64 current_time_us) {
    const u64 delay_us = static_cast<u64>(m_interpolation_delay_ms) * 1000ull;
    const u64 render_time = current_time_us > delay_us ? current_time_us - delay_us : 0ull;

    for (auto& [entity_index, buffer] : m_buffers) {
        if (!buffer.has_prev || !buffer.has_next) {
            continue;
        }

        const u64 span = buffer.next.timestamp > buffer.prev.timestamp
                               ? buffer.next.timestamp - buffer.prev.timestamp
                               : 1ull;
        f32 alpha = 0.f;
        if (render_time <= buffer.prev.timestamp) {
            alpha = 0.f;
        } else if (render_time >= buffer.next.timestamp) {
            alpha = 1.f;
        } else {
            alpha = static_cast<f32>(render_time - buffer.prev.timestamp) / static_cast<f32>(span);
        }
        alpha = clamp01(alpha);

        ecs::EntityID id = buffer.next.entity;
        if (id.index != entity_index) {
            id.index = entity_index;
        }

        if (!registry.alive(id)) {
            continue;
        }

        ecs::Transform* transform = registry.get<ecs::Transform>(id);
        if (transform == nullptr) {
            continue;
        }

        transform->position = lerp_vec3_(buffer.prev.position, buffer.next.position, alpha);
        transform->rotation = slerp_quat_(buffer.prev.rotation, buffer.next.rotation, alpha);
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
        return {lerp_f32_(a.x, end.x, t), lerp_f32_(a.y, end.y, t), lerp_f32_(a.z, end.z, t),
                lerp_f32_(a.w, end.w, t)};
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
