/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c and
// src/dxvk/rtx_render/rtx_accel_manager.cpp@0867d3c. See scene_model.hpp.
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/scene_model.hpp>

#include <fuse/relight/options/option_manager.hpp>

#include <algorithm>

namespace fuse::relight::scene::instances {

namespace {

bool isIdentity(const std::array<double, 16>& m) {
    for (std::size_t i = 0; i < 16; ++i) {
        if (m[i] != ((i % 5) == 0 ? 1.0 : 0.0)) {
            return false;
        }
    }
    return true;
}

} // namespace

SceneModel::SceneModel() : m_uniqueObjectSearchDistance(InstanceOptions::uniqueObjectDistance()) {
    registerInstanceOptions();
    InstanceEventHandler handler;
    handler.owner = this;
    handler.onInstanceUpdated = [this](RtInstance& instance, const SceneDrawInput&, bool materialBound, bool, bool) {
        if (!materialBound) {
            return; // the preserve path keeps the bound surface material
        }
        // createSurfaceMaterial / bind: retain the new material's subsurface extension, release the old one.
        const SubsurfaceKind kind = instance.getSubsurfaceKind();
        auto it = m_retainedSubsurface.find(instance.getId());
        const SubsurfaceKind previous = it != m_retainedSubsurface.end() ? it->second : SubsurfaceKind::None;
        if (previous != kind) {
            releaseSubsurface(previous);
            retainSubsurface(kind);
            m_retainedSubsurface[instance.getId()] = kind;
        }
    };
    handler.onInstanceDestroyed = [this](RtInstance& instance) {
        // SceneManager::onInstanceDestroyed -> releaseSurfaceMaterial.
        auto it = m_retainedSubsurface.find(instance.getId());
        if (it != m_retainedSubsurface.end()) {
            releaseSubsurface(it->second);
            m_retainedSubsurface.erase(it);
        }
        // PrimInstanceOwner::setReplacementInstance(nullptr): drop the RI's slot.
        if (ReplacementInstance* ri = instance.getReplacementInstance()) {
            std::replace(ri->prims.begin(), ri->prims.end(), &instance, static_cast<RtInstance*>(nullptr));
        }
    };
    m_instances.addEventHandler(handler);
}

SceneModel::~SceneModel() {
    // Instances are destroyed with the instance manager; the tracker's back-pointers go first.
    m_tracker.clear();
    m_instances.removeEventHandler(this);
}

void SceneModel::retainSubsurface(SubsurfaceKind kind) {
    if (kind == SubsurfaceKind::DiffusionProfile) {
        ++m_sssCount;
    } else if (kind == SubsurfaceKind::ThinOpaque) {
        ++m_thinOpaqueCount;
    }
}

void SceneModel::releaseSubsurface(SubsurfaceKind kind) {
    std::uint32_t* count = kind == SubsurfaceKind::DiffusionProfile ? &m_sssCount
                           : kind == SubsurfaceKind::ThinOpaque     ? &m_thinOpaqueCount
                                                                    : nullptr;
    if (count == nullptr) {
        return;
    }
    if (*count == 0) {
        // "releaseSurfaceMaterial: ... count underflow (mismatched retain/release)"
        ++m_subsurfaceUnderflows;
    } else {
        --*count;
    }
}

SceneDrawResult SceneModel::submitDraw(const SceneDrawInput& input) {
    SceneDrawResult result;
    ++m_stats.draws;

    TrackerMatch how = TrackerMatch::New;
    ReplacementInstance* ri = m_tracker.findOrCreateReplacementInstance(input, m_frame, &how);
    result.match = how;
    result.replacementInstanceId = ri->id;

    const bool secondSubmissionThisFrame = ri->frameLastSeen == m_frame;

    // The static (preserve) path reuses the BLAS entry as is: only valid if no other draw rebuilt it this frame.
    const auto blasAlreadyTouchedByOtherDraw = [&]() {
        for (RtInstance* inst : ri->prims) {
            if (inst != nullptr && inst->getBlas() != nullptr && inst->getBlas()->frameLastUpdated == m_frame) {
                return true;
            }
        }
        return false;
    };
    const bool legacyMaterialIdentityHashMatch = ri->legacyMaterialIdentityHash == input.materialIdentityHash;
    const bool primsAlive = !ri->prims.empty() && ri->prims[0] != nullptr;
    const bool usePreservePath = InstanceOptions::enablePreservePath() && primsAlive && ri->dirtyFlags == 0 &&
                                 !options::OptionManager::isDrawcallTranslationInvalid() && !secondSubmissionThisFrame && !input.categories.test(InstanceCategories::ParticleEmitter) &&
                                 !blasAlreadyTouchedByOtherDraw() && legacyMaterialIdentityHashMatch;

    RtInstance* instance = nullptr;
    if (usePreservePath) {
        // preserveReplacementInstance: refresh the BLAS input, then preserve the instance.
        instance = ri->prims[0];
        m_instances.preserveInstance(*instance, input, m_frame);
        result.preserved = true;
        ++m_stats.preserved;
    } else {
        // Any option read inside the dynamic update should force a full update when changed.
        FUSE_RELIGHT_OPTION_INVALIDATION_SCOPE(options::OptionFlags::InvalidatesDrawcallTranslation);
        // Recompute dynamic-feature bits on each dynamic update.
        ri->dirtyFlags &= ~ReplacementInstance::kDynamicFeatureMask;
        RtInstance* existingInstance = !ri->prims.empty() ? ri->prims[0] : nullptr;

        // processDrawCallState
        BlasEntry* blas = nullptr;
        const DrawCallCache::CacheState state = m_cache.get(input, m_frame, &blas);
        result.cacheState = m_cache.process(*blas, state, input, m_frame);
        InstanceUpdate update;
        instance = m_instances.processSceneObject(*blas, input, existingInstance, m_frame, &update);
        result.created = update.created;
        result.hasTransformChanged = update.hasTransformChanged;
        if (update.created) {
            ++m_stats.createdInstances;
        }
        if (input.categories.test(InstanceCategories::ParticleEmitter)) {
            // A particle system spawns from this instance: the next frame must take the dynamic path.
            ri->dirtyFlags |= ReplacementInstance::ParticleSystem;
        }
        if (instance != nullptr) {
            if (ri->prims.empty()) {
                ri->prims.push_back(instance);
            } else if (ri->prims[0] != instance) {
                ri->prims[0] = instance;
            }
            instance->setReplacementInstance(ri);
        }
        ri->legacyMaterialIdentityHash = input.materialIdentityHash;
    }

    ri->frameLastSeen = m_frame;
    ri->categoryFlags = input.categories;
    ri->isSkinned = input.numBones > 0;
    // Cache this submission's texture-coordinate projection so next frame's computeDirtyFlags detects drift.
    ri->textureTransform = input.textureTransform;
    ri->texgenMode = input.texgenMode;
    // Standalone draw calls store the object-space bounding box for anti-culling.
    if (input.boundingBox.isValid()) {
        ri->geometryBoundingBox = input.boundingBox;
        ri->objectToWorld = input.objectToWorld;
    }

    if (instance != nullptr) {
        result.instanceId = instance->getId();
        result.blasId = instance->getBlas() ? instance->getBlas()->id : 0;
        result.isStatic = instance->isStatic();
        result.objectToWorld = instance->getTransform();
        result.prevObjectToWorld = instance->getPrevTransform();
    }
    return result;
}

SceneFrameStats SceneModel::endFrame(const CameraState* mainCamera) {
    // AccelManager::mergeInstancesIntoBlas / addPointInstancerBlas: the instances drawn this frame, one surface
    // slot each, N for a point instancer (+ an SSS batch for subsurface instancers).
    m_pointBatches.clear();
    m_pointInstances.clear();
    std::uint32_t surfaceIndex = 0;
    for (RtInstance* inst : m_instances.getInstanceTable()) {
        if (inst->getFrameLastUpdated() != m_frame || inst->isHidden()) {
            continue;
        }
        const auto& transforms = inst->getInstancesToObject();
        if (!transforms || transforms->empty()) {
            ++surfaceIndex;
            continue;
        }
        PointInstancerBatch batch;
        batch.instanceId = inst->getId();
        batch.transforms = transforms;
        batch.objectToWorld = inst->getTransform();
        batch.prevObjectToWorld = inst->getPrevTransform();
        batch.baseSurfaceIndex = surfaceIndex;
        m_pointBatches.push_back(batch);
        if (inst->isSubsurface()) {
            PointInstancerBatch sss = batch;
            sss.subsurfaceTlas = true;
            m_pointBatches.push_back(sss);
        }
        surfaceIndex += static_cast<std::uint32_t>(transforms->size());
    }
    Vec3 cameraPosition;
    if (mainCamera != nullptr) {
        const std::array<float, 3> p = mainCamera->position();
        cameraPosition = {p[0], p[1], p[2]};
    }
    const PointInstancerCulling culling = PointInstancerCulling::fromOptions(cameraPosition);
    for (const PointInstancerBatch& batch : m_pointBatches) {
        if (batch.subsurfaceTlas) {
            continue; // same entries in the SSS TLAS
        }
        const std::vector<PointInstance> expanded = expandPointInstancer(batch, culling);
        m_pointInstances.insert(m_pointInstances.end(), expanded.begin(), expanded.end());
    }
    m_stats.pointInstances = static_cast<std::uint32_t>(m_pointInstances.size());
    m_stats.visiblePointInstances = static_cast<std::uint32_t>(
        std::count_if(m_pointInstances.begin(), m_pointInstances.end(), [](const PointInstance& p) { return p.visible(); }));

    // SceneManager::garbageCollection (prepareSceneData): BLAS entries, ReplacementInstances (anti-culling uses
    // the support flag of the previous frame, as upstream), then the instances they marked.
    m_cache.garbageCollection(m_frame);
    std::optional<AntiCullingFrustum> frustum;
    const bool cameraCut = mainCamera != nullptr && mainCamera->isCameraCut();
    if (m_isAntiCullingSupported && mainCamera != nullptr) {
        frustum = AntiCullingFrustum::fromCamera(*mainCamera);
    }
    m_stats.gc = m_tracker.garbageCollectReplacementInstances(m_frame, frustum ? &*frustum : nullptr, cameraCut);
    m_instances.garbageCollection();
    m_stats.antiCullingSupported = m_isAntiCullingSupported;
    // "When the game doesn't set up the View Matrix, we must disable Anti-Culling."
    m_isAntiCullingSupported = mainCamera != nullptr && !isIdentity(mainCamera->viewToWorld);

    // onFrameEnd: rebuild the spatial maps when rtx.uniqueObjectDistance changed.
    if (m_uniqueObjectSearchDistance != InstanceOptions::uniqueObjectDistance()) {
        m_uniqueObjectSearchDistance = InstanceOptions::uniqueObjectDistance();
        m_tracker.rebuildSpatialMaps(m_uniqueObjectSearchDistance * 2.f);
    }
    m_instances.onFrameEnd();
    // Any drawcall translation invalidation has been consumed by this point.
    options::OptionManager::clearDrawcallTranslationInvalid();

    SceneFrameStats stats = m_stats;
    stats.frame = m_frame;
    stats.activeInstances = m_instances.getActiveCount();
    stats.replacementInstances = static_cast<std::uint32_t>(m_tracker.getReplacementInstances().size());
    stats.blasEntries = static_cast<std::uint32_t>(m_cache.size());
    stats.thinOpaqueInstances = m_thinOpaqueCount;
    stats.sssInstances = m_sssCount;
    m_stats = SceneFrameStats();
    ++m_frame;
    return stats;
}

void SceneModel::clear() {
    m_tracker.clear();
    m_instances.garbageCollection();
    m_instances.clear();
    m_cache.clear();
    m_pointBatches.clear();
    m_pointInstances.clear();
}

} // namespace fuse::relight::scene::instances
