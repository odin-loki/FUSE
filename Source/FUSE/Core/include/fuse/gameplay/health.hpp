#pragma once

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::gameplay {

struct HealthPool {
    f32 health = 0.f;
    f32 maxHealth = 0.f;
    f32 armor = 0.f;
};

/// Armor absorbs first. Returns the damage that actually landed on armor plus health.
[[nodiscard]] inline f32 apply_damage(HealthPool& pool, f32 amount) {
    if (!std::isfinite(amount) || amount <= 0.f) {
        return 0.f;
    }
    f32 left = amount;
    f32 dealt = 0.f;
    if (pool.armor > 0.f) {
        const f32 used = pool.armor < left ? pool.armor : left;
        pool.armor -= used;
        left -= used;
        dealt += used;
    }
    if (left > 0.f && pool.health > 0.f) {
        const f32 used = pool.health < left ? pool.health : left;
        pool.health -= used;
        dealt += used;
    }
    return dealt;
}

inline void heal(HealthPool& pool, f32 amount) {
    if (!std::isfinite(amount) || amount <= 0.f || pool.maxHealth <= 0.f) {
        return;
    }
    pool.health += amount;
    if (pool.health > pool.maxHealth) {
        pool.health = pool.maxHealth;
    }
}

[[nodiscard]] inline bool is_dead(const HealthPool& pool) {
    return pool.health <= 0.f;
}

} // namespace fuse::gameplay
