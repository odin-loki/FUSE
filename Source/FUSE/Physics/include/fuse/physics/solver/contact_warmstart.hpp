#pragma once

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::physics {

/// Order-independent cache key for a contact warm start. The smaller body id is packed first, so
/// swapping the body arguments does not change the key. `feature` contributes only its low 21 bits.
[[nodiscard]] inline u64 contact_key(u32 bodyA, u32 bodyB, u32 feature) {
    const u32 minId = bodyA < bodyB ? bodyA : bodyB;
    const u32 maxId = bodyA < bodyB ? bodyB : bodyA;
    return (u64(minId) << 42) ^ (u64(maxId) << 21) ^ u64(feature & 0x1fffffu);
}

struct WarmStart {
    u64 key = 0;
    f32 normalImpulse = 0.f;
    f32 tangentImpulse = 0.f;
};

/// First cached impulse pair with `key`, or nullptr when `entries` is null, `count` is 0, or no
/// entry matches.
[[nodiscard]] inline const WarmStart* find_warm_start(const WarmStart* entries, std::size_t count, u64 key) {
    if (entries == nullptr || count == 0) {
        return nullptr;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (entries[i].key == key) {
            return &entries[i];
        }
    }
    return nullptr;
}

} // namespace fuse::physics
