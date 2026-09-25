// FUSE Relight RL-4.1: SceneModel / replaced scene -> GPU scene (see scene_adapter.hpp).
#include <fuse/relight/render/frame/scene_adapter.hpp>

#include <fuse/renderer/material/material.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::relight::render::frame {

namespace gs = fuse::renderer::gpu_scene;
namespace inst = fuse::relight::scene::instances;

namespace {

std::uint64_t mixKey(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t v[2] = {a, b};
    return hash::xxh3_64(v, sizeof(v));
}

std::uint64_t stringKey(const std::string& s, std::uint64_t salt) { return hash::xxh3_64(s.data(), s.size(), salt); }

// Distinct key spaces for geometry and materials.
constexpr std::uint64_t kSaltMesh = 0x6d657368ull;     // "mesh"
constexpr std::uint64_t kSaltMaterial = 0x6d61746cull; // "matl"

gs::GpuTransform toGpuTransform(const inst::Mat4f& m) {
    // Mat4f is a D3D row-vector matrix (translation in m[12..14]); as memory that is the column-major
    // layout transformFromColumnMajor expects.
    gs::GpuTransform t;
    gs::transformFromColumnMajor(m.data(), t);
    return t;
}

inst::Mat4f toMat4f(const replace::Mat4d& m) {
    inst::Mat4f r{};
    for (std::size_t i = 0; i < 16; ++i) {
        r[i] = static_cast<float>(m[i]);
    }
    return r;
}

} // namespace

GpuSceneAdapter::GpuSceneAdapter(gs::GpuScene& scene, TextureHandleFn textureHandle)
    : m_scene(scene), m_textureHandle(std::move(textureHandle)) {}

void GpuSceneAdapter::beginFrame(std::uint64_t serial) {
    m_serial = serial;
    ++m_frame;
    const std::uint32_t meshes = m_stats.meshes, materials = m_stats.materials;
    m_stats = AdapterStats{};
    m_stats.serial = serial;
    m_stats.meshes = meshes;
    m_stats.materials = materials;
    m_scene.beginFrame(serial);
}

std::uint32_t GpuSceneAdapter::meshRow(std::uint64_t key, const inst::AxisAlignedBoundingBox& bounds) {
    auto it = m_meshes.find(key);
    if (it != m_meshes.end()) {
        return it->second;
    }
    gs::GpuMesh mesh;
    if (bounds.isValid()) {
        const inst::Vec3 c = bounds.getCentroid();
        const inst::Vec3 e = (bounds.maxPos - bounds.minPos) * 0.5f;
        mesh.boundsCenter[0] = c.x;
        mesh.boundsCenter[1] = c.y;
        mesh.boundsCenter[2] = c.z;
        mesh.boundsRadius = inst::length(e);
    }
    const std::uint32_t row = m_scene.addMesh(mesh);
    if (row != gs::kInvalidIndex) {
        m_meshes.emplace(key, row);
        ++m_stats.meshes;
    }
    return row;
}

std::uint32_t GpuSceneAdapter::materialRow(std::uint64_t key, const tap::Color4& diffuse, std::uint32_t textureHandle) {
    auto it = m_materials.find(key);
    if (it != m_materials.end()) {
        return it->second;
    }
    const auto row = static_cast<std::uint32_t>(m_materials.size());
    gs::GpuMaterial m = renderer::Material{}.pack();
    m.baseColor = renderer::MaterialVec4{diffuse.r, diffuse.g, diffuse.b, diffuse.a};
    if (textureHandle != renderer::kBindlessInvalidShaderHandle) {
        m.baseColorTexIdx = renderer::bindlessShaderHandleIndex(textureHandle);
        ++m_stats.texturedMaterials;
    }
    if (!m_scene.setMaterial(row, m)) {
        return gs::kInvalidIndex;
    }
    m_materials.emplace(key, row);
    ++m_stats.materials;
    return row;
}

void GpuSceneAdapter::placeInstance(std::uint64_t key, std::uint32_t mesh, std::uint32_t material,
                                    const inst::Mat4f& objectToWorld, bool teleport, bool transparent) {
    const gs::GpuTransform t = toGpuTransform(objectToWorld);
    auto it = m_instances.find(key);
    if (it == m_instances.end() || !m_scene.alive(it->second.handle)) {
        gs::InstanceDesc d;
        d.mesh = mesh;
        d.material = material;
        d.transform = t;
        if (transparent) {
            d.flags |= gs::kInstanceTransparent;
        }
        const gs::InstanceHandle h = m_scene.addInstance(d);
        if (!h.valid()) {
            return;
        }
        m_scene.setTransform(h, t, true);
        m_instances[key] = InstanceEntry{h, m_frame};
        ++m_stats.added;
        return;
    }
    InstanceEntry& e = it->second;
    e.seen = m_frame;
    const gs::GpuTransform& cur = m_scene.transform(e.handle.slot);
    if (std::memcmp(&cur, &t, sizeof(t)) != 0) {
        m_scene.setTransform(e.handle, t, teleport);
        ++m_stats.moved;
    }
    const gs::GpuInstance& rec = m_scene.instance(e.handle.slot);
    if (rec.mesh != mesh) {
        m_scene.setInstanceMesh(e.handle, mesh);
    }
    if (rec.material != material) {
        m_scene.setInstanceMaterial(e.handle, material);
    }
}

void GpuSceneAdapter::submit(const AdapterDraw& draw) {
    if (draw.instanceId == 0) {
        return;
    }
    ++m_stats.draws;
    const std::uint32_t texHandle =
        draw.colorTexture != tap::kNoResource && m_textureHandle ? m_textureHandle(draw.colorTexture) : 0u;
    const replace::ReplacedDraw* r = draw.replaced;
    if (!r || r->drawOriginal) {
        std::uint64_t matKey = mixKey(draw.materialHash, kSaltMaterial);
        if (r && r->materialReplaced) {
            matKey = stringKey(r->materialMod + "/" + r->materialRecord, kSaltMaterial);
        }
        const std::uint32_t mesh = meshRow(mixKey(draw.blasId, kSaltMesh), draw.bounds);
        const std::uint32_t material = materialRow(matKey, draw.diffuse, texHandle);
        placeInstance(mixKey(draw.instanceId, 0), mesh, material, draw.objectToWorld, draw.created, draw.transparent);
    } else {
        ++m_stats.hiddenOriginals;
    }
    if (!r) {
        return;
    }
    for (std::size_t p = 0; p < r->parts.size(); ++p) {
        const replace::ReplacedPart& part = r->parts[p];
        const std::uint32_t mesh = meshRow(stringKey(r->meshMod + "/" + part.meshId, kSaltMesh), {});
        const std::uint64_t matKey = part.material.empty()
                                         ? mixKey(draw.materialHash, kSaltMaterial)
                                         : stringKey(part.materialMod + "/" + part.material, kSaltMaterial);
        const std::uint32_t material = materialRow(matKey, draw.diffuse, part.material.empty() ? texHandle : 0u);
        placeInstance(mixKey(draw.instanceId, p + 1), mesh, material, toMat4f(part.objectToWorld), draw.created, false);
        ++m_stats.replacementParts;
    }
}

void GpuSceneAdapter::submitLights(const std::vector<AdapterLight>& lights) {
    for (const AdapterLight& l : lights) {
        auto it = m_lights.find(l.key);
        if (it != m_lights.end() && m_scene.lightAlive(it->second.handle)) {
            m_scene.setLight(it->second.handle, l.light);
            it->second.seen = m_frame;
            continue;
        }
        const gs::LightHandle h = m_scene.addLight(l.light);
        if (h.valid()) {
            m_lights[l.key] = LightEntry{h, m_frame};
        }
    }
}

void GpuSceneAdapter::endFrame() {
    for (auto it = m_instances.begin(); it != m_instances.end();) {
        if (it->second.seen != m_frame) {
            m_scene.removeInstance(it->second.handle);
            it = m_instances.erase(it);
            ++m_stats.removed;
        } else {
            ++it;
        }
    }
    for (auto it = m_lights.begin(); it != m_lights.end();) {
        if (it->second.seen != m_frame) {
            m_scene.removeLight(it->second.handle);
            it = m_lights.erase(it);
        } else {
            ++it;
        }
    }
    m_stats.instances = static_cast<std::uint32_t>(m_instances.size());
    m_stats.lights = static_cast<std::uint32_t>(m_lights.size());
    m_stats.commit = m_scene.commit();
}

void GpuSceneAdapter::clear() {
    for (auto& [key, e] : m_instances) {
        m_scene.removeInstance(e.handle);
    }
    for (auto& [key, e] : m_lights) {
        m_scene.removeLight(e.handle);
    }
    m_instances.clear();
    m_lights.clear();
}

gs::InstanceHandle GpuSceneAdapter::instanceOf(std::uint64_t instanceId, std::uint32_t part) const {
    auto it = m_instances.find(mixKey(instanceId, part));
    return it == m_instances.end() ? gs::InstanceHandle{} : it->second.handle;
}

std::uint32_t GpuSceneAdapter::meshOf(std::uint64_t geometryKey) const {
    auto it = m_meshes.find(mixKey(geometryKey, kSaltMesh));
    return it == m_meshes.end() ? gs::kInvalidIndex : it->second;
}

std::uint32_t GpuSceneAdapter::materialOf(std::uint64_t materialKey) const {
    auto it = m_materials.find(mixKey(materialKey, kSaltMaterial));
    return it == m_materials.end() ? gs::kInvalidIndex : it->second;
}

} // namespace fuse::relight::render::frame

namespace fuse::relight::render::frame {

std::uint32_t GpuSceneAdapter::lightSlot(std::uint64_t lightKey) const {
    auto it = m_lights.find(lightKey);
    if (it == m_lights.end() || !m_scene.lightAlive(it->second.handle)) {
        return gs::kInvalidIndex;
    }
    return it->second.handle.slot;
}

} // namespace fuse::relight::render::frame
