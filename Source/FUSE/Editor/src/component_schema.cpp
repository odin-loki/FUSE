#include <fuse/editor/component_schema.hpp>

#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/property_inspector.hpp>

#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>

#if defined(FUSE_WORLD3D_HAS_AUDIO)
#include <fuse/audio/audio_components.hpp>
#endif

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <typeindex>

namespace fuse::editor {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDegToRad = kPi / 180.f;
constexpr f32 kRadToDeg = 180.f / kPi;

// ---- text <-> value -------------------------------------------------------------------------------

std::string trimmed(std::string_view text) {
    usize begin = 0;
    usize end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool parseFloat(std::string_view text, f32& out) {
    const std::string s = trimmed(text);
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    const f32 value = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool parseU32(std::string_view text, u32& out) {
    const std::string s = trimmed(text);
    if (s.empty() || s[0] == '-') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size() || value > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

bool parseBool(std::string_view text, bool& out) {
    const std::string s = trimmed(text);
    if (s == "1" || s == "true" || s == "True" || s == "TRUE") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "False" || s == "FALSE") {
        out = false;
        return true;
    }
    return false;
}

std::string formatVec3(const ecs::vec3& v) {
    return formatPropertyFloat(v.x) + "," + formatPropertyFloat(v.y) + "," + formatPropertyFloat(v.z);
}

bool parseVec3Text(std::string_view text, ecs::vec3& out) {
    f32 values[3]{};
    if (!parsePropertyFloats(text, values, 3u)) {
        return false;
    }
    out.x = values[0];
    out.y = values[1];
    out.z = values[2];
    return true;
}

bool parseQuatText(std::string_view text, ecs::quat& out) {
    f32 values[4]{};
    if (!parsePropertyFloats(text, values, 4u)) {
        return false;
    }
    const f32 lengthSq = values[0] * values[0] + values[1] * values[1] + values[2] * values[2] + values[3] * values[3];
    if (!(lengthSq > 1e-12f)) {
        return false;
    }
    out = {values[0], values[1], values[2], values[3]};
    return true;
}

// ---- field accessors (member-pointer templates) ---------------------------------------------------

template <typename C, f32 C::*M>
bool readF32(const void* component, std::string& out) {
    out = formatPropertyFloat(static_cast<const C*>(component)->*M);
    return true;
}

template <typename C, f32 C::*M>
bool writeF32(void* component, std::string_view value) {
    f32 parsed = 0.f;
    if (!parseFloat(value, parsed)) {
        return false;
    }
    static_cast<C*>(component)->*M = parsed;
    return true;
}

template <typename C, ecs::vec3 C::*M>
bool readVec3(const void* component, std::string& out) {
    out = formatVec3(static_cast<const C*>(component)->*M);
    return true;
}

template <typename C, ecs::vec3 C::*M>
bool writeVec3(void* component, std::string_view value) {
    ecs::vec3 parsed = static_cast<C*>(component)->*M; // keeps .w
    if (!parseVec3Text(value, parsed)) {
        return false;
    }
    static_cast<C*>(component)->*M = parsed;
    return true;
}

template <typename C, bool C::*M>
bool readBool(const void* component, std::string& out) {
    out = (static_cast<const C*>(component)->*M) ? "1" : "0";
    return true;
}

template <typename C, bool C::*M>
bool writeBool(void* component, std::string_view value) {
    bool parsed = false;
    if (!parseBool(value, parsed)) {
        return false;
    }
    static_cast<C*>(component)->*M = parsed;
    return true;
}

template <typename C, u32 C::*M>
bool readU32(const void* component, std::string& out) {
    out = std::to_string(static_cast<const C*>(component)->*M);
    return true;
}

template <typename C, u32 C::*M>
bool writeU32(void* component, std::string_view value) {
    u32 parsed = 0;
    if (!parseU32(value, parsed)) {
        return false;
    }
    static_cast<C*>(component)->*M = parsed;
    return true;
}

/// Enum stored as `E` (enum class or integer) with `Count` valid values.
template <typename C, typename E, E C::*M, u32 Count>
bool readEnum(const void* component, std::string& out) {
    out = std::to_string(static_cast<u32>(static_cast<const C*>(component)->*M));
    return true;
}

template <typename C, typename E, E C::*M, u32 Count>
bool writeEnum(void* component, std::string_view value) {
    u32 parsed = 0;
    if (!parseU32(value, parsed) || parsed >= Count) {
        return false;
    }
    static_cast<C*>(component)->*M = static_cast<E>(parsed);
    return true;
}

// ---- component-specific fields --------------------------------------------------------------------

bool readTransformRotation(const void* component, std::string& out) {
    out = formatPropertyQuat(static_cast<const ecs::Transform*>(component)->rotation);
    return true;
}

bool writeTransformRotation(void* component, std::string_view value) {
    ecs::quat rotation{};
    if (!parseQuatText(value, rotation)) {
        return false;
    }
    static_cast<ecs::Transform*>(component)->rotation = rotation;
    return true;
}

bool readRigidBodyMass(const void* component, std::string& out) {
    out = formatPropertyFloat(static_cast<const ecs::RigidBody*>(component)->mass);
    return true;
}

bool writeRigidBodyMass(void* component, std::string_view value) {
    f32 mass = 0.f;
    if (!parseFloat(value, mass) || mass < 0.f) {
        return false;
    }
    auto* body = static_cast<ecs::RigidBody*>(component);
    body->mass = mass;
    body->inv_mass = mass > 0.f ? 1.f / mass : 0.f; // mass 0 = immovable
    return true;
}

/// SDF primitive type: read as the full `sdf.shape` text ("type;x,y,z") so a type edit undoes to
/// the exact old params; written either as that text or as a bare type index (params adapted by
/// `PropertyInspector::sdfParamsForType`).
bool readSdfType(const void* component, std::string& out) {
    const auto* sdf = static_cast<const ecs::SDFObject*>(component);
    out = PropertyInspector::formatSdfShape(sdf->type, sdf->params);
    return true;
}

bool writeSdfType(void* component, std::string_view value) {
    auto* sdf = static_cast<ecs::SDFObject*>(component);
    const std::string text = trimmed(value);
    if (text.find(';') != std::string::npos) {
        ecs::SDFPrimitive type{};
        ecs::vec3 params = sdf->params;
        if (!PropertyInspector::parseSdfShape(text, type, params)) {
            return false;
        }
        sdf->type = type;
        sdf->params = params;
        return true;
    }
    u32 index = 0;
    if (!parseU32(text, index) || index > static_cast<u32>(ecs::SDFPrimitive::Custom)) {
        return false;
    }
    const auto type = static_cast<ecs::SDFPrimitive>(index);
    sdf->params = PropertyInspector::sdfParamsForType(sdf->type, type, sdf->params);
    sdf->type = type;
    return true;
}

bool readScriptPath(const void* component, std::string& out) {
    const std::string_view path = static_cast<const ecs::Script*>(component)->path();
    out = path.empty() ? std::string(kPropertyEmptyString) : std::string(path);
    return true;
}

bool writeScriptPath(void* component, std::string_view value) {
    auto* script = static_cast<ecs::Script*>(component);
    if (!script->set_path(value == kPropertyEmptyString ? std::string_view() : value)) {
        return false;
    }
    script->started = false; // a new module starts fresh in the next play session
    return true;
}

bool readSpawnDatablock(const void* component, std::string& out) {
    out = std::to_string(static_cast<const ecs::SpawnMarker*>(component)->datablock_id);
    return true;
}

bool writeSpawnDatablock(void* component, std::string_view value) {
    u32 parsed = 0;
    if (!parseU32(value, parsed)) {
        return false;
    }
    static_cast<ecs::SpawnMarker*>(component)->datablock_id = parsed;
    return true;
}

// ---- enum names -----------------------------------------------------------------------------------

constexpr const char* kSdfTypeNames[] = {"Sphere", "Box", "Capsule", "Torus", "Cylinder", "Custom"};
constexpr const char* kSdfOpNames[] = {"Union", "Subtract", "Intersect", "SmoothUnion"};
constexpr const char* kColliderShapeNames[] = {"Sphere", "Box",   "Capsule", "ConvexHull",
                                               "SdfMesh", "Voxel", "Plane",   "TriMesh"};

// ---- field tables ---------------------------------------------------------------------------------

using FT = PropertyFieldType;

PropertyFieldDesc floatField(const char* label, const char* property, f64 lo, f64 hi, f64 step,
                             bool (*read)(const void*, std::string&), bool (*write)(void*, std::string_view)) {
    PropertyFieldDesc d{};
    d.label = label;
    d.propertyName = property;
    d.type = FT::Float;
    d.minValue = lo;
    d.maxValue = hi;
    d.step = step;
    d.read = read;
    d.write = write;
    return d;
}

PropertyFieldDesc typedField(const char* label, const char* property, FT type,
                             bool (*read)(const void*, std::string&), bool (*write)(void*, std::string_view),
                             f64 lo = -1.0e6, f64 hi = 1.0e6, f64 step = 0.1) {
    PropertyFieldDesc d{};
    d.label = label;
    d.propertyName = property;
    d.type = type;
    d.minValue = lo;
    d.maxValue = hi;
    d.step = step;
    d.read = read;
    d.write = write;
    return d;
}

PropertyFieldDesc enumField(const char* label, const char* property, const char* const* names, u32 count,
                            bool (*read)(const void*, std::string&), bool (*write)(void*, std::string_view)) {
    PropertyFieldDesc d = typedField(label, property, FT::Enum, read, write, 0.0, count - 1.0, 1.0);
    d.enumNames = names;
    d.enumCount = count;
    return d;
}

using ecs::Camera;
using ecs::Collider;
using ecs::DirectionalLight;
using ecs::Mesh;
using ecs::PointLight;
using ecs::RigidBody;
using ecs::Script;
using ecs::SDFObject;
using ecs::SpawnMarker;
using ecs::SpotLight;
using ecs::Transform;

const std::vector<PropertyFieldDesc>& transformFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        typedField("Position", "transform.position", FT::Vec3, readVec3<Transform, &Transform::position>,
                   writeVec3<Transform, &Transform::position>),
        typedField("Rotation", "transform.rotation", FT::Euler, readTransformRotation, writeTransformRotation,
                   -360.0, 360.0, 1.0),
        typedField("Scale", "transform.scale", FT::Vec3, readVec3<Transform, &Transform::scale>,
                   writeVec3<Transform, &Transform::scale>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& meshFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        typedField("Material", "mesh.material_id", FT::UInt, readU32<Mesh, &Mesh::material_id>,
                   writeU32<Mesh, &Mesh::material_id>, 0.0, 65535.0, 1.0),
        typedField("Cast Shadow", "Mesh.cast_shadow", FT::Bool, readBool<Mesh, &Mesh::cast_shadow>,
                   writeBool<Mesh, &Mesh::cast_shadow>),
        typedField("Receive Shadow", "Mesh.receive_shadow", FT::Bool, readBool<Mesh, &Mesh::receive_shadow>,
                   writeBool<Mesh, &Mesh::receive_shadow>),
        typedField("Visible", "Mesh.visible", FT::Bool, readBool<Mesh, &Mesh::visible>,
                   writeBool<Mesh, &Mesh::visible>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& sdfFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        enumField("Type", "SDFObject.type", kSdfTypeNames, 6u, readSdfType, writeSdfType),
        enumField("CSG Op", "SDFObject.op", kSdfOpNames, 4u, readEnum<SDFObject, ecs::SDFCsgOp, &SDFObject::op, 4u>,
                  writeEnum<SDFObject, ecs::SDFCsgOp, &SDFObject::op, 4u>),
        typedField("Params", "SDFObject.params", FT::Vec3, readVec3<SDFObject, &SDFObject::params>,
                   writeVec3<SDFObject, &SDFObject::params>, 0.0, 1.0e4, 0.05),
        typedField("Material", "SDFObject.material_id", FT::UInt, readU32<SDFObject, &SDFObject::material_id>,
                   writeU32<SDFObject, &SDFObject::material_id>, 0.0, 65535.0, 1.0),
        floatField("Blend Alpha", "sdf.blend_alpha", 0.0, 1.0, 0.01, readF32<SDFObject, &SDFObject::blend_alpha>,
                   writeF32<SDFObject, &SDFObject::blend_alpha>),
        floatField("Blend Radius", "SDFObject.blend_radius", 0.0, 1.0e3, 0.01,
                   readF32<SDFObject, &SDFObject::blend_radius>, writeF32<SDFObject, &SDFObject::blend_radius>),
        floatField("Roughness", "SDFObject.roughness", 0.0, 1.0, 0.01, readF32<SDFObject, &SDFObject::roughness>,
                   writeF32<SDFObject, &SDFObject::roughness>),
        typedField("CSG Order", "SDFObject.csg_order", FT::UInt, readU32<SDFObject, &SDFObject::csg_order>,
                   writeU32<SDFObject, &SDFObject::csg_order>, 0.0, 65535.0, 1.0),
        typedField("Casts Shadow", "SDFObject.casts_shadow", FT::Bool, readBool<SDFObject, &SDFObject::casts_shadow>,
                   writeBool<SDFObject, &SDFObject::casts_shadow>),
        typedField("Visible", "SDFObject.visible", FT::Bool, readBool<SDFObject, &SDFObject::visible>,
                   writeBool<SDFObject, &SDFObject::visible>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& rigidBodyFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        floatField("Mass", "RigidBody.mass", 0.0, 1.0e6, 0.1, readRigidBodyMass, writeRigidBodyMass),
        typedField("Velocity", "RigidBody.velocity", FT::Vec3, readVec3<RigidBody, &RigidBody::velocity>,
                   writeVec3<RigidBody, &RigidBody::velocity>),
        typedField("Angular Velocity", "RigidBody.angular_velocity", FT::Vec3,
                   readVec3<RigidBody, &RigidBody::angular_velocity>, writeVec3<RigidBody, &RigidBody::angular_velocity>),
        floatField("Restitution", "RigidBody.restitution", 0.0, 1.0, 0.01, readF32<RigidBody, &RigidBody::restitution>,
                   writeF32<RigidBody, &RigidBody::restitution>),
        floatField("Linear Damping", "RigidBody.linear_damping", 0.0, 1.0, 0.01,
                   readF32<RigidBody, &RigidBody::linear_damping>, writeF32<RigidBody, &RigidBody::linear_damping>),
        floatField("Angular Damping", "RigidBody.angular_damping", 0.0, 1.0, 0.01,
                   readF32<RigidBody, &RigidBody::angular_damping>, writeF32<RigidBody, &RigidBody::angular_damping>),
        typedField("Static", "RigidBody.is_static", FT::Bool, readBool<RigidBody, &RigidBody::is_static>,
                   writeBool<RigidBody, &RigidBody::is_static>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& colliderFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        enumField("Shape", "Collider.shape", kColliderShapeNames, 8u, readEnum<Collider, u32, &Collider::shape, 8u>,
                  writeEnum<Collider, u32, &Collider::shape, 8u>),
        typedField("Params", "Collider.params", FT::Vec3, readVec3<Collider, &Collider::params>,
                   writeVec3<Collider, &Collider::params>, 0.0, 1.0e4, 0.05),
        floatField("Scalar", "Collider.scalar", -1.0e4, 1.0e4, 0.05, readF32<Collider, &Collider::scalar>,
                   writeF32<Collider, &Collider::scalar>),
        floatField("Static Friction", "Collider.friction_static", 0.0, 10.0, 0.01,
                   readF32<Collider, &Collider::friction_static>, writeF32<Collider, &Collider::friction_static>),
        floatField("Dynamic Friction", "Collider.friction_dynamic", 0.0, 10.0, 0.01,
                   readF32<Collider, &Collider::friction_dynamic>, writeF32<Collider, &Collider::friction_dynamic>),
        typedField("Layer", "Collider.layer", FT::UInt, readU32<Collider, &Collider::layer>,
                   writeU32<Collider, &Collider::layer>, 0.0, 4294967295.0, 1.0),
        typedField("Mask", "Collider.mask", FT::UInt, readU32<Collider, &Collider::mask>,
                   writeU32<Collider, &Collider::mask>, 0.0, 4294967295.0, 1.0),
        typedField("Trigger", "Collider.is_trigger", FT::Bool, readBool<Collider, &Collider::is_trigger>,
                   writeBool<Collider, &Collider::is_trigger>),
        typedField("CCD", "Collider.ccd", FT::Bool, readBool<Collider, &Collider::ccd>,
                   writeBool<Collider, &Collider::ccd>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& cameraFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        floatField("FOV (deg)", "Camera.fov_deg", 1.0, 179.0, 1.0, readF32<Camera, &Camera::fov_deg>,
                   writeF32<Camera, &Camera::fov_deg>),
        floatField("Near", "Camera.near_plane", 0.0001, 1.0e4, 0.01, readF32<Camera, &Camera::near_plane>,
                   writeF32<Camera, &Camera::near_plane>),
        floatField("Far", "Camera.far_plane", 0.01, 1.0e7, 10.0, readF32<Camera, &Camera::far_plane>,
                   writeF32<Camera, &Camera::far_plane>),
        floatField("Aspect", "Camera.aspect_ratio", 0.01, 100.0, 0.01, readF32<Camera, &Camera::aspect_ratio>,
                   writeF32<Camera, &Camera::aspect_ratio>),
        typedField("Active", "Camera.is_active", FT::Bool, readBool<Camera, &Camera::is_active>,
                   writeBool<Camera, &Camera::is_active>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& directionalFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        typedField("Color", "DirectionalLight.color", FT::Color, readVec3<DirectionalLight, &DirectionalLight::color>,
                   writeVec3<DirectionalLight, &DirectionalLight::color>, 0.0, 1.0e3, 0.01),
        floatField("Intensity", "directional.intensity", 0.0, 1.0e6, 0.1,
                   readF32<DirectionalLight, &DirectionalLight::intensity>,
                   writeF32<DirectionalLight, &DirectionalLight::intensity>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& pointFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        typedField("Color", "PointLight.color", FT::Color, readVec3<PointLight, &PointLight::color>,
                   writeVec3<PointLight, &PointLight::color>, 0.0, 1.0e3, 0.01),
        floatField("Intensity", "PointLight.intensity", 0.0, 1.0e6, 0.1, readF32<PointLight, &PointLight::intensity>,
                   writeF32<PointLight, &PointLight::intensity>),
        floatField("Radius", "PointLight.radius", 0.0, 1.0e5, 0.1, readF32<PointLight, &PointLight::radius>,
                   writeF32<PointLight, &PointLight::radius>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& spotFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        typedField("Color", "SpotLight.color", FT::Color, readVec3<SpotLight, &SpotLight::color>,
                   writeVec3<SpotLight, &SpotLight::color>, 0.0, 1.0e3, 0.01),
        floatField("Intensity", "spot.intensity", 0.0, 1.0e6, 0.1, readF32<SpotLight, &SpotLight::intensity>,
                   writeF32<SpotLight, &SpotLight::intensity>),
        floatField("Inner Cone (deg)", "SpotLight.inner_cone_deg", 0.0, 90.0, 0.5,
                   readF32<SpotLight, &SpotLight::inner_cone_deg>, writeF32<SpotLight, &SpotLight::inner_cone_deg>),
        floatField("Outer Cone (deg)", "SpotLight.outer_cone_deg", 0.0, 90.0, 0.5,
                   readF32<SpotLight, &SpotLight::outer_cone_deg>, writeF32<SpotLight, &SpotLight::outer_cone_deg>),
        floatField("Radius", "SpotLight.radius", 0.0, 1.0e5, 0.1, readF32<SpotLight, &SpotLight::radius>,
                   writeF32<SpotLight, &SpotLight::radius>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& spawnFields() {
    static const std::vector<PropertyFieldDesc> fields = {
        typedField("Datablock", "SpawnMarker.datablock_id", FT::UInt, readSpawnDatablock, writeSpawnDatablock, 0.0,
                   4294967295.0, 1.0),
        typedField("Active", "SpawnMarker.active", FT::Bool, readBool<SpawnMarker, &SpawnMarker::active>,
                   writeBool<SpawnMarker, &SpawnMarker::active>),
    };
    return fields;
}

const std::vector<PropertyFieldDesc>& scriptFields() {
    static const std::vector<PropertyFieldDesc> fields = [] {
        std::vector<PropertyFieldDesc> list = {
            typedField("Script", "Script.script_path", FT::AssetPath, readScriptPath, writeScriptPath),
            typedField("Enabled", "Script.enabled", FT::Bool, readBool<Script, &Script::enabled>,
                       writeBool<Script, &Script::enabled>),
        };
        list[0].assetFilter = "Lua scripts (*.lua)";
        return list;
    }();
    return fields;
}

#if defined(FUSE_WORLD3D_HAS_AUDIO)
using fuse::audio::AudioSource;

template <f32 fuse::audio::AudioSourceDesc::*M>
bool readAudioF32(const void* component, std::string& out) {
    out = formatPropertyFloat(static_cast<const AudioSource*>(component)->desc.*M);
    return true;
}

template <f32 fuse::audio::AudioSourceDesc::*M>
bool writeAudioF32(void* component, std::string_view value) {
    f32 parsed = 0.f;
    if (!parseFloat(value, parsed)) {
        return false;
    }
    static_cast<AudioSource*>(component)->desc.*M = parsed;
    return true;
}

template <bool fuse::audio::AudioSourceDesc::*M>
bool readAudioBool(const void* component, std::string& out) {
    out = (static_cast<const AudioSource*>(component)->desc.*M) ? "1" : "0";
    return true;
}

template <bool fuse::audio::AudioSourceDesc::*M>
bool writeAudioBool(void* component, std::string_view value) {
    bool parsed = false;
    if (!parseBool(value, parsed)) {
        return false;
    }
    static_cast<AudioSource*>(component)->desc.*M = parsed;
    return true;
}

template <typename E, E fuse::audio::AudioSourceDesc::*M, u32 Count>
bool readAudioEnum(const void* component, std::string& out) {
    out = std::to_string(static_cast<u32>(static_cast<const AudioSource*>(component)->desc.*M));
    return true;
}

template <typename E, E fuse::audio::AudioSourceDesc::*M, u32 Count>
bool writeAudioEnum(void* component, std::string_view value) {
    u32 parsed = 0;
    if (!parseU32(value, parsed) || parsed >= Count) {
        return false;
    }
    static_cast<AudioSource*>(component)->desc.*M = static_cast<E>(parsed);
    return true;
}

constexpr const char* kAttenuationNames[] = {"Linear", "Logarithmic", "Exponential", "Inverse", "Custom"};
constexpr const char* kAudioBusNames[] = {"Master", "Sfx", "Music", "Voice"};

const std::vector<PropertyFieldDesc>& audioFields() {
    using D = fuse::audio::AudioSourceDesc;
    static const std::vector<PropertyFieldDesc> fields = {
        floatField("Volume", "AudioSource.volume", 0.0, 4.0, 0.01, readAudioF32<&D::volume>, writeAudioF32<&D::volume>),
        floatField("Pitch", "AudioSource.pitch", 0.01, 4.0, 0.01, readAudioF32<&D::pitch>, writeAudioF32<&D::pitch>),
        floatField("Min Distance", "AudioSource.min_distance", 0.0, 1.0e4, 0.1, readAudioF32<&D::min_distance>,
                   writeAudioF32<&D::min_distance>),
        floatField("Max Distance", "AudioSource.max_distance", 0.0, 1.0e5, 1.0, readAudioF32<&D::max_distance>,
                   writeAudioF32<&D::max_distance>),
        enumField("Attenuation", "AudioSource.attenuation", kAttenuationNames, 5u,
                  readAudioEnum<fuse::audio::AttenuationCurve, &D::attenuation, 5u>,
                  writeAudioEnum<fuse::audio::AttenuationCurve, &D::attenuation, 5u>),
        floatField("Rolloff", "AudioSource.rolloff", 0.0, 16.0, 0.05, readAudioF32<&D::rolloff>,
                   writeAudioF32<&D::rolloff>),
        enumField("Bus", "AudioSource.bus", kAudioBusNames, 4u, readAudioEnum<fuse::audio::AudioBus, &D::bus, 4u>,
                  writeAudioEnum<fuse::audio::AudioBus, &D::bus, 4u>),
        typedField("Looping", "AudioSource.looping", FT::Bool, readAudioBool<&D::looping>, writeAudioBool<&D::looping>),
        typedField("Spatial", "AudioSource.spatial", FT::Bool, readAudioBool<&D::spatial>, writeAudioBool<&D::spatial>),
        typedField("Play On Awake", "AudioSource.play_on_awake", FT::Bool, readAudioBool<&D::play_on_awake>,
                   writeAudioBool<&D::play_on_awake>),
    };
    return fields;
}
#endif

// ---- type-erased registry operations --------------------------------------------------------------

template <typename T>
bool hasOp(const ecs::Registry& registry, ecs::EntityID entity) {
    return registry.alive(entity) && registry.has<T>(entity);
}

template <typename T>
void* getOp(ecs::Registry& registry, ecs::EntityID entity) {
    return registry.alive(entity) ? static_cast<void*>(registry.get<T>(entity)) : nullptr;
}

template <typename T>
const void* getConstOp(const ecs::Registry& registry, ecs::EntityID entity) {
    return registry.alive(entity) ? static_cast<const void*>(registry.get<T>(entity)) : nullptr;
}

template <typename T>
void addOp(ecs::Registry& registry, ecs::EntityID entity, const void* bytes) {
    static_assert(std::is_trivially_copyable_v<T>, "inspector snapshots copy components as bytes");
    T value{};
    if (bytes != nullptr) {
        std::memcpy(static_cast<void*>(&value), bytes, sizeof(T));
    }
    registry.add<T>(entity, value);
}

template <typename T>
void removeOp(ecs::Registry& registry, ecs::EntityID entity) {
    registry.remove<T>(entity);
}

template <typename T>
ComponentKindDesc kind(const char* displayName, const std::vector<PropertyFieldDesc>* fields, bool addable = true,
                       bool removable = true) {
    ComponentKindDesc d{};
    d.name = T::component_name;
    d.displayName = displayName;
    if (fields != nullptr) {
        d.fields = std::span<const PropertyFieldDesc>(fields->data(), fields->size());
    }
    d.addable = addable;
    d.removable = removable;
    d.size = sizeof(T);
    d.has = hasOp<T>;
    d.get = getOp<T>;
    d.getConst = getConstOp<T>;
    d.add = addOp<T>;
    d.remove = removeOp<T>;
    return d;
}

const std::vector<ComponentKindDesc>& kinds() {
    static const std::vector<ComponentKindDesc> table = [] {
        std::vector<ComponentKindDesc> list;
        list.push_back(kind<Transform>("Transform", &transformFields(), true, false));
        list.push_back(kind<Mesh>("Mesh", &meshFields()));
        list.push_back(kind<SDFObject>("SDF Object", &sdfFields()));
        list.push_back(kind<RigidBody>("Rigid Body", &rigidBodyFields()));
        list.push_back(kind<Collider>("Collider", &colliderFields()));
        list.push_back(kind<Camera>("Camera", &cameraFields()));
        list.push_back(kind<DirectionalLight>("Directional Light", &directionalFields()));
        list.push_back(kind<PointLight>("Point Light", &pointFields()));
        list.push_back(kind<SpotLight>("Spot Light", &spotFields()));
#if defined(FUSE_WORLD3D_HAS_AUDIO)
        list.push_back(kind<AudioSource>("Audio Source", &audioFields()));
#endif
        list.push_back(kind<Script>("Script", &scriptFields()));
        list.push_back(kind<SpawnMarker>("Spawn Marker", &spawnFields()));
        list.push_back(kind<ecs::TagStatic>("Static (tag)", nullptr));
        list.push_back(kind<ecs::TagKinematic>("Kinematic (tag)", nullptr));
        list.push_back(kind<ecs::TagPlayer>("Player (tag)", nullptr));
        return list;
    }();
    return table;
}

// ---- matrices -------------------------------------------------------------------------------------

/// General affine inverse (3x3 linear part + translation); identity for a singular matrix.
ecs::mat4 inverseAffineGeneral(const ecs::mat4& m) {
    const auto& a = m.data;
    // Column-major: element (row r, col c) = a[r + 4c].
    const f32 m00 = a[0], m01 = a[4], m02 = a[8];
    const f32 m10 = a[1], m11 = a[5], m12 = a[9];
    const f32 m20 = a[2], m21 = a[6], m22 = a[10];
    const f32 c00 = m11 * m22 - m12 * m21;
    const f32 c01 = m12 * m20 - m10 * m22;
    const f32 c02 = m10 * m21 - m11 * m20;
    const f32 det = m00 * c00 + m01 * c01 + m02 * c02;
    if (!(std::fabs(det) > 1e-20f)) {
        return ecs::mat4::identity();
    }
    const f32 inv = 1.f / det;
    f32 r[3][3];
    r[0][0] = c00 * inv;
    r[0][1] = (m02 * m21 - m01 * m22) * inv;
    r[0][2] = (m01 * m12 - m02 * m11) * inv;
    r[1][0] = c01 * inv;
    r[1][1] = (m00 * m22 - m02 * m20) * inv;
    r[1][2] = (m02 * m10 - m00 * m12) * inv;
    r[2][0] = c02 * inv;
    r[2][1] = (m01 * m20 - m00 * m21) * inv;
    r[2][2] = (m00 * m11 - m01 * m10) * inv;
    ecs::mat4 out = ecs::mat4::identity();
    for (u32 row = 0; row < 3; ++row) {
        for (u32 col = 0; col < 3; ++col) {
            out.data[row + 4 * col] = r[row][col];
        }
    }
    const f32 tx = a[12], ty = a[13], tz = a[14];
    for (u32 row = 0; row < 3; ++row) {
        out.data[row + 12] = -(r[row][0] * tx + r[row][1] * ty + r[row][2] * tz);
    }
    return out;
}

ecs::quat quatFromRotationMatrix(const f32 (&r)[3][3]) {
    // r[row][col], orthonormal.
    ecs::quat q{};
    const f32 trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.f) {
        const f32 s = std::sqrt(trace + 1.f) * 2.f;
        q.w = 0.25f * s;
        q.x = (r[2][1] - r[1][2]) / s;
        q.y = (r[0][2] - r[2][0]) / s;
        q.z = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const f32 s = std::sqrt(1.f + r[0][0] - r[1][1] - r[2][2]) * 2.f;
        q.w = (r[2][1] - r[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (r[0][1] + r[1][0]) / s;
        q.z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const f32 s = std::sqrt(1.f + r[1][1] - r[0][0] - r[2][2]) * 2.f;
        q.w = (r[0][2] - r[2][0]) / s;
        q.x = (r[0][1] + r[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (r[1][2] + r[2][1]) / s;
    } else {
        const f32 s = std::sqrt(1.f + r[2][2] - r[0][0] - r[1][1]) * 2.f;
        q.w = (r[1][0] - r[0][1]) / s;
        q.x = (r[0][2] + r[2][0]) / s;
        q.y = (r[1][2] + r[2][1]) / s;
        q.z = 0.25f * s;
    }
    const f32 length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length > 0.f) {
        q.x /= length;
        q.y /= length;
        q.z /= length;
        q.w /= length;
    }
    if (q.w < 0.f) { // canonical hemisphere
        q = {-q.x, -q.y, -q.z, -q.w};
    }
    return q;
}

} // namespace

// ---- public API -----------------------------------------------------------------------------------

void registerEditorComponentTypes() {
    ecs::register_builtin_components();
#if defined(FUSE_WORLD3D_HAS_AUDIO)
    ecs::ComponentTypes::register_type<fuse::audio::AudioSource>();
#endif
}

std::span<const ComponentKindDesc> editorComponentKinds() {
    const std::vector<ComponentKindDesc>& table = kinds();
    return {table.data(), table.size()};
}

const ComponentKindDesc* findComponentKind(std::string_view componentName) {
    for (const ComponentKindDesc& k : kinds()) {
        if (componentName == k.name) {
            return &k;
        }
    }
    return nullptr;
}

const PropertyFieldDesc* findComponentProperty(std::string_view propertyName, const ComponentKindDesc** kindOut) {
    for (const ComponentKindDesc& k : kinds()) {
        for (const PropertyFieldDesc& field : k.fields) {
            if (propertyName == field.propertyName) {
                if (kindOut != nullptr) {
                    *kindOut = &k;
                }
                return &field;
            }
        }
    }
    return nullptr;
}

bool readComponentProperty(const ecs::Registry& registry, ecs::EntityID entity, std::string_view propertyName,
                           std::string& out) {
    const ComponentKindDesc* k = nullptr;
    const PropertyFieldDesc* field = findComponentProperty(propertyName, &k);
    if (field == nullptr || field->read == nullptr || !k->has(registry, entity)) {
        return false;
    }
    const void* component = k->getConst(registry, entity);
    return component != nullptr && field->read(component, out);
}

bool writeComponentProperty(ecs::Registry& registry, ecs::EntityID entity, std::string_view propertyName,
                            std::string_view value) {
    const ComponentKindDesc* k = nullptr;
    const PropertyFieldDesc* field = findComponentProperty(propertyName, &k);
    if (field == nullptr || field->write == nullptr || !k->has(registry, entity)) {
        return false;
    }
    void* component = k->get(registry, entity);
    if (component == nullptr || !field->write(component, value)) {
        return false;
    }
    if (ecs::Transform* transform = registry.get<ecs::Transform>(entity)) {
        transform->dirty = true; // any component edit re-renders the entity
    }
    return true;
}

bool parsePropertyFloats(std::string_view text, f32* values, u32 count) {
    std::string normalized(text);
    for (char& ch : normalized) {
        if (ch == ',') {
            ch = ' ';
        }
    }
    const char* cursor = normalized.c_str();
    for (u32 i = 0; i < count; ++i) {
        char* end = nullptr;
        const f32 value = std::strtof(cursor, &end);
        if (end == cursor || !std::isfinite(value)) {
            return false;
        }
        values[i] = value;
        cursor = end;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    return *cursor == '\0';
}

std::string formatPropertyQuat(const ecs::quat& rotation) {
    return formatPropertyFloat(rotation.x) + "," + formatPropertyFloat(rotation.y) + "," +
           formatPropertyFloat(rotation.z) + "," + formatPropertyFloat(rotation.w);
}

ecs::quat quatFromEulerDeg(f32 xDeg, f32 yDeg, f32 zDeg) {
    const f32 hx = 0.5f * xDeg * kDegToRad;
    const f32 hy = 0.5f * yDeg * kDegToRad;
    const f32 hz = 0.5f * zDeg * kDegToRad;
    const f32 cx = std::cos(hx), sx = std::sin(hx);
    const f32 cy = std::cos(hy), sy = std::sin(hy);
    const f32 cz = std::cos(hz), sz = std::sin(hz);
    // q = qz * qy * qx
    ecs::quat q{};
    q.w = cz * cy * cx + sz * sy * sx;
    q.x = cz * cy * sx - sz * sy * cx;
    q.y = cz * sy * cx + sz * cy * sx;
    q.z = sz * cy * cx - cz * sy * sx;
    return q;
}

ecs::vec3 eulerDegFromQuat(const ecs::quat& rotation) {
    ecs::quat q = rotation;
    const f32 length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length > 0.f) {
        q.x /= length;
        q.y /= length;
        q.z /= length;
        q.w /= length;
    }
    ecs::vec3 out{};
    const f32 sinXcosY = 2.f * (q.w * q.x + q.y * q.z);
    const f32 cosXcosY = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    out.x = std::atan2(sinXcosY, cosXcosY) * kRadToDeg;
    f32 sinY = 2.f * (q.w * q.y - q.z * q.x);
    sinY = sinY > 1.f ? 1.f : (sinY < -1.f ? -1.f : sinY);
    out.y = std::asin(sinY) * kRadToDeg;
    const f32 sinZcosY = 2.f * (q.w * q.z + q.x * q.y);
    const f32 cosZcosY = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    out.z = std::atan2(sinZcosY, cosZcosY) * kRadToDeg;
    return out;
}

ecs::mat4 entityWorldMatrix(const ecs::Registry& registry, ecs::EntityID entity) {
    ecs::mat4 world = ecs::mat4::identity();
    ecs::EntityID current = entity;
    // Bounded walk (a malformed parent cycle cannot hang the editor).
    for (u32 depth = 0; depth < 1024u && current.valid() && registry.alive(current); ++depth) {
        const ecs::Transform* transform = registry.get<ecs::Transform>(current);
        if (transform == nullptr) {
            break;
        }
        world = ecs::multiply(ecs::from_trs(transform->position, transform->rotation, transform->scale), world);
        current = transform->parent;
    }
    return world;
}

void localTrsForWorld(const ecs::mat4& parentWorld, const ecs::mat4& world, ecs::vec3& position,
                      ecs::quat& rotation, ecs::vec3& scale) {
    const ecs::mat4 local = ecs::multiply(inverseAffineGeneral(parentWorld), world);
    const auto& a = local.data;
    position = {a[12], a[13], a[14], position.w};
    f32 columns[3][3] = {{a[0], a[1], a[2]}, {a[4], a[5], a[6]}, {a[8], a[9], a[10]}};
    f32 lengths[3]{};
    for (u32 c = 0; c < 3; ++c) {
        lengths[c] = std::sqrt(columns[c][0] * columns[c][0] + columns[c][1] * columns[c][1] +
                               columns[c][2] * columns[c][2]);
    }
    // A mirrored basis keeps a proper rotation with a negative X scale.
    const f32 det = columns[0][0] * (columns[1][1] * columns[2][2] - columns[1][2] * columns[2][1]) -
                    columns[1][0] * (columns[0][1] * columns[2][2] - columns[0][2] * columns[2][1]) +
                    columns[2][0] * (columns[0][1] * columns[1][2] - columns[0][2] * columns[1][1]);
    if (det < 0.f) {
        lengths[0] = -lengths[0];
    }
    f32 r[3][3]{};
    for (u32 c = 0; c < 3; ++c) {
        const f32 inv = lengths[c] != 0.f ? 1.f / lengths[c] : 0.f;
        for (u32 row = 0; row < 3; ++row) {
            r[row][c] = columns[c][row] * inv;
        }
    }
    rotation = quatFromRotationMatrix(r);
    scale = {lengths[0], lengths[1], lengths[2], scale.w};
}

// ---- ComponentPresenceCommand ---------------------------------------------------------------------

ComponentPresenceCommand::ComponentPresenceCommand(ecs::Registry& registry, ecs::EntityID entity,
                                                   const ComponentKindDesc& kind, bool adding)
    : m_registry(registry), m_entity(entity), m_kind(&kind), m_adding(adding) {}

void ComponentPresenceCommand::addFromSnapshot_() {
    m_kind->add(m_registry, m_entity, m_haveSnapshot ? m_snapshot.data() : nullptr);
}

void ComponentPresenceCommand::removeWithSnapshot_() {
    const void* bytes = m_kind->getConst(m_registry, m_entity);
    if (bytes != nullptr) {
        m_snapshot.assign(static_cast<const unsigned char*>(bytes), static_cast<const unsigned char*>(bytes) + m_kind->size);
        m_haveSnapshot = true;
    }
    m_kind->remove(m_registry, m_entity);
}

void ComponentPresenceCommand::execute() {
    m_applied = false;
    if (!m_registry.alive(m_entity)) {
        return;
    }
    const bool present = m_kind->has(m_registry, m_entity);
    if (m_adding) {
        if (present) {
            return;
        }
        addFromSnapshot_(); // redo re-adds the values the undo captured
    } else {
        if (!present || !m_kind->removable) {
            return;
        }
        removeWithSnapshot_();
    }
    if (ecs::Transform* transform = m_registry.get<ecs::Transform>(m_entity)) {
        transform->dirty = true;
    }
    m_applied = true;
}

void ComponentPresenceCommand::undo() {
    if (!m_applied || !m_registry.alive(m_entity)) {
        return;
    }
    if (m_adding) {
        removeWithSnapshot_();
    } else {
        addFromSnapshot_();
    }
    if (ecs::Transform* transform = m_registry.get<ecs::Transform>(m_entity)) {
        transform->dirty = true;
    }
    m_applied = false;
}

std::string ComponentPresenceCommand::description() const {
    return std::string(m_adding ? "Add " : "Remove ") + m_kind->displayName;
}

} // namespace fuse::editor
