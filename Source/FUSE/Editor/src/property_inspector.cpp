#include <fuse/editor/property_inspector.hpp>

#include <fuse/editor/material_property_inspect.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/object.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

namespace fuse::editor {

namespace {

std::string f(f32 value) {
    return formatPropertyFloat(value);
}

std::string v3(const ecs::vec3& value) {
    return f(value.x) + "," + f(value.y) + "," + f(value.z);
}

std::string b(bool value) {
    return value ? "true" : "false";
}

template <typename T>
const T& as(const void* data) {
    return *static_cast<const T*>(data);
}

const char* sdfPrimitiveName(ecs::SDFPrimitive type) {
    switch (type) {
    case ecs::SDFPrimitive::Sphere:
        return "Sphere";
    case ecs::SDFPrimitive::Box:
        return "Box";
    case ecs::SDFPrimitive::Capsule:
        return "Capsule";
    case ecs::SDFPrimitive::Torus:
        return "Torus";
    case ecs::SDFPrimitive::Cylinder:
        return "Cylinder";
    case ecs::SDFPrimitive::Custom:
        return "Custom";
    }
    return "Unknown";
}

const char* sdfOpName(ecs::SDFCsgOp op) {
    switch (op) {
    case ecs::SDFCsgOp::Union:
        return "Union";
    case ecs::SDFCsgOp::Subtract:
        return "Subtract";
    case ecs::SDFCsgOp::Intersect:
        return "Intersect";
    case ecs::SDFCsgOp::SmoothUnion:
        return "SmoothUnion";
    }
    return "Unknown";
}

bool isTagName(std::string_view name) {
    return name == ecs::TagStatic::component_name || name == ecs::TagPlayer::component_name ||
           name == ecs::TagKinematic::component_name || name == ecs::TagDestroy::component_name;
}

} // namespace

PropertyInspector::ComponentSection PropertyInspector::describeComponent(const char* componentName, usize size,
                                                                        const void* data) {
    ComponentSection section{};
    section.componentName = componentName != nullptr ? componentName : "";
    const std::string_view name{section.componentName};
    std::vector<Field>& out = section.fields;

    if (data == nullptr) {
        section.generic = true;
    } else if (name == ecs::Transform::component_name && size == sizeof(ecs::Transform)) {
        const auto& c = as<ecs::Transform>(data);
        out = {{"position", v3(c.position)},
               {"rotation", f(c.rotation.x) + "," + f(c.rotation.y) + "," + f(c.rotation.z) + "," + f(c.rotation.w)},
               {"scale", v3(c.scale)},
               {"parent", c.parent.valid() ? std::to_string(c.parent.index) : std::string("none")}};
    } else if (name == ecs::Mesh::component_name && size == sizeof(ecs::Mesh)) {
        const auto& c = as<ecs::Mesh>(data);
        out = {{"material_id", std::to_string(c.material_id)},
               {"index_count", std::to_string(c.index_count)},
               {"aabb_min", v3(c.aabb_min)},
               {"aabb_max", v3(c.aabb_max)},
               {"cast_shadow", b(c.cast_shadow)},
               {"receive_shadow", b(c.receive_shadow)},
               {"visible", b(c.visible)}};
    } else if (name == ecs::SDFObject::component_name && size == sizeof(ecs::SDFObject)) {
        const auto& c = as<ecs::SDFObject>(data);
        out = {{"type", sdfPrimitiveName(c.type)},
               {"op", sdfOpName(c.op)},
               {"params", v3(c.params)},
               {"material_id", std::to_string(c.material_id)},
               {"blend_alpha", f(c.blend_alpha)},
               {"blend_radius", f(c.blend_radius)},
               {"roughness", f(c.roughness)},
               {"csg_order", std::to_string(c.csg_order)},
               {"casts_shadow", b(c.casts_shadow)},
               {"visible", b(c.visible)}};
    } else if (name == ecs::RigidBody::component_name && size == sizeof(ecs::RigidBody)) {
        const auto& c = as<ecs::RigidBody>(data);
        out = {{"mass", f(c.mass)},
               {"velocity", v3(c.velocity)},
               {"angular_velocity", v3(c.angular_velocity)},
               {"restitution", f(c.restitution)},
               {"linear_damping", f(c.linear_damping)},
               {"angular_damping", f(c.angular_damping)},
               {"is_static", b(c.is_static)},
               {"is_sleeping", b(c.is_sleeping)}};
    } else if (name == ecs::Camera::component_name && size == sizeof(ecs::Camera)) {
        const auto& c = as<ecs::Camera>(data);
        out = {{"fov_deg", f(c.fov_deg)},
               {"near_plane", f(c.near_plane)},
               {"far_plane", f(c.far_plane)},
               {"aspect_ratio", f(c.aspect_ratio)},
               {"is_active", b(c.is_active)}};
    } else if (name == ecs::DirectionalLight::component_name && size == sizeof(ecs::DirectionalLight)) {
        const auto& c = as<ecs::DirectionalLight>(data);
        out = {{"color", v3(c.color)}, {"intensity", f(c.intensity)}};
    } else if (name == ecs::PointLight::component_name && size == sizeof(ecs::PointLight)) {
        const auto& c = as<ecs::PointLight>(data);
        out = {{"color", v3(c.color)}, {"intensity", f(c.intensity)}, {"radius", f(c.radius)}};
    } else if (name == ecs::SpotLight::component_name && size == sizeof(ecs::SpotLight)) {
        const auto& c = as<ecs::SpotLight>(data);
        out = {{"color", v3(c.color)},
               {"intensity", f(c.intensity)},
               {"inner_cone_deg", f(c.inner_cone_deg)},
               {"outer_cone_deg", f(c.outer_cone_deg)},
               {"radius", f(c.radius)}};
    } else if (name == ecs::SpawnMarker::component_name && size == sizeof(ecs::SpawnMarker)) {
        const auto& c = as<ecs::SpawnMarker>(data);
        out = {{"datablock_id", std::to_string(c.datablock_id)}, {"active", b(c.active)}};
    } else if (name == ecs::Collider::component_name && size == sizeof(ecs::Collider)) {
        const auto& c = as<ecs::Collider>(data);
        out = {{"shape", std::to_string(c.shape)},
               {"params", v3(c.params)},
               {"scalar", f(c.scalar)},
               {"friction_static", f(c.friction_static)},
               {"friction_dynamic", f(c.friction_dynamic)},
               {"layer", std::to_string(c.layer)},
               {"mask", std::to_string(c.mask)},
               {"is_trigger", b(c.is_trigger)},
               {"ccd", b(c.ccd)}};
    } else if (isTagName(name)) {
        // Marker component: header only, no editable fields.
    } else {
        // Registered by another module with no typed layout here: show it read-only as bytes.
        section.generic = true;
        static constexpr char kHex[] = "0123456789abcdef";
        std::string hex;
        const usize shown = size < 32u ? size : 32u;
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (usize i = 0; i < shown; ++i) {
            hex.push_back(kHex[bytes[i] >> 4]);
            hex.push_back(kHex[bytes[i] & 0xF]);
        }
        if (shown < size) {
            hex += "...";
        }
        out = {{"size", std::to_string(size)}, {"bytes", hex}};
    }

    section.exposedFieldCount = static_cast<u32>(out.size());
    return section;
}

void PropertyInspector::sync(const EditorState& state, EditorScene& scene) {
    m_sections.clear();
    m_target = state.primarySelection;

    if (!m_target.valid() || !scene.registry().alive(m_target)) {
        m_target = ecs::EntityID::null();
        return;
    }

    // One section per registered component type the entity carries, in registration order, so
    // components added by any module show up without inspector changes.
    ecs::register_builtin_components();
    const std::vector<std::type_index> carried = scene.registry().component_types(m_target);
    for (const ecs::ComponentTypeInfo& info : ecs::ComponentTypes::all()) {
        if (std::find(carried.begin(), carried.end(), info.type) == carried.end()) {
            continue;
        }
        m_sections.push_back(describeComponent(info.name, info.size, scene.registry().get_raw(m_target, info.type)));
    }
}

void PropertyInspector::syncRuntime(const EditorState& state, const scene::Scene& scene) {
    m_sections.clear();
    m_target = state.primarySelection;

    if (!m_target.valid() || scene.entityAt(m_target.index) == nullptr) {
        m_target = ecs::EntityID::null();
        return;
    }

    m_sections.push_back({"name", 1u});
}

bool PropertyInspector::getName(const scene::Scene& scene, std::string& out) const {
    if (!m_target.valid()) {
        return false;
    }

    const scene::SceneEntity* entity = scene.entityAt(m_target.index);
    if (entity == nullptr) {
        return false;
    }

    out = entity->name;
    return true;
}

bool PropertyInspector::setName(std::string name, scene::Scene& scene, CommandQueue& queue) {
    if (!m_target.valid()) {
        return false;
    }

    scene::SceneEntity* entity = scene.entityAt(m_target.index);
    if (entity == nullptr) {
        return false;
    }

    entity->name = std::move(name);

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "name";
    command.propertyValue = entity->name;
    queue.post(std::move(command));
    return true;
}

bool PropertyInspector::setName(std::string name, scene::Scene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    scene::SceneEntity* entity = scene.entityAt(m_target.index);
    if (entity == nullptr) {
        return false;
    }

    const std::string before = entity->name;
    entity->name = std::move(name);

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "name";
    command.propertyValue = entity->name;
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::setTransformPosition(const ecs::vec3& position, EditorScene& scene,
                                             CommandStack& cmds) {
    if (!m_target.valid() || !scene.registry().has<ecs::Transform>(m_target)) {
        return false;
    }

    ecs::Transform* transform = scene.registry().get<ecs::Transform>(m_target);
    if (transform == nullptr) {
        return false;
    }

    const std::string before = formatPropertyFloat(transform->position.x) + "," +
                               formatPropertyFloat(transform->position.y) + "," +
                               formatPropertyFloat(transform->position.z);

    transform->position = position;
    transform->dirty = true;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "transform.position";
    command.propertyValue = formatPropertyFloat(position.x) + "," +
                            formatPropertyFloat(position.y) + "," + formatPropertyFloat(position.z);
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::getMeshMaterialId(const EditorScene& scene, u32& out) const {
    if (!m_target.valid()) {
        return false;
    }

    const ecs::Mesh* mesh = scene.registry().get<ecs::Mesh>(m_target);
    if (mesh == nullptr) {
        return false;
    }

    out = mesh->material_id;
    return true;
}

bool PropertyInspector::tryGetMeshMaterialId(const EditorScene& scene, u32 catalogCount,
                                             u32& out) const {
    if (!getMeshMaterialId(scene, out)) {
        return false;
    }

    if (isMaterialCatalogEmpty(catalogCount) || isInvalidMaterialSlot(out, catalogCount)) {
        return false;
    }

    return true;
}

bool PropertyInspector::setMeshMaterialId(u32 materialId, EditorScene& scene, CommandStack& cmds) {
    return trySetMeshMaterialId(materialId, UINT32_MAX, scene, cmds);
}

bool PropertyInspector::trySetMeshMaterialId(u32 materialId, u32 catalogCount, EditorScene& scene,
                                             CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::Mesh* mesh = scene.registry().get<ecs::Mesh>(m_target);
    if (mesh == nullptr) {
        return false;
    }

    if (catalogCount != UINT32_MAX &&
        (isMaterialCatalogEmpty(catalogCount) || isInvalidMaterialSlot(materialId, catalogCount))) {
        return false;
    }

    const std::string before = std::to_string(mesh->material_id);
    mesh->material_id = materialId;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "mesh.material_id";
    command.propertyValue = std::to_string(materialId);
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::setSdfBlendAlpha(f32 alpha, EditorScene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::SDFObject* sdf = scene.registry().get<ecs::SDFObject>(m_target);
    if (sdf == nullptr) {
        return false;
    }

    const std::string before = formatPropertyFloat(sdf->blend_alpha);
    sdf->blend_alpha = alpha;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "sdf.blend_alpha";
    command.propertyValue = formatPropertyFloat(alpha);
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::setDirectionalIntensity(f32 intensity, EditorScene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::DirectionalLight* light = scene.registry().get<ecs::DirectionalLight>(m_target);
    if (light == nullptr) {
        return false;
    }

    const std::string before = formatPropertyFloat(light->intensity);
    light->intensity = intensity;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "directional.intensity";
    command.propertyValue = formatPropertyFloat(intensity);
    cmds.push(std::move(command), before);
    return true;
}

bool PropertyInspector::setSpotIntensity(f32 intensity, EditorScene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::SpotLight* light = scene.registry().get<ecs::SpotLight>(m_target);
    if (light == nullptr) {
        return false;
    }

    const std::string before = formatPropertyFloat(light->intensity);
    light->intensity = intensity;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "spot.intensity";
    command.propertyValue = formatPropertyFloat(intensity);
    cmds.push(std::move(command), before);
    return true;
}

ecs::vec3 PropertyInspector::sdfParamsForType(ecs::SDFPrimitive from, ecs::SDFPrimitive to,
                                              const ecs::vec3& params) {
    // Characteristic size of the old shape: radius / major radius / largest half extent.
    f32 size = params.x;
    if (from == ecs::SDFPrimitive::Box) {
        size = std::max(params.x, std::max(params.y, params.z));
    }
    if (!(size > 0.f)) {
        size = 1.f;
    }
    ecs::vec3 out = params;
    if (!(out.x > 0.f)) {
        out.x = size;
    }
    switch (to) {
    case ecs::SDFPrimitive::Box:
        if (!(out.y > 0.f)) {
            out.y = size;
        }
        if (!(out.z > 0.f)) {
            out.z = size;
        }
        break;
    case ecs::SDFPrimitive::Capsule:
    case ecs::SDFPrimitive::Cylinder:
        if (!(out.y > 0.f)) {
            out.y = size;
        }
        break;
    case ecs::SDFPrimitive::Torus:
        if (!(out.y > 0.f)) {
            out.y = 0.25f * size;
        }
        break;
    case ecs::SDFPrimitive::Sphere:
    case ecs::SDFPrimitive::Custom:
        break;
    }
    return out;
}

std::string PropertyInspector::formatSdfShape(ecs::SDFPrimitive type, const ecs::vec3& params) {
    return std::to_string(static_cast<u32>(type)) + ";" + formatPropertyFloat(params.x) + "," +
           formatPropertyFloat(params.y) + "," + formatPropertyFloat(params.z);
}

bool PropertyInspector::parseSdfShape(const std::string& text, ecs::SDFPrimitive& type, ecs::vec3& params) {
    const usize semi = text.find(';');
    if (semi == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    const unsigned long raw = std::strtoul(text.c_str(), &end, 10);
    if (end != text.c_str() + semi || raw > static_cast<unsigned long>(ecs::SDFPrimitive::Custom)) {
        return false;
    }
    f32 values[3]{};
    const char* cursor = text.c_str() + semi + 1;
    for (u32 i = 0; i < 3u; ++i) {
        values[i] = std::strtof(cursor, &end);
        if (end == cursor || (i < 2u && *end != ',') || (i == 2u && *end != '\0')) {
            return false;
        }
        cursor = end + 1;
    }
    type = static_cast<ecs::SDFPrimitive>(raw);
    params = {values[0], values[1], values[2], params.w};
    return true;
}

bool PropertyInspector::setSdfType(ecs::SDFPrimitive type, EditorScene& scene, CommandStack& cmds) {
    if (!m_target.valid()) {
        return false;
    }

    ecs::SDFObject* sdf = scene.registry().get<ecs::SDFObject>(m_target);
    if (sdf == nullptr) {
        return false;
    }

    const std::string before = formatSdfShape(sdf->type, sdf->params);
    sdf->params = sdfParamsForType(sdf->type, type, sdf->params);
    sdf->type = type;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.target = Handle<Object>(m_target.index, m_target.generation);
    command.propertyName = "sdf.shape";
    command.propertyValue = formatSdfShape(sdf->type, sdf->params);
    cmds.push(std::move(command), before);
    return true;
}

} // namespace fuse::editor
