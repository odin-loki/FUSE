#pragma once

// Ordered waypoint path. advance_if_close steps to the next point when the
// position is within radius of the active one and does not wrap. finished
// becomes true only after the last point has been reached.

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <vector>

namespace fuse::gameplay {

class WaypointPath {
public:
    void add(fuse::math::Vec3 point) { m_points.push_back(point); }

    void clear() {
        m_points.clear();
        m_index = 0;
        m_finished = false;
    }

    [[nodiscard]] std::size_t count() const { return m_points.size(); }

    /// Active waypoint, or the zero vector when the path is empty.
    [[nodiscard]] fuse::math::Vec3 current() const {
        if (m_index >= m_points.size()) {
            return {};
        }
        return m_points[m_index];
    }

    /// Moves to the next waypoint when distance to the current one is <= radius.
    /// Returns true only when the index advances. On the last point a close
    /// position sets finished and returns false. radius <= 0 never advances.
    [[nodiscard]] bool advance_if_close(fuse::math::Vec3 position, f32 radius) {
        if (!std::isfinite(radius) || radius <= 0.f || m_index >= m_points.size()) {
            return false;
        }
        const fuse::math::Vec3 delta = position - m_points[m_index];
        const f32 dist2 = delta.dot(delta);
        if (!std::isfinite(dist2) || dist2 > radius * radius) {
            return false;
        }
        if (m_index + 1 >= m_points.size()) {
            m_finished = true;
            return false;
        }
        ++m_index;
        return true;
    }

    /// True only after advance_if_close reaches the last point. Empty paths and
    /// a cursor sitting on the last point before that call are not finished.
    [[nodiscard]] bool finished() const { return m_finished; }

private:
    std::vector<fuse::math::Vec3> m_points;
    std::size_t m_index = 0;
    bool m_finished = false;
};

} // namespace fuse::gameplay
