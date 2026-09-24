#include <fuse/renderer/gpu_scene/gpu_scene_ecs.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_extract_kernel.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::renderer::gpu_scene {

namespace {

constexpr f32 kDegToRad = 0.017453292519943295f;

void lightFrame(const ecs::Transform& t, GpuLight& out) {
    const auto& m = t.local_to_world.data;
    out.position[0] = m[12];
    out.position[1] = m[13];
    out.position[2] = m[14];
    // Lights shine down their local -Z axis.
    f32 d[3] = {-m[8], -m[9], -m[10]};
    const f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const f32 inv = len > 0.f ? 1.f / len : 0.f;
    for (u32 a = 0; a < 3u; ++a) {
        out.direction[a] = len > 0.f ? d[a] * inv : (a == 2u ? -1.f : 0.f);
    }
}

void lightColor(const ecs::vec3& color, f32 intensity, GpuLight& out) {
    out.color[0] = color.x;
    out.color[1] = color.y;
    out.color[2] = color.z;
    out.intensity = intensity;
}

GpuLight packLight(ecs::EntityID id, const ecs::Transform& t, const ecs::PointLight& l) {
    GpuLight out{};
    out.type = static_cast<u32>(GpuLightType::Point);
    lightFrame(t, out);
    lightColor(l.color, l.intensity, out);
    out.range = l.radius;
    out.entityIndex = id.index;
    return out;
}

GpuLight packLight(ecs::EntityID id, const ecs::Transform& t, const ecs::SpotLight& l) {
    GpuLight out{};
    out.type = static_cast<u32>(GpuLightType::Spot);
    lightFrame(t, out);
    lightColor(l.color, l.intensity, out);
    out.range = l.radius;
    out.cosInner = std::cos(l.inner_cone_deg * kDegToRad);
    out.cosOuter = std::cos(l.outer_cone_deg * kDegToRad);
    out.entityIndex = id.index;
    return out;
}

GpuLight packLight(ecs::EntityID id, const ecs::Transform& t, const ecs::DirectionalLight& l) {
    GpuLight out{};
    out.type = static_cast<u32>(GpuLightType::Directional);
    lightFrame(t, out);
    lightColor(l.color, l.intensity, out);
    out.entityIndex = id.index;
    return out;
}

} // namespace

// --- EntityMap -----------------------------------------------------------------------------------

void GpuSceneEcsExtractor::EntityMap::reserve(u32 entities) {
    if (entities > m_entries.size()) {
        m_entries.resize(entities);
        m_slot.resize(entities, kInvalidIndex);
    }
    m_list.reserve(entities);
}

GpuSceneEcsExtractor::EntityMap::Entry& GpuSceneEcsExtractor::EntityMap::entry(u32 index) {
    if (index >= m_entries.size()) {
        const usize grown = std::max<usize>(static_cast<usize>(index) + 1u, m_entries.size() * 2u);
        m_entries.resize(grown);
        m_slot.resize(grown, kInvalidIndex);
    }
    return m_entries[index];
}

SlotAllocator::Handle GpuSceneEcsExtractor::EntityMap::handleOf(ecs::EntityID id) const {
    if (id.index >= m_entries.size() || m_entries[id.index].generation != id.generation || id.generation == 0u) {
        return SlotAllocator::Handle{};
    }
    return m_entries[id.index].handle;
}

void GpuSceneEcsExtractor::EntityMap::track(ecs::EntityID id, SlotAllocator::Handle handle, u32 frame) {
    Entry& e = entry(id.index);
    e.generation = id.generation;
    e.handle = handle;
    e.lastSeen = frame;
    e.listPos = static_cast<u32>(m_list.size());
    m_list.push_back(id.index);
    m_slot[id.index] = handle.slot;
}

void GpuSceneEcsExtractor::EntityMap::untrack(u32 index) {
    Entry& e = m_entries[index];
    const u32 pos = e.listPos;
    const u32 last = m_list.back();
    m_list[pos] = last;
    m_entries[last].listPos = pos;
    m_list.pop_back();
    e = Entry{};
    m_slot[index] = kInvalidIndex;
}

// --- extractor -----------------------------------------------------------------------------------

void GpuSceneEcsExtractor::init(const EcsExtractDesc& desc) {
    m_desc = desc;
    m_instances.reserve(desc.entityCapacity);
    if (desc.extractLights) {
        m_lights.reserve(std::min<u32>(desc.entityCapacity, 4096u));
    }
}

void GpuSceneEcsExtractor::extractChunk(GpuScene& scene, std::span<const ecs::EntityID> ids,
                                        std::span<const ecs::Transform> transforms, std::span<const ecs::Mesh> meshes,
                                        u32 extraFlags, EcsExtractStats& stats) {
    const kernel::Span<const u32> remap{m_desc.meshRemap, m_desc.meshRemap != nullptr ? m_desc.meshRemapCount : 0u};
    const u32 n = static_cast<u32>(ids.size());
    // Serial liveness: slot allocation touches shared GpuScene state.
    for (u32 i = 0; i < n; ++i) {
        const ecs::EntityID id = ids[i];
        EntityMap::Entry& e = m_instances.entry(id.index);
        if (e.generation != 0u) {
            if (e.generation == id.generation && scene.alive(e.handle)) {
                e.lastSeen = m_frame;
                continue;
            }
            scene.removeInstance(e.handle); // index reused by a new entity generation
            m_instances.untrack(id.index);
            ++stats.removed;
        }
        InstanceDesc desc{};
        desc.mesh = extract_kernel::resolve_mesh(meshes[i], remap);
        desc.material = meshes[i].material_id;
        desc.flags = extract_kernel::instance_flags(meshes[i], extraFlags);
        desc.transform = extract_kernel::pack_transform(transforms[i]);
        desc.entityIndex = id.index;
        desc.entityGeneration = id.generation;
        const InstanceHandle handle = scene.addInstance(desc);
        if (handle.valid()) {
            m_instances.track(id, handle, m_frame);
            ++stats.added;
        }
    }
    stats.entities += n;
    if (n == 0u) {
        return;
    }
    // Parallel pack + diff (views taken after the adds: the tables may have grown).
    u32 counters[2] = {0u, 0u};
    extract_kernel::Params p{};
    p.ids = kernel::Span<const ecs::EntityID>{ids.data(), n};
    p.transforms = kernel::Span<const ecs::Transform>{transforms.data(), n};
    p.meshes = kernel::Span<const ecs::Mesh>{meshes.data(), n};
    p.entitySlot = kernel::Span<const u32>{m_instances.slots().data(), static_cast<u32>(m_instances.slots().size())};
    p.meshRemap = remap;
    p.instances = scene.instanceView();
    p.transformsOut = scene.transformView();
    p.extraFlags = extraFlags;
    p.counters = counters;
    kernel::launch(m_desc.backend, extract_kernel::make_launch(n), extract_kernel::Kernel{}, p);
    ++stats.kernelLaunches;
    stats.instanceWrites += counters[0];
    stats.transformWrites += counters[1];
}

template <typename LightT>
void GpuSceneEcsExtractor::extractLights(ecs::Registry& registry, GpuScene& scene, EcsExtractStats& stats) {
    registry.each_chunk<ecs::Transform, LightT>(
        [&](std::span<const ecs::EntityID> ids, std::span<ecs::Transform> transforms, std::span<LightT> lights) {
            for (usize i = 0; i < ids.size(); ++i) {
                const ecs::EntityID id = ids[i];
                const GpuLight packed = packLight(id, transforms[i], lights[i]);
                EntityMap::Entry& e = m_lights.entry(id.index);
                if (e.generation != 0u && e.generation == id.generation && scene.lightAlive(e.handle)) {
                    if (e.lastSeen == m_frame) {
                        continue; // several light components on one entity: the first one wins
                    }
                    e.lastSeen = m_frame;
                    if (std::memcmp(&scene.light(e.handle.slot), &packed, sizeof(GpuLight)) != 0) {
                        scene.setLight(e.handle, packed);
                        ++stats.lightWrites;
                    }
                    ++stats.lights;
                    continue;
                }
                if (e.generation != 0u) {
                    scene.removeLight(e.handle);
                    m_lights.untrack(id.index);
                    ++stats.lightsRemoved;
                }
                const LightHandle handle = scene.addLight(packed);
                if (handle.valid()) {
                    m_lights.track(id, handle, m_frame);
                    ++stats.lightsAdded;
                    ++stats.lights;
                }
            }
        });
}

EcsExtractStats GpuSceneEcsExtractor::extract(ecs::Registry& registry, GpuScene& scene) {
    EcsExtractStats stats{};
    if (++m_frame == 0u) {
        m_frame = 1u; // 0 means "never seen"
    }
    registry.each_chunk<ecs::Transform, ecs::Mesh>(
        [&](std::span<const ecs::EntityID> ids, std::span<ecs::Transform> transforms, std::span<ecs::Mesh> meshes) {
            extractChunk(scene, ids, transforms, meshes, 0u, stats);
        },
        ecs::Without<ecs::TagStatic>{});
    registry.each_chunk<ecs::Transform, ecs::Mesh, ecs::TagStatic>(
        [&](std::span<const ecs::EntityID> ids, std::span<ecs::Transform> transforms, std::span<ecs::Mesh> meshes,
            std::span<ecs::TagStatic>) { extractChunk(scene, ids, transforms, meshes, kInstanceStatic, stats); });

    // Tracked entities not seen this frame lost their components or died.
    const std::vector<u32>& tracked = m_instances.list();
    for (usize i = tracked.size(); i-- > 0;) {
        const u32 index = tracked[i];
        EntityMap::Entry& e = m_instances.entry(index);
        if (e.lastSeen != m_frame) {
            scene.removeInstance(e.handle);
            m_instances.untrack(index);
            ++stats.removed;
        }
    }

    if (m_desc.extractLights) {
        extractLights<ecs::PointLight>(registry, scene, stats);
        extractLights<ecs::SpotLight>(registry, scene, stats);
        extractLights<ecs::DirectionalLight>(registry, scene, stats);
        const std::vector<u32>& lights = m_lights.list();
        for (usize i = lights.size(); i-- > 0;) {
            const u32 index = lights[i];
            EntityMap::Entry& e = m_lights.entry(index);
            if (e.lastSeen != m_frame) {
                scene.removeLight(e.handle);
                m_lights.untrack(index);
                ++stats.lightsRemoved;
            }
        }
    }
    return stats;
}

void GpuSceneEcsExtractor::releaseAll(GpuScene& scene) {
    while (!m_instances.list().empty()) {
        const u32 index = m_instances.list().back();
        scene.removeInstance(m_instances.entry(index).handle);
        m_instances.untrack(index);
    }
    while (!m_lights.list().empty()) {
        const u32 index = m_lights.list().back();
        scene.removeLight(m_lights.entry(index).handle);
        m_lights.untrack(index);
    }
}

} // namespace fuse::renderer::gpu_scene
