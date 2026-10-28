#pragma once

// Ore: ore/torque-addon/GMK/Engine/source/component/scaleComponent.h

#include <fuse/mechanics/component.hpp>
#include <fuse/types.hpp>

#include <functional>

namespace fuse::mechanics {

/// Scale-to-target leaf (GMK ScaleComponent without SimObject/Con::).
/// Uniform uses one scale. XYZ moves each axis toward its target at the same rate
/// (units per second). Arrival fires the optional callback once per crossing.
class ScaleComponent : public Component {
public:
    ScaleComponent();
    explicit ScaleComponent(std::string name, f32 rate = 1.f);

    const char* typeName() const override { return "ScaleComponent"; }

    f32 rate() const { return m_rate; }
    void setRate(f32 rate) { m_rate = rate; }

    bool uniform() const { return m_uniform; }
    void setUniform(bool uniform);

    bool snapWhenZero() const { return m_snapWhenZero; }
    void setSnapWhenZero(bool snap) { m_snapWhenZero = snap; }

    f32 x() const { return m_x; }
    f32 y() const { return m_y; }
    f32 z() const { return m_z; }

    f32 targetX() const { return m_targetX; }
    f32 targetY() const { return m_targetY; }
    f32 targetZ() const { return m_targetZ; }

    void setScale(f32 x, f32 y, f32 z);
    void setScale(f32 uniformScale) { setScale(uniformScale, uniformScale, uniformScale); }

    void setTarget(f32 x, f32 y, f32 z);
    void setTarget(f32 uniformTarget) { setTarget(uniformTarget, uniformTarget, uniformTarget); }

    void setOnArrive(std::function<void()> callback) { m_onArrive = std::move(callback); }

    /// Move toward the target. Zero rate snaps onto the target when snapWhenZero is set.
    void tick(f32 dt);

    bool arrived() const { return m_arrived; }
    u32 arriveCount() const { return m_arriveCount; }

private:
    void syncArrived();
    bool reachedTarget() const;

    f32 m_rate = 1.f;
    bool m_uniform = true;
    bool m_snapWhenZero = false;
    f32 m_x = 1.f;
    f32 m_y = 1.f;
    f32 m_z = 1.f;
    f32 m_targetX = 1.f;
    f32 m_targetY = 1.f;
    f32 m_targetZ = 1.f;
    bool m_arrived = true;
    u32 m_arriveCount = 0;
    std::function<void()> m_onArrive;
};

} // namespace fuse::mechanics
