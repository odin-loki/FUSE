#include <fuse/world2d/scene_object_2d.hpp>

#include <fuse/world2d/scene_handle_table.hpp>
#include <fuse/world2d/scene_transform.hpp>

#include <atomic>
#include <cmath>

namespace fuse {

namespace {

/// Process-wide stamp source: every recomputed world matrix gets a fresh, never reused version, so a
/// child's recorded parent version can only match the exact parent matrix it was built from (even if
/// a destroyed parent's address is reused by a new node).
std::atomic<u64> g_worldVersionCounter{1};

u64 nextWorldVersion() {
    return g_worldVersionCounter.fetch_add(1, std::memory_order_relaxed) + 1;
}

constexpr float kRadToDeg = 180.f / 3.14159265358979323846f;
constexpr float kDegToRad = 3.14159265358979323846f / 180.f;

} // namespace

SceneObject2D::SceneObject2D() : Object("SceneObject2D") {}

SceneObject2D::SceneObject2D(std::string name) : Object(std::move(name)) {}

SceneObject2D::~SceneObject2D() {
    if (m_handleTable != nullptr) {
        m_handleTable->onNodeDestroyed_(*this);
    }
}

void SceneObject2D::setPosition(float x, float y) {
    m_translation.x = x;
    m_translation.y = y;
    m_localDirty = true;
}

void SceneObject2D::setLocalTranslation(const math::Vec3& translation) {
    m_translation = translation;
    m_localDirty = true;
}

void SceneObject2D::setLocalRotation(const math::Quat& rotation) {
    m_rotation = rotation.normalized();
    scene_math::eulerDegFromQuat(m_rotation, m_yawDeg, m_pitchDeg, m_rollDeg);
    m_localDirty = true;
}

void SceneObject2D::setLocalScale(const math::Vec3& scale) {
    m_scale = scale;
    m_localDirty = true;
}

float SceneObject2D::rotation() const {
    return m_yawDeg * kDegToRad;
}

void SceneObject2D::setRotation(float radians) {
    setEulerDeg(radians * kRadToDeg, 0.f, 0.f);
}

void SceneObject2D::setScale(float sx, float sy) {
    m_scale.x = sx;
    m_scale.y = sy;
    m_localDirty = true;
}

void SceneObject2D::setEulerDeg(float yawDeg, float pitchDeg, float rollDeg) {
    m_yawDeg = yawDeg;
    m_pitchDeg = pitchDeg;
    m_rollDeg = rollDeg;
    m_rotation = scene_math::quatFromEulerDeg(yawDeg, pitchDeg, rollDeg);
    m_localDirty = true;
}

math::Mat4 SceneObject2D::localMatrix() const {
    return scene_math::composeTRS(m_translation, m_rotation, m_scale);
}

const SceneObject2D* SceneObject2D::sceneParent() const {
    const Object* node = parent();
    while (node != nullptr) {
        if (const SceneObject2D* scene = asSceneObject2D(node)) {
            return scene;
        }
        node = node->parent();
    }
    return nullptr;
}

const math::Mat4& SceneObject2D::refreshWorld_(const SceneObject2D* parentNode) const {
    const u64 parentVersion = parentNode != nullptr ? parentNode->m_worldVersion : 0u;
    if (!m_localDirty && m_worldVersion != 0u && parentNode == m_cachedParent &&
        parentVersion == m_cachedParentVersion) {
        return m_world;
    }
    const math::Mat4 local = localMatrix();
    m_world = parentNode != nullptr ? math::multiply(parentNode->m_world, local) : local;
    m_cachedParent = parentNode;
    m_cachedParentVersion = parentVersion;
    m_localDirty = false;
    m_worldVersion = nextWorldVersion();
    return m_world;
}

const math::Mat4& SceneObject2D::worldMatrix() const {
    const SceneObject2D* parentNode = sceneParent();
    if (parentNode != nullptr) {
        parentNode->worldMatrix(); // brings the whole ancestor chain up to date first
    }
    return refreshWorld_(parentNode);
}

u64 SceneObject2D::worldVersion() const {
    worldMatrix();
    return m_worldVersion;
}

math::Vec3 SceneObject2D::worldTranslation() const {
    const math::Mat4& w = worldMatrix();
    return {w.data[12], w.data[13], w.data[14]};
}

LocalTransform2D SceneObject2D::localTransform() const {
    LocalTransform2D local;
    local.x = m_translation.x;
    local.y = m_translation.y;
    local.rotation = rotation();
    local.scaleX = m_scale.x;
    local.scaleY = m_scale.y;
    return local;
}

WorldTransform2D SceneObject2D::worldTransform() const {
    const math::Mat4& w = worldMatrix();
    WorldTransform2D world;
    world.x = w.data[12];
    world.y = w.data[13];
    world.rotation = std::atan2(w.data[1], w.data[0]);
    world.scaleX = std::sqrt(w.data[0] * w.data[0] + w.data[1] * w.data[1] + w.data[2] * w.data[2]);
    world.scaleY = std::sqrt(w.data[4] * w.data[4] + w.data[5] * w.data[5] + w.data[6] * w.data[6]);
    return world;
}

void SceneObject2D::setWorldPose(const math::Vec3& worldT, const math::Quat& worldR) {
    const SceneObject2D* parentNode = sceneParent();
    if (parentNode == nullptr) {
        m_translation = worldT;
        setLocalRotation(worldR);
        return;
    }
    const math::Mat4 parentInv = scene_math::inverseAffineGeneral(parentNode->worldMatrix());
    m_translation = math::transformPoint(parentInv, worldT);
    // Parent rotation from its (unscaled) world basis.
    math::Vec3 pt;
    math::Quat pr;
    math::Vec3 ps;
    scene_math::decomposeTRS(parentNode->worldMatrix(), pt, pr, ps);
    setLocalRotation(pr.conjugate() * worldR);
}

void SceneObject2D::reparentKeepWorld(Object* newParent) {
    const math::Mat4 world = worldMatrix();
    reparent(newParent);
    const SceneObject2D* parentNode = sceneParent();
    const math::Mat4 local =
        parentNode != nullptr ? math::multiply(scene_math::inverseAffineGeneral(parentNode->worldMatrix()), world)
                              : world;
    math::Vec3 t;
    math::Quat r;
    math::Vec3 s;
    scene_math::decomposeTRS(local, t, r, s);
    m_translation = t;
    m_scale = s;
    setLocalRotation(r);
}

} // namespace fuse
