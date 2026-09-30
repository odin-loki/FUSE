// E02 SceneRenderer: see include/fuse/renderer/scene_renderer/scene_renderer.hpp.
#include <fuse/renderer/scene_renderer/scene_renderer.hpp>

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_reference.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace fuse::renderer::scene_renderer {

namespace {
constexpr f32 kDegToRad = 0.017453292519943295f;

/// World AABB of a local box under a column-major object->world matrix (centre + |M| * half extent).
void worldBox(const f32 m[16], const f32 lo[3], const f32 hi[3], f32 outMin[3], f32 outMax[3]) {
    f32 c[3];
    f32 e[3];
    for (u32 a = 0; a < 3u; ++a) {
        c[a] = (lo[a] + hi[a]) * 0.5f;
        e[a] = (hi[a] - lo[a]) * 0.5f;
    }
    for (u32 r = 0; r < 3u; ++r) {
        const f32 wc = m[r] * c[0] + m[4 + r] * c[1] + m[8 + r] * c[2] + m[12 + r];
        const f32 we = std::fabs(m[r]) * e[0] + std::fabs(m[4 + r]) * e[1] + std::fabs(m[8 + r]) * e[2];
        outMin[r] = wc - we;
        outMax[r] = wc + we;
    }
}
} // namespace

bool cameraFromRegistry(ecs::Registry& registry, frame::FrameCamera& out) {
    bool found = false;
    bool active = false;
    registry.each<ecs::Transform, ecs::Camera>([&](ecs::EntityID, ecs::Transform& t, ecs::Camera& c) {
        if (active || (found && !c.is_active)) {
            return;
        }
        const auto& m = t.local_to_world.data;
        f32 fwd[3] = {-m[8], -m[9], -m[10]};
        const f32 len = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (!(len > 0.f)) {
            fwd[0] = 0.f;
            fwd[1] = 0.f;
            fwd[2] = -1.f;
        } else {
            for (f32& v : fwd) {
                v /= len;
            }
        }
        for (u32 a = 0; a < 3u; ++a) {
            out.eye[a] = m[12 + a];
            out.target[a] = m[12 + a] + fwd[a];
        }
        out.fovY = c.fov_deg * kDegToRad;
        out.nearPlane = c.near_plane;
        out.farPlane = c.far_plane;
        found = true;
        active = c.is_active;
    });
    return found;
}

SceneRenderer::~SceneRenderer() { destroy(); }

bool SceneRenderer::initialize(const SceneRendererDesc& desc) {
    destroy();
    m_desc = desc;
    if (desc.device == nullptr || desc.allocator == nullptr || desc.bindless == nullptr || desc.width == 0u || desc.height == 0u) {
        m_reason = "invalid description";
        return false;
    }
    m_initialized = true; // destroy() releases partial state from here on
    auto fail = [&](const char* why) {
        destroy();
        m_reason = why;
        return false;
    };
    // Upload queue: the caller's, or an own one over a CpuToGpu staging ring.
    if (desc.upload != nullptr) {
        m_upload = desc.upload;
    } else {
        BufferDesc staging{};
        staging.size = desc.stagingBytes;
        staging.usage = BufferUsage::TransferSrc;
        staging.memoryUsage = MemoryUsage::CpuToGpu;
        staging.name = "scene_renderer.staging";
        if (!desc.allocator->createBuffer(staging, m_staging) || m_staging.mapped == nullptr ||
            !m_ownUpload.init(desc.device, m_staging.handle, m_staging.mapped, desc.stagingBytes)) {
            return fail("staging / UploadQueue");
        }
        m_upload = &m_ownUpload;
    }
    gpu_scene::GpuSceneDesc sd{};
    sd.device = desc.device;
    sd.allocator = desc.allocator;
    sd.upload = m_upload;
    sd.bindless = desc.bindless;
    sd.instanceCapacity = desc.instanceCapacity;
    sd.meshCapacity = desc.meshCapacity;
    sd.materialCapacity = desc.materialCapacity;
    sd.lightCapacity = desc.lightCapacity;
    sd.name = "scene_renderer.scene";
    if (!m_scene.init(sd) || !m_scene.gpuEnabled()) {
        return fail("GpuScene");
    }
    gpu_scene::EcsExtractDesc ed{};
    ed.entityCapacity = desc.entityCapacity;
    m_extractor.init(ed);
    m_feed.reset();
    // Tier: Auto -> T2 when the device passes the ray-query gate.
    const bool rtUsable = rt::queryRtCapabilities(desc.device).usable;
    if (desc.tier == SceneTier::T2 && !rtUsable) {
        return fail("T2 requested below the T2 gate (rt::queryRtCapabilities)");
    }
    m_tier = desc.tier == SceneTier::T2 || (desc.tier == SceneTier::Auto && rtUsable) ? frame::FrameTier::T2 : frame::FrameTier::T0;
    if (!createComposer()) {
        return fail(m_reason);
    }
    m_sdfWorld.boxes.reserve(desc.instanceCapacity);
    m_sdfDirty = true;
    m_remapVersion = ~0ull;
    m_reason = "ok";
    return true;
}

bool SceneRenderer::createComposer() {
    frame::FrameComposerDesc d = m_desc.composer;
    d.device = m_desc.device;
    d.allocator = m_desc.allocator;
    d.bindless = m_desc.bindless;
    d.upload = m_upload;
    d.scene = &m_scene;
    d.tier = m_tier;
    d.renderWidth = m_desc.renderWidth != 0u ? m_desc.renderWidth : m_desc.width;
    d.renderHeight = m_desc.renderHeight != 0u ? m_desc.renderHeight : m_desc.height;
    d.displayWidth = m_desc.width;
    d.displayHeight = m_desc.height;
    d.instanceCapacity = m_desc.instanceCapacity;
    d.meshCapacity = m_desc.meshCapacity;
    d.lightCapacity = m_desc.lightCapacity;
    if (!m_composer.init(d)) {
        m_reason = m_composer.reason();
        return false;
    }
    m_sdfDirty = true;
    return true;
}

void SceneRenderer::destroy() {
    if (!m_initialized) {
        return;
    }
    m_composer.destroy();
    m_extractor.releaseAll(m_scene);
    m_scene.destroy();
    if (m_upload == &m_ownUpload) {
        m_ownUpload.destroy();
    }
    if (m_staging.handle != nullptr) {
        m_desc.allocator->destroyBuffer(m_staging);
    }
    m_staging = Buffer{};
    m_upload = nullptr;
    m_meshes.clear();
    m_feed.reset();
    m_materials = nullptr;
    m_sdfWorld.boxes.clear();
    m_sdfObjects.clear();
    m_sdfSurfaces.clear();
    m_stats = SceneFrameStats{};
    m_serial = 0;
    m_frames = 0;
    m_initialized = false;
    m_reason = "not initialised";
}

bool SceneRenderer::resize(u32 width, u32 height, u32 renderWidth, u32 renderHeight) {
    if (!m_initialized || width == 0u || height == 0u) {
        return false;
    }
    const frame::FrameUiSource ui = m_composer.uiSource();
    m_composer.destroy();
    m_desc.width = width;
    m_desc.height = height;
    m_desc.renderWidth = renderWidth;
    m_desc.renderHeight = renderHeight;
    if (!createComposer()) {
        return false;
    }
    if (ui.record != nullptr) {
        m_composer.setUiSource(ui);
    }
    m_frames = 0; // history restarts (renderScene resets on frame 0)
    return true;
}

void SceneRenderer::setMaterialSystem(const MaterialSystem* materials) {
    m_materials = materials;
    m_feed.reset();
}

bool SceneRenderer::setMaterial(u32 id, const Material& material) {
    m_sdfDirty = true;
    return m_scene.setMaterial(id, material);
}

void SceneRenderer::rebuildSdf(ecs::Registry& registry) {
    m_sdfWorld.boxes.clear();
    const gpu_scene::TableBytes mats = m_scene.tableBytes(gpu_scene::GpuSceneTable::Materials);
    auto addChunk = [&](std::span<const ecs::EntityID> ids, std::span<ecs::Transform> transforms, std::span<ecs::Mesh> meshes) {
        for (usize i = 0; i < ids.size(); ++i) {
            const ecs::Mesh& mesh = meshes[i];
            if (!mesh.visible || !mesh.vertex_buffer.isValid()) {
                continue;
            }
            f32 lo[3] = {mesh.aabb_min.x, mesh.aabb_min.y, mesh.aabb_min.z};
            f32 hi[3] = {mesh.aabb_max.x, mesh.aabb_max.y, mesh.aabb_max.z};
            const bool ecsBox = hi[0] > lo[0] || hi[1] > lo[1] || hi[2] > lo[2];
            if (!ecsBox && !m_meshes.localBounds(mesh.vertex_buffer.index(), lo, hi)) {
                continue;
            }
            DdgiCpuBox box{};
            f32 wmin[3];
            f32 wmax[3];
            worldBox(transforms[i].local_to_world.data.data(), lo, hi, wmin, wmax);
            box.min = math::Vec3{wmin[0], wmin[1], wmin[2]};
            box.max = math::Vec3{wmax[0], wmax[1], wmax[2]};
            if (mesh.material_id < mats.count && mats.data != nullptr) {
                gpu_scene::GpuMaterial row{};
                std::memcpy(&row, mats.data + static_cast<usize>(mesh.material_id) * mats.stride, sizeof(row));
                // ddgiSurfaceFromMaterial on the packed row: albedo = base * (1 - metallic), emissive = colour x intensity.
                const f32 dielectric = 1.f - std::clamp(row.baseColor.w, 0.f, 1.f);
                box.surface.albedo = math::Vec3{row.baseColor.x, row.baseColor.y, row.baseColor.z} * dielectric;
                box.surface.emissive = math::Vec3{row.roughnessEmissive.y, row.roughnessEmissive.z, row.roughnessEmissive.w} *
                                       std::max(row.emissiveIntensity, 0.f);
            }
            m_sdfWorld.boxes.push_back(box);
        }
    };
    registry.each_chunk<ecs::Transform, ecs::Mesh>(addChunk);
    gi_gpu::sdfSceneFromBoxes(m_sdfWorld, m_sdfObjects, m_sdfSurfaces);
    m_composer.setSdfScene(m_sdfObjects.data(), static_cast<u32>(m_sdfObjects.size()), m_sdfSurfaces.data(),
                           static_cast<u32>(m_sdfSurfaces.size()));
}

frame::FrameGraphOutputs SceneRenderer::renderScene(ecs::Registry& registry, const frame::FrameCamera& camera, rg::Graph& graph,
                                                    const SceneFrameDesc& frameDesc) {
    m_stats = SceneFrameStats{};
    if (!m_initialized || !m_composer.valid()) {
        return frame::FrameGraphOutputs{};
    }
    m_serial = frameDesc.serial != 0u ? frameDesc.serial : m_serial + 1u;
    const u64 serial = m_serial;
    if (m_desc.manageBindlessSerial) {
        m_desc.bindless->setFrameSerial(serial);
    }
    m_scene.beginFrame(serial);
    m_composer.beginSceneFrame(serial);

    // Meshes, then materials, then the ECS mirror.
    m_stats.meshesUploaded = m_meshes.flush(m_scene);
    if (m_meshes.remapVersion() != m_remapVersion) {
        m_extractor.setMeshRemap(m_meshes.remapCount() != 0u ? m_meshes.remap() : nullptr, m_meshes.remapCount());
        m_remapVersion = m_meshes.remapVersion();
    }
    if (m_materials != nullptr) {
        m_stats.materials = m_feed.sync(*m_materials, m_scene);
        m_sdfDirty = m_sdfDirty || m_stats.materials.rowsWritten != 0u;
    }
    m_stats.extract = m_extractor.extract(registry, m_scene);
    const gpu_scene::EcsExtractStats& ex = m_stats.extract;
    m_sdfDirty = m_sdfDirty || ex.added != 0u || ex.removed != 0u || m_stats.meshesUploaded != 0u ||
                 (m_desc.dynamicSdf && ex.transformWrites != 0u);
    if (m_desc.sdfFromScene && m_sdfDirty && m_tier == frame::FrameTier::T0 &&
        (m_composer.available() & frame::kStageDdgi) != 0u) {
        rebuildSdf(registry);
        m_stats.sdfRebuilt = true;
    }
    m_sdfDirty = false;

    // Sun: the first DirectionalLight entity's light slot.
    frame::FrameDesc fd{};
    fd.serial = serial;
    fd.frameIndex = frameDesc.frameIndex != 0xFFFFFFFFu ? frameDesc.frameIndex : m_frames;
    fd.camera = camera;
    fd.resetHistory = frameDesc.resetHistory || m_frames == 0u;
    fd.deltaSeconds = frameDesc.deltaSeconds;
    bool sunFound = false;
    registry.each<ecs::Transform, ecs::DirectionalLight>([&](ecs::EntityID id, ecs::Transform&, ecs::DirectionalLight&) {
        if (sunFound) {
            return;
        }
        const gpu_scene::LightHandle h = m_extractor.lightOf(id);
        if (!h.valid() || !m_scene.lightAlive(h)) {
            return;
        }
        const gpu_scene::GpuLight& l = m_scene.light(h.slot);
        fd.sun.slot = h.slot;
        for (u32 a = 0; a < 3u; ++a) {
            fd.sun.toSun[a] = -l.direction[a];
            fd.sun.illuminance[a] = frameDesc.sunIlluminance[0] >= 0.f ? frameDesc.sunIlluminance[a] : l.color[a] * l.intensity;
        }
        sunFound = true;
    });
    m_stats.sunSlot = fd.sun.slot;

    m_stats.commit = m_scene.commit();
    const bool committed = m_composer.commitScene();
    m_upload->flush();
    const bool begun = m_stats.commit.ok && committed && m_composer.beginFrame(fd, m_settings);
    if (!begun) {
        return frame::FrameGraphOutputs{};
    }
    const frame::FrameGraphOutputs out = m_composer.addFrame(graph);
    ++m_frames;
    m_stats.ok = out.output.valid();
    return out;
}

bool SceneRenderer::addPresent(rg::Graph& graph, const frame::FrameGraphOutputs& outputs, const ScenePresentTargets& targets) {
    if (!outputs.output.valid() || !targets.target.valid()) {
        return false;
    }
    const u32 w = m_composer.outputWidth();
    const u32 h = m_composer.outputHeight();
    const bool fg = outputs.presentInterpolated.valid() && outputs.presentReal.valid() && targets.interpolatedTarget.valid();
    PresentBlit& real = m_blits[0];
    real = PresentBlit{};
    real.source = fg ? outputs.presentReal : outputs.output;
    real.sourceWidth = w;
    real.sourceHeight = h;
    real.target = targets.target;
    real.targetWidth = targets.width != 0u ? targets.width : w;
    real.targetHeight = targets.height != 0u ? targets.height : h;
    real.present = targets.swapchain;
    bool ok = true;
    if (fg) {
        // The interpolated frame is presented first (FSR 3 frame pacing: present_timing.hpp).
        PresentBlit& mid = m_blits[1];
        mid = real;
        mid.source = outputs.presentInterpolated;
        mid.target = targets.interpolatedTarget;
        ok = addPresentBlit(graph, mid);
    }
    return addPresentBlit(graph, real) && ok;
}

void SceneRenderer::collectRetired(u64 completedSerial) {
    if (!m_initialized) {
        return;
    }
    m_composer.collectRetired(completedSerial);
    m_scene.collectRetired(completedSerial);
    if (m_desc.manageBindlessSerial) {
        m_desc.bindless->collectRetired(completedSerial);
    }
}

} // namespace fuse::renderer::scene_renderer
