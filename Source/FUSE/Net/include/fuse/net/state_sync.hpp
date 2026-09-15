#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/types.hpp>

#include <unordered_map>

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

/// Client-side interpolation between received authoritative states (B7.4).
class ClientInterpolator {
public:
    void set_interpolation_delay_ms(u32 delay_ms) { m_interpolation_delay_ms = delay_ms; }
    [[nodiscard]] u32 interpolation_delay_ms() const { return m_interpolation_delay_ms; }

    void receive_state(const EntityNetState& state);
    void update(ecs::Registry& registry, u64 current_time_us);

private:
    struct StateBuffer {
        EntityNetState prev{};
        EntityNetState next{};
        bool has_prev = false;
        bool has_next = false;
    };

    std::unordered_map<u32, StateBuffer> m_buffers;
    u32 m_interpolation_delay_ms = 100;

    static f32 lerp_f32_(f32 a, f32 b, f32 t);
    static ecs::vec3 lerp_vec3_(const ecs::vec3& a, const ecs::vec3& b, f32 t);
    static ecs::quat slerp_quat_(const ecs::quat& a, const ecs::quat& b, f32 t);
};

} // namespace fuse::net
