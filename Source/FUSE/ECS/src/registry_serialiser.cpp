#include <fuse/ecs/registry_serialiser.hpp>

#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <typeindex>

namespace fuse::ecs {

/// One archetype block as read from disk, with component types resolved.
struct ArchetypeImage {
    std::vector<const ComponentTypeInfo*> components; // file order (sorted by name)
    std::vector<EntityID> entities;
    std::vector<std::vector<std::byte>> columns; // parallel to `components`
};

struct RegistryImage {
    std::vector<u32> generations;
    std::vector<u8> alive;
    std::vector<u32> freeList;
    std::vector<ArchetypeImage> archetypes;
    usize aliveCount = 0;
};

namespace {

constexpr usize kHeaderBytes = 64;

class Writer {
public:
    template <typename T>
    void put(const T& value) {
        const auto* p = reinterpret_cast<const char*>(&value);
        m_bytes.insert(m_bytes.end(), p, p + sizeof(T));
    }
    void bytes(const void* data, usize size) {
        const auto* p = static_cast<const char*>(data);
        m_bytes.insert(m_bytes.end(), p, p + size);
    }
    std::vector<char>& data() { return m_bytes; }

private:
    std::vector<char> m_bytes;
};

class Reader {
public:
    explicit Reader(const std::vector<char>& bytes) : m_bytes(bytes) {}
    template <typename T>
    bool get(T& value) {
        return bytes(&value, sizeof(T));
    }
    bool bytes(void* out, usize size) {
        if (m_offset + size > m_bytes.size()) {
            return false;
        }
        std::memcpy(out, m_bytes.data() + m_offset, size);
        m_offset += size;
        return true;
    }
    bool skip(usize size) {
        if (m_offset + size > m_bytes.size()) {
            return false;
        }
        m_offset += size;
        return true;
    }

private:
    const std::vector<char>& m_bytes;
    usize m_offset = 0;
};

/// Per-type column fixups for components whose bytes carry runtime-only state or bounded strings.
/// Script (MP-B7.3-SCRIPT-COMPONENT): the module path + exposed-property table are stored as-is, but
/// `started` / `lua_ref` belong to the live ScriptRuntime instance, so they are written cleared and
/// cleared again on load (a loaded scene re-attaches its behaviours through ScriptSystem). On load the
/// strings must be NUL-terminated inside their buffers and the property table well-formed.
void scrub_script_runtime_state(Script& script) {
    script.started = false;
    script.lua_ref = Script::kNoRef;
}

bool terminated(const char* text, usize capacity) {
    return std::memchr(text, 0, capacity) != nullptr;
}

bool validate_script(const Script& script, std::string& error) {
    if (!terminated(script.script_path, Script::kPathCapacity)) {
        error = "corrupt Script component: script_path is not NUL-terminated";
        return false;
    }
    if (script.property_count > Script::kMaxProperties) {
        error = "corrupt Script component: property_count " + std::to_string(script.property_count) + " exceeds " +
                std::to_string(Script::kMaxProperties);
        return false;
    }
    for (u32 i = 0; i < script.property_count; ++i) {
        const ScriptProperty& property = script.properties[i];
        if (!terminated(property.key, ScriptProperty::kKeyCapacity) ||
            !terminated(property.text, ScriptProperty::kTextCapacity)) {
            error = "corrupt Script component: property string is not NUL-terminated";
            return false;
        }
        if (property.key[0] == '\0' || static_cast<u8>(property.type) > static_cast<u8>(ScriptProperty::Type::Vec3)) {
            error = "corrupt Script component: bad property entry " + std::to_string(i);
            return false;
        }
    }
    return true;
}

/// Rewrites `count` Script rows in `rows` (byte storage). `loading` also validates each row.
bool fixup_script_rows(std::byte* rows, usize count, bool loading, std::string& error) {
    for (usize i = 0; i < count; ++i) {
        Script script;
        std::memcpy(&script, rows + i * sizeof(Script), sizeof(Script));
        if (loading && !validate_script(script, error)) {
            return false;
        }
        scrub_script_runtime_state(script);
        std::memcpy(rows + i * sizeof(Script), &script, sizeof(Script));
    }
    return true;
}

bool column_is(const ComponentTypeInfo* info, const char* name, usize size) {
    return info != nullptr && info->size == size && std::strcmp(info->name, name) == 0;
}

template <typename T>
void zero_init(T& value) {
    alignas(T) std::byte cleared[sizeof(T)]{};
    std::memcpy(&value, cleared, sizeof(T));
}

void scrub_mesh_runtime_state(Mesh& mesh) {
    Mesh out;
    zero_init(out);
    out.vertex_buffer = mesh.vertex_buffer;
    out.index_buffer = mesh.index_buffer;
    out.index_count = mesh.index_count;
    out.material_id = mesh.material_id;
    out.aabb_min = mesh.aabb_min;
    out.aabb_max = mesh.aabb_max;
    out.cast_shadow = mesh.cast_shadow;
    out.receive_shadow = mesh.receive_shadow;
    out.visible = mesh.visible;
    mesh = out;
}

void scrub_rigidbody_runtime_state(RigidBody& body) {
    RigidBody out;
    zero_init(out);
    out.velocity = body.velocity;
    out.angular_velocity = body.angular_velocity;
    out.force_accumulator = body.force_accumulator;
    out.torque_accumulator = body.torque_accumulator;
    out.mass = body.mass;
    out.inv_mass = body.inv_mass;
    out.restitution = body.restitution;
    out.linear_damping = body.linear_damping;
    out.angular_damping = body.angular_damping;
    out.is_static = body.is_static;
    out.is_sleeping = body.is_sleeping;
    out.sleep_timer = body.sleep_timer;
    body = out;
}

void scrub_sdf_object_runtime_state(SDFObject& sdf) {
    SDFObject out;
    zero_init(out);
    out.type = sdf.type;
    out.op = sdf.op;
    out.params = sdf.params;
    out.material_id = sdf.material_id;
    out.blend_alpha = sdf.blend_alpha;
    out.casts_shadow = sdf.casts_shadow;
    out.visible = sdf.visible;
    out.blend_radius = sdf.blend_radius;
    out.roughness = sdf.roughness;
    out.csg_order = sdf.csg_order;
    sdf = out;
}

void scrub_collider_runtime_state(Collider& collider) {
    Collider out;
    zero_init(out);
    out.shape = collider.shape;
    out.params = collider.params;
    out.scalar = collider.scalar;
    out.friction_static = collider.friction_static;
    out.friction_dynamic = collider.friction_dynamic;
    out.layer = collider.layer;
    out.mask = collider.mask;
    out.is_trigger = collider.is_trigger;
    out.ccd = collider.ccd;
    out.shape_asset = collider.shape_asset;
    out.shape_piece = collider.shape_piece;
    out.shape_ref = collider.shape_ref;
    collider = out;
}

void scrub_directional_light_runtime_state(DirectionalLight& light) {
    DirectionalLight out;
    zero_init(out);
    out.color = light.color;
    out.intensity = light.intensity;
    light = out;
}

void scrub_point_light_runtime_state(PointLight& light) {
    PointLight out;
    zero_init(out);
    out.color = light.color;
    out.intensity = light.intensity;
    out.radius = light.radius;
    light = out;
}

void scrub_spot_light_runtime_state(SpotLight& light) {
    SpotLight out;
    zero_init(out);
    out.color = light.color;
    out.intensity = light.intensity;
    out.inner_cone_deg = light.inner_cone_deg;
    out.outer_cone_deg = light.outer_cone_deg;
    out.radius = light.radius;
    light = out;
}

void fixup_mesh_rows(std::byte* rows, usize count) {
    for (usize i = 0; i < count; ++i) {
        Mesh mesh;
        std::memcpy(&mesh, rows + i * sizeof(Mesh), sizeof(Mesh));
        scrub_mesh_runtime_state(mesh);
        std::memcpy(rows + i * sizeof(Mesh), &mesh, sizeof(Mesh));
    }
}

void fixup_rigidbody_rows(std::byte* rows, usize count) {
    for (usize i = 0; i < count; ++i) {
        RigidBody body;
        std::memcpy(&body, rows + i * sizeof(RigidBody), sizeof(RigidBody));
        scrub_rigidbody_runtime_state(body);
        std::memcpy(rows + i * sizeof(RigidBody), &body, sizeof(RigidBody));
    }
}

void fixup_sdf_object_rows(std::byte* rows, usize count) {
    for (usize i = 0; i < count; ++i) {
        SDFObject sdf;
        std::memcpy(&sdf, rows + i * sizeof(SDFObject), sizeof(SDFObject));
        scrub_sdf_object_runtime_state(sdf);
        std::memcpy(rows + i * sizeof(SDFObject), &sdf, sizeof(SDFObject));
    }
}

void fixup_tag_rows(std::byte* rows, usize count, usize elementSize) {
    std::memset(rows, 0, count * elementSize);
}

void fixup_collider_rows(std::byte* rows, usize count) {
    for (usize i = 0; i < count; ++i) {
        Collider collider;
        std::memcpy(&collider, rows + i * sizeof(Collider), sizeof(Collider));
        scrub_collider_runtime_state(collider);
        std::memcpy(rows + i * sizeof(Collider), &collider, sizeof(Collider));
    }
}

void fixup_directional_light_rows(std::byte* rows, usize count) {
    for (usize i = 0; i < count; ++i) {
        DirectionalLight light;
        std::memcpy(&light, rows + i * sizeof(DirectionalLight), sizeof(DirectionalLight));
        scrub_directional_light_runtime_state(light);
        std::memcpy(rows + i * sizeof(DirectionalLight), &light, sizeof(DirectionalLight));
    }
}

void fixup_point_light_rows(std::byte* rows, usize count) {
    for (usize i = 0; i < count; ++i) {
        PointLight light;
        std::memcpy(&light, rows + i * sizeof(PointLight), sizeof(PointLight));
        scrub_point_light_runtime_state(light);
        std::memcpy(rows + i * sizeof(PointLight), &light, sizeof(PointLight));
    }
}

void fixup_spot_light_rows(std::byte* rows, usize count) {
    for (usize i = 0; i < count; ++i) {
        SpotLight light;
        std::memcpy(&light, rows + i * sizeof(SpotLight), sizeof(SpotLight));
        scrub_spot_light_runtime_state(light);
        std::memcpy(rows + i * sizeof(SpotLight), &light, sizeof(SpotLight));
    }
}

EntityID archetype_min_entity(const Archetype& archetype) {
    EntityID best{std::numeric_limits<u32>::max(), std::numeric_limits<u32>::max()};
    for (u32 row = 0; row < archetype.count(); ++row) {
        const EntityID id = archetype.entities[row];
        if (id.index < best.index || (id.index == best.index && id.generation < best.generation)) {
            best = id;
        }
    }
    return best;
}

void scrub_transform_runtime_state(Transform& transform) {
    const vec3 position = transform.position;
    const quat rotation = transform.rotation;
    const vec3 scale = transform.scale;
    const EntityID parent = transform.parent;
    const bool dirty = transform.dirty;
    zero_init(transform);
    transform.position = position;
    transform.rotation = rotation;
    transform.scale = scale;
    transform.parent = parent;
    transform.dirty = dirty;
    transform.local_to_world = mat4::identity();
    transform.world_to_local = mat4::identity();
}

void fixup_transform_rows(std::byte* rows, usize count) {
    for (usize i = 0; i < count; ++i) {
        Transform transform;
        std::memcpy(&transform, rows + i * sizeof(Transform), sizeof(Transform));
        scrub_transform_runtime_state(transform);
        std::memcpy(rows + i * sizeof(Transform), &transform, sizeof(Transform));
    }
}

bool fixup_column_rows(const ComponentTypeInfo* info, std::byte* rows, usize count, bool loading, std::string& error) {
    if (column_is(info, Script::component_name, sizeof(Script))) {
        return fixup_script_rows(rows, count, loading, error);
    }
    if (column_is(info, Transform::component_name, sizeof(Transform))) {
        fixup_transform_rows(rows, count);
        return true;
    }
    if (column_is(info, Mesh::component_name, sizeof(Mesh))) {
        fixup_mesh_rows(rows, count);
        return true;
    }
    if (column_is(info, RigidBody::component_name, sizeof(RigidBody))) {
        fixup_rigidbody_rows(rows, count);
        return true;
    }
    if (column_is(info, SDFObject::component_name, sizeof(SDFObject))) {
        fixup_sdf_object_rows(rows, count);
        return true;
    }
    if (column_is(info, Collider::component_name, sizeof(Collider))) {
        fixup_collider_rows(rows, count);
        return true;
    }
    if (column_is(info, DirectionalLight::component_name, sizeof(DirectionalLight))) {
        fixup_directional_light_rows(rows, count);
        return true;
    }
    if (column_is(info, PointLight::component_name, sizeof(PointLight))) {
        fixup_point_light_rows(rows, count);
        return true;
    }
    if (column_is(info, SpotLight::component_name, sizeof(SpotLight))) {
        fixup_spot_light_rows(rows, count);
        return true;
    }
    if (column_is(info, TagStatic::component_name, sizeof(TagStatic)) ||
        column_is(info, TagPlayer::component_name, sizeof(TagPlayer)) ||
        column_is(info, TagDestroy::component_name, sizeof(TagDestroy)) ||
        column_is(info, TagKinematic::component_name, sizeof(TagKinematic))) {
        fixup_tag_rows(rows, count, info->size);
        return true;
    }
    return true;
}

RegistrySerialiseResult fail(std::string message) {
    RegistrySerialiseResult result;
    result.error = std::move(message);
    return result;
}

} // namespace

RegistrySerialiseResult RegistrySerialiser::save(const Registry& registry, const std::string& path) {
    register_builtin_components();

    // Resolve every non-empty archetype's component names up front; unknown types abort the save.
    struct Block {
        const Archetype* archetype;
        std::vector<const ComponentTypeInfo*> components;
    };
    std::vector<Block> blocks;
    for (const Archetype& archetype : registry.m_archetypes) {
        if (archetype.count() == 0u) {
            continue;
        }
        Block block{&archetype, {}};
        for (const std::type_index& type : archetype.component_types) {
            const ComponentTypeInfo* info = ComponentTypes::find(type);
            if (info == nullptr) {
                return fail(std::string("component type not registered for serialisation: ") + type.name());
            }
            block.components.push_back(info);
        }
        std::sort(block.components.begin(), block.components.end(),
                  [](const ComponentTypeInfo* a, const ComponentTypeInfo* b) { return std::strcmp(a->name, b->name) < 0; });
        blocks.push_back(std::move(block));
    }
    std::stable_sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) {
        if (a.components.size() != b.components.size()) {
            return a.components.size() < b.components.size();
        }
        for (usize i = 0; i < a.components.size(); ++i) {
            const int cmp = std::strcmp(a.components[i]->name, b.components[i]->name);
            if (cmp != 0) {
                return cmp < 0;
            }
        }
        const EntityID ea = archetype_min_entity(*a.archetype);
        const EntityID eb = archetype_min_entity(*b.archetype);
        if (ea.index != eb.index) {
            return ea.index < eb.index;
        }
        return ea.generation < eb.generation;
    });

    Writer w;
    w.put(kMagic);
    w.put(kVersion);
    w.put(static_cast<u32>(registry.m_alive_count));
    w.put(static_cast<u32>(blocks.size()));
    w.put(static_cast<u32>(registry.m_records.size()));
    w.put(static_cast<u32>(registry.m_free_list.size()));
    w.data().resize(kHeaderBytes, 0);

    for (const auto& rec : registry.m_records) {
        w.put(rec.generation);
        w.put(static_cast<u8>(rec.alive ? 1u : 0u));
    }
    std::vector<u32> freeList = registry.m_free_list;
    std::sort(freeList.begin(), freeList.end());
    for (const u32 index : freeList) {
        w.put(index);
    }

    for (const Block& block : blocks) {
        const Archetype& archetype = *block.archetype;
        w.put(static_cast<u32>(block.components.size()));
        w.put(static_cast<u32>(archetype.count()));
        for (const ComponentTypeInfo* info : block.components) {
            const u16 nameLength = static_cast<u16>(std::strlen(info->name));
            w.put(nameLength);
            w.bytes(info->name, nameLength);
            w.put(static_cast<u32>(info->size));
        }
        const u32 rowCount = archetype.count();
        std::vector<u32> rowOrder(rowCount);
        for (u32 row = 0; row < rowCount; ++row) {
            rowOrder[row] = row;
        }
        std::stable_sort(rowOrder.begin(), rowOrder.end(), [&](u32 a, u32 b) {
            const EntityID ea = archetype.entities[a];
            const EntityID eb = archetype.entities[b];
            if (ea.index != eb.index) {
                return ea.index < eb.index;
            }
            return ea.generation < eb.generation;
        });

        std::vector<EntityID> sortedEntities;
        sortedEntities.reserve(rowCount);
        for (const u32 row : rowOrder) {
            sortedEntities.push_back(archetype.entities[row]);
        }
        w.bytes(sortedEntities.data(), sortedEntities.size() * sizeof(EntityID));

        for (const ComponentTypeInfo* info : block.components) {
            const ComponentColumn* column = archetype.find_column(info->type);
            if (column == nullptr || column->storage.size() != archetype.count() * info->size) {
                return fail(std::string("column size mismatch for ") + info->name);
            }
            const usize rowBytes = info->size;
            std::vector<std::byte> sortedColumn(rowCount * rowBytes);
            for (u32 outRow = 0; outRow < rowCount; ++outRow) {
                const u32 srcRow = rowOrder[outRow];
                std::memcpy(sortedColumn.data() + outRow * rowBytes, column->storage.data() + srcRow * rowBytes,
                            rowBytes);
            }
            {
                std::string ignored;
                if (!fixup_column_rows(info, sortedColumn.data(), rowCount, false, ignored)) {
                    return fail(ignored.empty() ? std::string("column fixup failed for ") + info->name : ignored);
                }
            }
            w.bytes(sortedColumn.data(), sortedColumn.size());
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail("cannot open for write: " + path);
    }
    out.write(w.data().data(), static_cast<std::streamsize>(w.data().size()));
    if (!out) {
        return fail("write failed: " + path);
    }

    RegistrySerialiseResult result;
    result.ok = true;
    result.entityCount = registry.m_alive_count;
    result.archetypeCount = blocks.size();
    return result;
}

RegistrySerialiseResult RegistrySerialiser::parse(const std::string& path, RegistryImage& image) {
    register_builtin_components();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail("cannot open: " + path);
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Reader r(bytes);

    u32 magic = 0, version = 0, aliveCount = 0, archetypeCount = 0, recordCount = 0, freeCount = 0;
    if (!r.get(magic) || !r.get(version) || !r.get(aliveCount) || !r.get(archetypeCount) || !r.get(recordCount) ||
        !r.get(freeCount) || !r.skip(kHeaderBytes - 6u * sizeof(u32))) {
        return fail("truncated header");
    }
    if (magic != kMagic) {
        return fail("bad magic (not a FUSE ECS scene)");
    }
    if (version != kVersion) {
        return fail("unsupported version " + std::to_string(version));
    }

    image = {};
    image.aliveCount = aliveCount;
    image.generations.resize(recordCount);
    image.alive.resize(recordCount);
    for (u32 i = 0; i < recordCount; ++i) {
        if (!r.get(image.generations[i]) || !r.get(image.alive[i])) {
            return fail("truncated entity records");
        }
    }
    image.freeList.resize(freeCount);
    for (u32 i = 0; i < freeCount; ++i) {
        if (!r.get(image.freeList[i]) || image.freeList[i] >= recordCount) {
            return fail("bad free list");
        }
    }

    usize entitiesSeen = 0;
    image.archetypes.resize(archetypeCount);
    for (ArchetypeImage& block : image.archetypes) {
        u32 componentCount = 0, rows = 0;
        if (!r.get(componentCount) || !r.get(rows)) {
            return fail("truncated archetype header");
        }
        for (u32 c = 0; c < componentCount; ++c) {
            u16 nameLength = 0;
            if (!r.get(nameLength)) {
                return fail("truncated component name");
            }
            std::string name(nameLength, '\0');
            u32 size = 0;
            if (!r.bytes(name.data(), nameLength) || !r.get(size)) {
                return fail("truncated component entry");
            }
            const ComponentTypeInfo* info = ComponentTypes::find(name);
            if (info == nullptr) {
                return fail("unknown component type in file: " + name);
            }
            if (info->size != size) {
                return fail("component size changed since save: " + name);
            }
            block.components.push_back(info);
        }
        block.entities.resize(rows);
        if (!r.bytes(block.entities.data(), rows * sizeof(EntityID))) {
            return fail("truncated entity ids");
        }
        for (const EntityID id : block.entities) {
            if (id.index >= recordCount || image.alive[id.index] == 0u || image.generations[id.index] != id.generation) {
                return fail("archetype entity does not match the record table");
            }
        }
        for (const ComponentTypeInfo* info : block.components) {
            std::vector<std::byte>& column = block.columns.emplace_back(rows * info->size);
            if (!r.bytes(column.data(), column.size())) {
                return fail(std::string("truncated column ") + info->name);
            }
            {
                std::string error;
                if (!fixup_column_rows(info, column.data(), rows, true, error)) {
                    return fail(error.empty() ? std::string("column fixup failed for ") + info->name : error);
                }
            }
        }
        entitiesSeen += rows;
    }
    // Entities with no components live in the empty archetype, which is not stored.
    RegistrySerialiseResult result;
    result.ok = entitiesSeen <= aliveCount;
    result.entityCount = aliveCount;
    result.archetypeCount = archetypeCount;
    if (!result.ok) {
        result.error = "more archetype rows than live entities";
    }
    return result;
}

RegistrySerialiseResult RegistrySerialiser::apply(const RegistryImage& image, Registry& registry) {
    registry.init(std::max<usize>(kMaxEntities, image.generations.size()));
    registry.m_records.resize(image.generations.size());
    for (usize i = 0; i < image.generations.size(); ++i) {
        auto& rec = registry.m_records[i];
        rec.generation = image.generations[i];
        rec.alive = false;
        rec.reserved = false;
        rec.archetype_index = 0;
        rec.row = 0;
    }
    registry.m_free_list = image.freeList;
    // Dead slots that are not on the free list were reserved (destroy_entity_reserved) when saved;
    // keep them reserved so create() still cannot reuse them and create_at can revive them.
    std::vector<u8> onFreeList(image.generations.size(), 0u);
    for (const u32 index : image.freeList) {
        onFreeList[index] = 1u;
    }
    for (usize i = 0; i < image.generations.size(); ++i) {
        registry.m_records[i].reserved = image.alive[i] == 0u && onFreeList[i] == 0u && image.generations[i] != 0u;
    }

    usize placed = 0;
    for (const ArchetypeImage& block : image.archetypes) {
        std::vector<std::type_index> types;
        for (const ComponentTypeInfo* info : block.components) {
            types.push_back(info->type);
        }
        std::sort(types.begin(), types.end());
        // Registry::init() already owns the canonical empty archetype at index 0 (ArchetypeID{0}).
        const u32 archetypeIndex = types.empty() ? 0u : registry.find_or_create_archetype(types);
        Archetype& archetype = registry.m_archetypes[archetypeIndex];
        const u32 firstRow = static_cast<u32>(archetype.count());
        for (usize c = 0; c < block.components.size(); ++c) {
            ComponentColumn& column = archetype.ensure_column(block.components[c]->type, block.components[c]->size,
                                                              registry.column_memory_resource(block.components[c]->type));
            column.storage.insert(column.storage.end(), block.columns[c].begin(), block.columns[c].end());
        }
        for (usize row = 0; row < block.entities.size(); ++row) {
            const EntityID id = block.entities[row];
            archetype.entities.push_back(id);
            auto& rec = registry.m_records[id.index];
            rec.alive = true;
            rec.archetype_index = archetypeIndex;
            rec.row = firstRow + static_cast<u32>(row);
        }
        placed += block.entities.size();
    }

    // Remaining live records carry no components: they belong to the empty archetype (index 0).
    for (usize i = 0; i < image.generations.size(); ++i) {
        auto& rec = registry.m_records[i];
        if (image.alive[i] != 0u && !rec.alive) {
            rec.alive = true;
            rec.archetype_index = 0;
            rec.row = static_cast<u32>(registry.m_archetypes[0].append_entity(EntityID{static_cast<u32>(i), rec.generation}));
            ++placed;
        }
    }
    registry.m_alive_count = placed;

    RegistrySerialiseResult result;
    result.ok = placed == image.aliveCount;
    result.entityCount = placed;
    result.archetypeCount = image.archetypes.size();
    if (!result.ok) {
        result.error = "restored entity count does not match header";
    }
    return result;
}

RegistrySerialiseResult RegistrySerialiser::load(const std::string& path, Registry& registry) {
    RegistryImage image;
    const RegistrySerialiseResult parsed = parse(path, image);
    if (!parsed.ok) {
        return parsed;
    }
    return apply(image, registry);
}

struct RegistryLoadQueue::Request {
    std::string path;
    Callback callback;
    RegistryImage image;
    RegistrySerialiseResult parsed;
    fuse::jobs::JobCounter done{1};
};

RegistryLoadQueue::RegistryLoadQueue() = default;

RegistryLoadQueue::~RegistryLoadQueue() {
    std::vector<std::shared_ptr<Request>> requests;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        requests = m_requests;
    }
    for (const auto& request : requests) {
        request->done.wait();
    }
}

void RegistryLoadQueue::load_async(const std::string& path, Callback onComplete) {
    auto request = std::make_shared<Request>();
    request->path = path;
    request->callback = std::move(onComplete);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requests.push_back(request);
    }
    // Parse off-thread; the job only touches its own Request (shared ownership keeps it alive).
    fuse::jobs::JobScheduler::instance().submit([request]() {
        request->parsed = RegistrySerialiser::parse(request->path, request->image);
        request->done.signal();
    });
}

u32 RegistryLoadQueue::pump(Registry& registry) {
    std::vector<std::shared_ptr<Request>> finished;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto split = std::stable_partition(m_requests.begin(), m_requests.end(),
                                           [](const std::shared_ptr<Request>& r) { return !r->done.isComplete(); });
        finished.assign(split, m_requests.end());
        m_requests.erase(split, m_requests.end());
    }
    for (const auto& request : finished) {
        request->done.wait(); // completion is already published; this orders the parse results
        RegistrySerialiseResult result =
            request->parsed.ok ? RegistrySerialiser::apply(request->image, registry) : request->parsed;
        if (request->callback) {
            request->callback(result);
        }
    }
    return static_cast<u32>(finished.size());
}

u32 RegistryLoadQueue::pending() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<u32>(m_requests.size());
}

} // namespace fuse::ecs
