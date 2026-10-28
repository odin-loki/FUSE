#include <fuse/mechanics/scale_component.hpp>

#include <cmath>

namespace fuse::mechanics {

namespace {

constexpr f32 kArriveEpsilon = 0.0001f;

f32 moveToward(f32 current, f32 target, f32 step, bool snap) {
    const f32 delta = target - current;
    const f32 distance = std::fabs(delta);
    if (snap || distance <= kArriveEpsilon) {
        return target;
    }
    if (step <= 0.f) {
        return current;
    }
    if (step >= distance) {
        return target;
    }
    return current + std::copysign(step, delta);
}

} // namespace

ScaleComponent::ScaleComponent() = default;

ScaleComponent::ScaleComponent(std::string name, f32 rate)
    : Component(std::move(name))
    , m_rate(rate) {}

void ScaleComponent::setUniform(bool uniform) {
    m_uniform = uniform;
    if (m_uniform) {
        m_y = m_x;
        m_z = m_x;
        m_targetY = m_targetX;
        m_targetZ = m_targetX;
    }
    syncArrived();
}

void ScaleComponent::setScale(f32 x, f32 y, f32 z) {
    m_x = x;
    m_y = m_uniform ? x : y;
    m_z = m_uniform ? x : z;
    syncArrived();
}

void ScaleComponent::setTarget(f32 x, f32 y, f32 z) {
    m_targetX = x;
    m_targetY = m_uniform ? x : y;
    m_targetZ = m_uniform ? x : z;
    syncArrived();
}

void ScaleComponent::tick(f32 dt) {
    const bool wasArrived = m_arrived;
    const f32 speed = std::fabs(m_rate);
    const bool zeroRate = speed == 0.f;
    const bool snap = zeroRate && m_snapWhenZero;
    const f32 step = (!zeroRate && dt > 0.f) ? speed * dt : 0.f;

    if (m_uniform) {
        m_x = moveToward(m_x, m_targetX, step, snap);
        m_y = m_x;
        m_z = m_x;
        m_targetY = m_targetX;
        m_targetZ = m_targetX;
    } else {
        m_x = moveToward(m_x, m_targetX, step, snap);
        m_y = moveToward(m_y, m_targetY, step, snap);
        m_z = moveToward(m_z, m_targetZ, step, snap);
    }

    m_arrived = reachedTarget();
    if (m_arrived && !wasArrived) {
        ++m_arriveCount;
        if (m_onArrive) {
            m_onArrive();
        }
    }
}

void ScaleComponent::syncArrived() {
    m_arrived = reachedTarget();
}

bool ScaleComponent::reachedTarget() const {
    return std::fabs(m_x - m_targetX) <= kArriveEpsilon
        && std::fabs(m_y - m_targetY) <= kArriveEpsilon
        && std::fabs(m_z - m_targetZ) <= kArriveEpsilon;
}

} // namespace fuse::mechanics
