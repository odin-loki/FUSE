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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_instance_manager.cpp@0867d3c and
// src/dxvk/rtx_render/rtx_scene_manager.cpp@0867d3c (SceneManager::preserveInstance). See
// instance_manager.hpp.
#include <fuse/relight/scene/instances/instance_manager.hpp>
#include <fuse/relight/scene/instances/instance_options.hpp>

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <algorithm>
#include <utility>
#include <variant>

namespace fuse::relight::scene::instances {

namespace {

/// rtx.worldSpaceUiBackgroundOffset (borrowed; Remix default 0).
float worldSpaceUiBackgroundOffset() {
    if (const options::OptionBase* o = options::OptionManager::findOption("rtx.worldSpaceUiBackgroundOffset")) {
        const options::OptionValue v = o->getResolvedValue();
        if (const float* f = std::get_if<float>(&v)) {
            return *f;
        }
    }
    return 0.f;
}

std::uint32_t cameraBit(CameraType t) {
    return 1u << static_cast<std::uint32_t>(t);
}

} // namespace

std::array<float, 9> normalMatrix(const Mat4f& m) {
    // transpose(inverse(Matrix3(m))): the cofactor matrix divided by the determinant.
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float D = -(b * i - c * h), E = a * i - c * g, F = -(a * h - b * g);
    const float G = b * f - c * e, H = -(a * f - c * d), I = a * e - b * d;
    const float det = a * A + b * B + c * C;
    const float inv = det != 0.f ? 1.f / det : 0.f;
    return {A * inv, B * inv, C * inv, D * inv, E * inv, F * inv, G * inv, H * inv, I * inv};
}

hash::Hash64 AxisAlignedBoundingBox::calculateHash() const {
    const float data[6] = {minPos.x, minPos.y, minPos.z, maxPos.x, maxPos.y, maxPos.z};
    return hash::xxh3_64(data, sizeof data);
}

// ---- RtInstance ------------------------------------------------------------------------------------------------

void RtInstance::onTransformChanged() {
    m_normalObjectToWorld = normalMatrix(m_objectToWorld);
}

bool RtInstance::teleport(const Mat4f& objectToWorld) {
    m_objectToWorld = objectToWorld;
    m_prevObjectToWorld = objectToWorld;
    onTransformChanged();
    m_blasDirty = true;
    return false; // freshly teleported instances are always treated as still.
}

bool RtInstance::teleport(const Mat4f& objectToWorld, const Mat4f& prevObjectToWorld) {
    m_objectToWorld = objectToWorld;
    m_prevObjectToWorld = prevObjectToWorld;
    onTransformChanged();
    m_blasDirty = true;
    return !bitwiseEqual(m_prevObjectToWorld, m_objectToWorld);
}

void RtInstance::teleportWithHistory(const Mat4f& oldToNew) {
    // Upstream: objectToWorld = oldToNew * objectToWorld (column vectors) = objectToWorld x oldToNew here.
    m_objectToWorld = multiply(m_objectToWorld, oldToNew);
    m_prevObjectToWorld = multiply(m_prevObjectToWorld, oldToNew);
    onTransformChanged();
    m_blasDirty = true;
}

bool RtInstance::move(const Mat4f& objectToWorld) {
    m_prevObjectToWorld = m_objectToWorld;
    m_objectToWorld = objectToWorld;
    onTransformChanged();
    // See if the transform has changed even a tiny bit: the result feeds the 'isStatic' surface flag (motion
    // vectors are skipped for static surfaces).
    return !bitwiseEqual(m_prevObjectToWorld, m_objectToWorld);
}

bool RtInstance::moveAgain(const Mat4f& objectToWorld) {
    m_objectToWorld = objectToWorld;
    onTransformChanged();
    return !bitwiseEqual(m_prevObjectToWorld, m_objectToWorld);
}

bool RtInstance::setFrameLastUpdated(std::uint32_t frameIndex) {
    if (m_frameLastUpdated != frameIndex) {
        m_seenCameraTypes = 0;
        m_frameLastUpdated = frameIndex;
        return true;
    }
    return false;
}

bool RtInstance::registerCamera(CameraType cameraType) {
    const bool settingNewCameraType = (m_seenCameraTypes & cameraBit(cameraType)) == 0;
    m_seenCameraTypes |= cameraBit(cameraType);
    return settingNewCameraType;
}

bool RtInstance::isCameraRegistered(CameraType cameraType) const {
    return (m_seenCameraTypes & cameraBit(cameraType)) != 0;
}

hash::Hash64 RtInstance::calculateAntiCullingHash() const {
    if (!AntiCullingOptions::isObjectAntiCullingEnabled()) {
        return hash::kEmptyHash;
    }
    const Vec3 pos = getWorldPosition();
    const float posData[3] = {pos.x, pos.y, pos.z};
    const hash::Hash64 posHash = hash::xxh3_64(posData, sizeof posData);
    hash::Hash64 antiCullingHash = hash::xxh3_64(&m_materialDataHash, sizeof(hash::Hash64), posHash);
    // RtxOptions::needsMeshBoundingBox() is true whenever object anti-culling is enabled.
    if (AntiCullingOptions::Object::hashInstanceWithBoundingBoxHash() && m_linkedBlas != nullptr) {
        const hash::Hash64 bboxHash = m_linkedBlas->input.boundingBox.calculateHash();
        antiCullingHash = hash::xxh3_64(&bboxHash, sizeof(antiCullingHash), antiCullingHash);
    }
    return antiCullingHash;
}

// ---- InstanceManager -------------------------------------------------------------------------------------------

InstanceManager::~InstanceManager() {
    for (RtInstance* instance : m_instances) {
        delete instance;
    }
}

void InstanceManager::removeEventHandler(void* owner) {
    for (auto it = m_eventHandlers.begin(); it != m_eventHandlers.end(); ++it) {
        if (it->owner == owner) {
            m_eventHandlers.erase(it);
            break;
        }
    }
}

void InstanceManager::clear() {
    notifySceneChanged();
    for (RtInstance* instance : m_instances) {
        removeInstance(instance);
        delete instance;
    }
    m_instances.clear();
}

void InstanceManager::garbageCollection() {
    // Instance lifetimes are managed externally: tracked instances are marked for GC when their
    // ReplacementInstance is cleared.
    for (std::uint32_t i = 0; i < m_instances.size();) {
        RtInstance*& pInstance = m_instances[i];
        if (pInstance->m_isMarkedForGC) {
            notifySceneChanged();
            removeInstance(pInstance);
            // NOTE: pInstance is now the (previously) last element
            std::swap(pInstance, m_instances.back());
            m_instances[i]->m_vectorIdx = i;
            delete m_instances.back();
            m_instances.pop_back();
            continue;
        }
        ++i;
    }
}

RtInstance* InstanceManager::processSceneObject(BlasEntry& blas, const SceneDrawInput& drawCall, RtInstance* existingInstance,
                                                std::uint32_t currentFrame, InstanceUpdate* update) {
    InstanceUpdate local;
    InstanceUpdate& result = update ? *update : local;
    result = InstanceUpdate();

    // If no existing instance is provided, this is a genuinely new draw call and we need a fresh instance.
    RtInstance* currentInstance = existingInstance;
    if (currentInstance == nullptr) {
        currentInstance = addInstance(blas, currentFrame);
        result.created = true;
    } else if (currentInstance->getBlas() != &blas) {
        // The BlasEntry changed - re-link the instance to the current one.
        if (BlasEntry* oldBlas = currentInstance->getBlas()) {
            oldBlas->unlinkInstance(currentInstance);
        }
        currentInstance->m_linkedBlas = &blas;
        blas.linkInstance(currentInstance);
        currentInstance->m_blasDirty = true;
        notifySceneChanged();
    }
    updateInstance(*currentInstance, blas, drawCall, currentFrame, result);
    return currentInstance;
}

RtInstance* InstanceManager::addInstance(BlasEntry& blas, std::uint32_t currentFrame) {
    notifySceneChanged();
    const std::uint32_t instanceIdx = static_cast<std::uint32_t>(m_instances.size());
    RtInstance* instance = new RtInstance(m_nextInstanceId++, instanceIdx);
    m_instances.push_back(instance);
    instance->m_frameCreated = currentFrame;
    instance->m_linkedBlas = &blas;
    // onInstanceAdded: the scene manager links the instance to its BLAS.
    blas.linkInstance(instance);
    for (auto& handler : m_eventHandlers) {
        if (handler.onInstanceAdded) {
            handler.onInstanceAdded(*instance);
        }
    }
    return instance;
}

void InstanceManager::updateInstance(RtInstance& currentInstance, const BlasEntry& blas, const SceneDrawInput& drawCall,
                                     std::uint32_t currentFrame, InstanceUpdate& update) {
    const CategoryFlags previousCategoryFlags = currentInstance.m_categoryFlags;
    const bool previousIsSubsurface = currentInstance.m_isSubsurface;
    const auto previousInstancesToObject = currentInstance.m_instancesToObject;
    const std::size_t previousInstancesToObjectSize = previousInstancesToObject ? previousInstancesToObject->size() : 0;

    currentInstance.m_categoryFlags = drawCall.categories;
    currentInstance.m_instancesToObject = drawCall.instancesToObject;

    // setFrameLastUpdated() must be called first as it resets instance's state on a first call in a frame
    const bool isFirstUpdateThisFrame = currentInstance.setFrameLastUpdated(currentFrame);
    update.firstUpdateThisFrame = isFirstUpdateThisFrame;

    // Full instance processing always goes through the dynamic draw path this frame.
    currentInstance.m_isPreservePath = false;

    currentInstance.m_isHidden = currentInstance.m_categoryFlags.test(InstanceCategories::Hidden);
    // Hide the sky instance since it is not raytraced (sky mesh and material are only good for capture and
    // replacement purposes).
    if (drawCall.cameraType == CameraType::Sky) {
        currentInstance.m_isHidden = true;
    }

    const bool isNewCameraSet = !currentInstance.isCameraRegistered(drawCall.cameraType);
    const bool overridePreviousCameraUpdate =
        isNewCameraSet &&
        (drawCall.cameraType == CameraType::Main ||
         // Don't overwrite transform from when the instance was seen with the main camera
         !currentInstance.isCameraRegistered(CameraType::Main));

    bool hasTransformChanged = false;
    bool hasPreviousPositions = false;

    // (!isFirstUpdateThisFrame: mergeInstanceHeuristics merges the alpha state of a second draw of the same
    // instance - render-side, not ported.)

    // Updates done only once a frame unless overriden due to an explicit state
    if (isFirstUpdateThisFrame || overridePreviousCameraUpdate) {
        if (isFirstUpdateThisFrame) {
            currentInstance.m_materialType = drawCall.materialType;
            // MaterialData::getHash of the render material: an override material, else the legacy one.
            const Hash64 materialInstanceHash =
                drawCall.overrideMaterialHash != hash::kEmptyHash ? drawCall.overrideMaterialHash : drawCall.materialHash;
            currentInstance.m_materialDataHash = drawCall.materialHash;
            currentInstance.m_hasMaterialChanged = currentInstance.m_materialHash != hash::kEmptyHash &&
                                                   currentInstance.m_materialHash != materialInstanceHash;
            currentInstance.m_materialHash = materialInstanceHash;
            if (currentInstance.m_hasMaterialChanged) {
                notifySceneChanged();
            }
            currentInstance.m_texcoordHash = drawCall.geometry[hash::HashComponent::Texcoords];
            currentInstance.m_indexHash = drawCall.geometry[hash::HashComponent::Indices];
            currentInstance.m_associatedGeometryHash = drawCall.assetHash;
            if (drawCall.materialType == MaterialType::Opaque) {
                currentInstance.m_isSubsurface = drawCall.subsurface.diffusionProfile;
                currentInstance.m_subsurfaceKind = classifySubsurface(drawCall.subsurface);
            } else {
                currentInstance.m_subsurfaceKind = SubsurfaceKind::None;
            }
        }

        // Update transform
        // Heuristic: motion vectors on translucent surfaces cannot be trusted.
        const bool isMotionUnstable = currentInstance.m_materialType == MaterialType::Translucent ||
                                      currentInstance.m_categoryFlags.test(InstanceCategories::Particle) ||
                                      currentInstance.m_categoryFlags.test(InstanceCategories::WorldUI);
        // Previous positions are only valid on the frame the BLAS was refit.
        const bool previousPositionsValidThisFrame = blas.frameLastUpdated == currentFrame;
        hasPreviousPositions = previousPositionsValidThisFrame && blas.previousPositionsDefined && !isMotionUnstable;
        const bool isFirstUpdateAfterCreation = currentInstance.isCreatedThisFrame(currentFrame) && isFirstUpdateThisFrame;

        Mat4f objectToWorld = drawCall.objectToWorld;
        // Hack upstream (TREX-2272): offset world-space UI backgrounds backwards along their Z axis.
        const float backgroundOffset = worldSpaceUiBackgroundOffset();
        if (backgroundOffset != 0.f && currentInstance.m_categoryFlags.test(InstanceCategories::WorldMatte)) {
            for (int k = 0; k < 4; ++k) {
                objectToWorld[12 + k] += objectToWorld[8 + k] * backgroundOffset;
            }
        }

        // Update the transform based on what state we're in
        if (isFirstUpdateAfterCreation) {
            hasTransformChanged = currentInstance.teleport(objectToWorld);
        } else if (isFirstUpdateThisFrame) {
            hasTransformChanged = currentInstance.move(objectToWorld);
        } else {
            hasTransformChanged = currentInstance.moveAgain(objectToWorld);
        }
        if (hasTransformChanged) {
            notifySceneChanged();
            currentInstance.m_blasDirty = true;
        }
        currentInstance.m_isStatic =
            !(hasTransformChanged || hasPreviousPositions) || currentInstance.m_materialType == MaterialType::RayPortal;
    }

    const auto currentInstancesToObject = currentInstance.m_instancesToObject;
    const std::size_t currentInstancesToObjectSize = currentInstancesToObject ? currentInstancesToObject->size() : 0;
    const bool accelerationStructureKeyChanged = previousCategoryFlags != currentInstance.m_categoryFlags ||
                                                 previousIsSubsurface != currentInstance.m_isSubsurface ||
                                                 previousInstancesToObject.get() != currentInstancesToObject.get() ||
                                                 previousInstancesToObjectSize != currentInstancesToObjectSize;
    if (accelerationStructureKeyChanged) {
        notifySceneChanged();
        currentInstance.m_blasDirty = true;
    }

    update.hasTransformChanged = hasTransformChanged;
    update.hasPreviousPositions = hasPreviousPositions;

    // InstanceManager::preserveInstance at the end of updateInstance: camera registration and onInstanceUpdated.
    currentInstance.registerCamera(drawCall.cameraType);
    const bool fireEvents = isFirstUpdateThisFrame || overridePreviousCameraUpdate;
    if (fireEvents) {
        for (auto& handler : m_eventHandlers) {
            if (handler.onInstanceUpdated) {
                handler.onInstanceUpdated(currentInstance, drawCall, true, hasTransformChanged, hasPreviousPositions);
            }
        }
    }
}

void InstanceManager::preserveInstance(RtInstance& instance, const SceneDrawInput& drawCall, std::uint32_t currentFrame) {
    // SceneManager::preserveInstance
    BlasEntry* pBlas = instance.getBlas();
    if (pBlas == nullptr) {
        return;
    }
    const bool isFirstUpdateThisFrame = instance.setFrameLastUpdated(currentFrame);
    // One-shot cleanup for the first preserve frame after a dynamic update.
    if (!instance.m_isPreservePath) {
        instance.m_isPreservePath = true;
        // The last dynamic update may have left hasMaterialChanged == true.
        instance.m_hasMaterialChanged = false;
        // The last dynamic update may have left prevObjectToWorld != objectToWorld and isStatic == false (e.g.
        // after a transform-changing move()). Re-sync once.
        if (!instance.m_isStatic) {
            instance.m_prevObjectToWorld = instance.m_objectToWorld;
            instance.m_isStatic = true;
        }
    }
    // On the first preserve encounter per frame, release the previous positions.
    if (pBlas->frameLastTouched != currentFrame) {
        pBlas->previousPositionsDefined = false;
    }
    pBlas->frameLastTouched = currentFrame;
    // The BLAS input is refreshed with this frame's draw state (preserveReplacementInstance).
    pBlas->input = drawCall;

    // InstanceManager::preserveInstance: camera registration; onInstanceUpdated with no material and no motion.
    instance.registerCamera(drawCall.cameraType);
    for (auto& handler : m_eventHandlers) {
        if (handler.onInstanceUpdated) {
            handler.onInstanceUpdated(instance, drawCall, false, false, false);
        }
    }
    (void)isFirstUpdateThisFrame;
}

void InstanceManager::removeInstance(RtInstance* instance) {
    // Listeners run first (the scene model detaches the instance from its ReplacementInstance's prims and
    // releases its subsurface bookkeeping), then the references are cleaned up.
    for (auto& handler : m_eventHandlers) {
        if (handler.onInstanceDestroyed) {
            handler.onInstanceDestroyed(*instance);
        }
    }
    instance->m_owner = nullptr;
    if (instance->m_linkedBlas != nullptr) {
        instance->m_linkedBlas->unlinkInstance(instance);
        instance->m_linkedBlas = nullptr;
    }
}

} // namespace fuse::relight::scene::instances
