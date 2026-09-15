#pragma once

#include <fuse/fx/fx_defs.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <vector>

namespace fuse::fx {

struct ResidualEntry {
    ResidualKind kind = ResidualKind::Zodiac;
    std::string assetId;
    float duration = 0.f;
    float fadeDuration = 0.f;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float elapsed = 0.f;
    bool expired = false;
};

/// Hook surface for spawning transient world FX — ore analogue: `afxResidueMgr`.
using ResidualSpawnHook = std::function<void(const ResidualEntry&)>;

class ResidualEffectQueue {
public:
    void setSpawnHook(ResidualSpawnHook hook) { m_spawnHook = std::move(hook); }

    void enqueue(const ResidualEntry& entry);
    void tick(float dt);

    u32 activeCount() const;
    u32 totalSpawned() const { return m_totalSpawned; }
    u32 totalExpired() const { return m_totalExpired; }

    const std::vector<ResidualEntry>& entries() const { return m_entries; }

private:
    std::vector<ResidualEntry> m_entries;
    ResidualSpawnHook m_spawnHook;
    u32 m_totalSpawned = 0;
    u32 m_totalExpired = 0;
};

} // namespace fuse::fx
