#include <fuse/fx/residual_effects.hpp>

namespace fuse::fx {

void ResidualEffectQueue::enqueue(const ResidualEntry& entry) {
    m_entries.push_back(entry);
    ++m_totalSpawned;
    if (m_spawnHook) {
        m_spawnHook(entry);
    }
}

void ResidualEffectQueue::tick(float dt) {
    for (ResidualEntry& entry : m_entries) {
        if (entry.expired) {
            continue;
        }

        entry.elapsed += dt;
        const float totalLifetime = entry.duration + entry.fadeDuration;
        if (entry.elapsed >= totalLifetime) {
            entry.expired = true;
            ++m_totalExpired;
        }
    }
}

u32 ResidualEffectQueue::activeCount() const {
    u32 count = 0;
    for (const ResidualEntry& entry : m_entries) {
        if (!entry.expired) {
            ++count;
        }
    }
    return count;
}

} // namespace fuse::fx
