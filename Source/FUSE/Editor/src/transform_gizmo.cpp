#include <fuse/editor/transform_gizmo.hpp>

#include <fuse/ecs/math/mat.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::editor {
namespace {

constexpr f32 kPi = 3.14159265358979f;

ecs::vec3 add(const ecs::vec3& a, const ecs::vec3& b, f32 s = 1.f) {
    return ecs::vec3{a.x + b.x * s, a.y + b.y * s, a.z + b.z * s, a.w};
}

// Direction through the matrix (uses the ECS convention via transform_point).
ecs::vec3 linear(const ecs::mat4& m, const ecs::vec3& v) {
    const ecs::vec3 a = ecs::transform_point(m, ecs::vec3{v.x, v.y, v.z, 1.f});
    const ecs::vec3 o = ecs::transform_point(m, ecs::vec3{0.f, 0.f, 0.f, 1.f});
    return ecs::vec3{a.x - o.x, a.y - o.y, a.z - o.z, 0.f};
}

ecs::vec3 normalized(const ecs::vec3& v) {
    const f32 l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 1e-8f ? ecs::vec3{v.x / l, v.y / l, v.z / l, 0.f} : v;
}

// Solve P_linear * out = v (3x3 from the parent matrix's columns; independent of world_to_local).
ecs::vec3 inverseLinear(const ecs::mat4& p, const ecs::vec3& v) {
    const ecs::vec3 a = linear(p, ecs::vec3{1.f, 0.f, 0.f, 0.f});
    const ecs::vec3 b = linear(p, ecs::vec3{0.f, 1.f, 0.f, 0.f});
    const ecs::vec3 c = linear(p, ecs::vec3{0.f, 0.f, 1.f, 0.f});
    auto cross = [](const ecs::vec3& x, const ecs::vec3& y) {
        return ecs::vec3{x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x, 0.f};
    };
    auto dot = [](const ecs::vec3& x, const ecs::vec3& y) { return x.x * y.x + x.y * y.y + x.z * y.z; };
    const f32 det = dot(a, cross(b, c));
    if (std::fabs(det) < 1e-12f) {
        return v;
    }
    // Cramer's rule.
    return ecs::vec3{dot(v, cross(b, c)) / det, dot(a, cross(v, c)) / det, dot(a, cross(b, v)) / det, 0.f};
}

f32 snapTo(f32 v, f32 step) { return step > 0.f ? std::round(v / step) * step : v; }

f32 segmentDistance(f32 px, f32 py, const GizmoHandle2D& h) {
    const f32 dx = h.x1 - h.x0, dy = h.y1 - h.y0;
    const f32 len2 = dx * dx + dy * dy;
    f32 t = len2 > 0.f ? ((px - h.x0) * dx + (py - h.y0) * dy) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const f32 cx = h.x0 + dx * t - px, cy = h.y0 + dy * t - py;
    return std::sqrt(cx * cx + cy * cy);
}

} // namespace

ecs::vec3 TransformGizmo::rotate(const ecs::quat& q, const ecs::vec3& v) {
    // v' = v + 2w(u×v) + 2u×(u×v)
    const f32 tx = 2.f * (q.y * v.z - q.z * v.y);
    const f32 ty = 2.f * (q.z * v.x - q.x * v.z);
    const f32 tz = 2.f * (q.x * v.y - q.y * v.x);
    return ecs::vec3{v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
                     v.z + q.w * tz + (q.x * ty - q.y * tx), 0.f};
}

ecs::quat TransformGizmo::multiply(const ecs::quat& a, const ecs::quat& b) {
    return ecs::quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

ecs::quat TransformGizmo::axisAngle(const ecs::vec3& axis, f32 radians) {
    const f32 s = std::sin(radians * 0.5f);
    return ecs::quat{axis.x * s, axis.y * s, axis.z * s, std::cos(radians * 0.5f)};
}

ecs::vec3 TransformGizmo::gizmoOrigin(const ecs::Transform& transform) {
    if (transform.parent == ecs::EntityID::null()) {
        return transform.position;
    }
    const auto& m = transform.local_to_world.data;
    return ecs::vec3{m[12], m[13], m[14], 1.f};
}

ecs::vec3 TransformGizmo::worldOrigin(const ecs::Transform& transform) const {
    if (!m_hasParent) {
        return gizmoOrigin(transform);
    }
    const ecs::vec3 w = ecs::transform_point(m_parentToWorld,
                                             ecs::vec3{transform.position.x, transform.position.y, transform.position.z, 1.f});
    return ecs::vec3{w.x, w.y, w.z, 1.f};
}

ecs::vec3 TransformGizmo::axisDirection(int axis, const ecs::quat& rotation) const {
    ecs::vec3 unit{axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f, axis == 2 ? 1.f : 0.f, 0.f};
    const bool local = m_settings.space == TransformSpace::Local || m_settings.tool == TransformTool::Scale;
    if (!local) {
        return unit;
    }
    const ecs::vec3 inParent = rotate(rotation, unit);
    return m_hasParent ? normalized(linear(m_parentToWorld, inParent)) : inParent;
}

bool TransformGizmo::layout(const ViewportPanel& panel, const ecs::Transform& transform) {
    m_visible = false;
    if (m_settings.tool == TransformTool::Select) {
        return false;
    }
    const ecs::vec3 origin = worldOrigin(transform);
    f32 ox = 0.f, oy = 0.f, rx = 0.f, ry = 0.f;
    if (!panel.worldToScreen(origin, ox, oy) || !panel.worldToScreen(add(origin, panel.right()), rx, ry)) {
        return false;
    }
    const f32 pxPerUnit = std::hypot(rx - ox, ry - oy);
    if (!(pxPerUnit > 1e-4f)) {
        return false;
    }
    m_worldLength = m_settings.handlePixels / pxPerUnit;
    for (int a = 0; a < 3; ++a) {
        m_axes[a] = axisDirection(a, transform.rotation);
        GizmoHandle2D& h = m_handles[a];
        h.x0 = ox;
        h.y0 = oy;
        h.visible = panel.worldToScreen(add(origin, m_axes[a], m_worldLength), h.x1, h.y1);
        // Axes pointing (almost) straight at the camera cannot be dragged meaningfully.
        if (h.visible && std::hypot(h.x1 - h.x0, h.y1 - h.y0) < 4.f) {
            h.visible = false;
        }
    }
    m_visible = true;
    return true;
}

int TransformGizmo::hitTest(f32 px, f32 py) const {
    if (!m_visible) {
        return -1;
    }
    int best = -1;
    f32 bestDist = m_settings.hitPixels;
    for (int a = 0; a < 3; ++a) {
        if (!m_handles[a].visible) {
            continue;
        }
        const f32 d = segmentDistance(px, py, m_handles[a]);
        if (d <= bestDist) {
            bestDist = d;
            best = a;
        }
    }
    return best;
}

bool TransformGizmo::beginDrag(int axis, f32 px, f32 py, const ecs::Transform& transform) {
    if (!m_visible || axis < 0 || axis > 2 || !m_handles[axis].visible) {
        return false;
    }
    m_dragAxis = axis;
    m_dragX = px;
    m_dragY = py;
    m_start = TransformCommand::capture(transform);
    return true;
}

TransformCommand::State TransformGizmo::dragTo(f32 px, f32 py) const {
    TransformCommand::State out = m_start;
    if (m_dragAxis < 0) {
        return out;
    }
    const GizmoHandle2D& h = m_handles[m_dragAxis];
    const f32 sx = h.x1 - h.x0, sy = h.y1 - h.y0;
    const f32 slen = std::hypot(sx, sy);
    const f32 mx = px - m_dragX, my = py - m_dragY;
    const f32 alongPx = slen > 0.f ? (mx * sx + my * sy) / slen : 0.f;
    const f32 alongWorld = alongPx / std::max(slen, 1e-4f) * m_worldLength;
    const bool snap = m_settings.snap;

    switch (m_settings.tool) {
    case TransformTool::Translate: {
        const f32 d = snap ? snapTo(alongWorld, m_settings.translateStep) : alongWorld;
        // m_axes are world directions; a child's position lives in its parent's frame.
        const ecs::vec3 worldDelta{m_axes[m_dragAxis].x * d, m_axes[m_dragAxis].y * d, m_axes[m_dragAxis].z * d, 0.f};
        const ecs::vec3 delta = m_hasParent ? inverseLinear(m_parentToWorld, worldDelta) : worldDelta;
        const ecs::vec3 moved = add(m_start.position, delta);
        out.position = ecs::vec3{moved.x, moved.y, moved.z, m_start.position.w};
        break;
    }
    case TransformTool::Rotate: {
        // Drag perpendicular to the projected axis (screen tangent) spins about it.
        const f32 tangentPx = slen > 0.f ? (mx * -sy + my * sx) / slen : mx;
        f32 deg = tangentPx * m_settings.rotateDegPerPixel;
        if (snap) {
            deg = snapTo(deg, m_settings.rotateStepDeg);
        }
        const ecs::vec3 unit{m_dragAxis == 0 ? 1.f : 0.f, m_dragAxis == 1 ? 1.f : 0.f, m_dragAxis == 2 ? 1.f : 0.f,
                             0.f};
        // Global: spin about the world axis, expressed in the parent frame for children.
        const ecs::vec3 spin = (m_settings.space == TransformSpace::World && m_hasParent)
                                   ? normalized(inverseLinear(m_parentToWorld, unit))
                                   : unit;
        const ecs::quat r = axisAngle(spin, deg * kPi / 180.f);
        out.rotation = m_settings.space == TransformSpace::Local ? multiply(m_start.rotation, r)
                                                                 : multiply(r, m_start.rotation);
        break;
    }
    case TransformTool::Scale: {
        f32 factor = 1.f + alongWorld / std::max(m_worldLength, 1e-4f);
        if (snap) {
            factor = snapTo(factor, m_settings.scaleStep);
        }
        factor = std::max(factor, 0.01f);
        f32* comps[3] = {&out.scale.x, &out.scale.y, &out.scale.z};
        *comps[m_dragAxis] *= factor;
        break;
    }
    case TransformTool::Select:
        break;
    }
    return out;
}

} // namespace fuse::editor
