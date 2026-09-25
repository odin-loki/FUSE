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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_options.h@0867d3c and
// src/dxvk/rtx_render/rtx_point_instancer_system.h@0867d3c (the cullingRadius / fadeStartRadius range
// callbacks). FUSE Relight RL-1.7: the instance tracking options (see instance_options.hpp).
#include <fuse/relight/scene/instances/instance_options.hpp>

#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <variant>

namespace fuse::relight::scene::instances {

namespace {

// Odr-use every option so each is emitted (and registered at start-up) by this translation unit: rtx.conf
// keys must resolve, and RL-1.5's by-name reads of rtx.sceneScale / rtx.uniqueObjectDistance find them.
[[maybe_unused]] const options::OptionBase* const kRegisteredOptions[] = {
    &InstanceOptions::sceneScale,
    &InstanceOptions::uniqueObjectDistance,
    &InstanceOptions::numFramesToKeepInstances,
    &InstanceOptions::numFramesToKeepBLAS,
    &InstanceOptions::enablePreservePath,
    &AntiCullingOptions::Object::enable,
    &AntiCullingOptions::Object::enableHighPrecisionAntiCulling,
    &AntiCullingOptions::Object::enableInfinityFarFrustum,
    &AntiCullingOptions::Object::hashInstanceWithBoundingBoxHash,
    &AntiCullingOptions::Object::numObjectsToKeep,
    &AntiCullingOptions::Object::fovScale,
    &AntiCullingOptions::Object::farPlaneScale,
    &AntiCullingOptions::Light::enable,
    &AntiCullingOptions::Light::numLightsToKeep,
    &AntiCullingOptions::Light::numFramesToExtendLightLifetime,
    &AntiCullingOptions::Light::fovScale,
    &PointInstancerOptions::enable,
    &PointInstancerOptions::cullingRadius,
    &PointInstancerOptions::fadeStartRadius,
};

template <typename T>
T borrowed(const char* name, T fallback) {
    if (const options::OptionBase* o = options::OptionManager::findOption(name)) {
        const options::OptionValue v = o->getResolvedValue();
        if (const T* value = std::get_if<T>(&v)) {
            return *value;
        }
    }
    return fallback;
}

} // namespace

bool InstanceOptions::enableFreeCamera() {
    return borrowed<bool>("rtx.camera.enableFreeCamera", false); // RtCamera::enableFreeCamera default
}

void PointInstancerOptions::onCullingRadiusChanged(void*) {
    fadeStartRadius.setMaxValue(cullingRadius());
}

void PointInstancerOptions::onFadeStartRadiusChanged(void*) {
    cullingRadius.setMinValue(fadeStartRadius());
}

bool SubsurfaceOptions::enableThinOpaque() {
    return borrowed<bool>("rtx.subsurface.enableThinOpaque", true);
}

bool SubsurfaceOptions::enableDiffusionProfile() {
    return borrowed<bool>("rtx.subsurface.enableDiffusionProfile", true);
}

float SubsurfaceOptions::surfaceThicknessScale() {
    return borrowed<float>("rtx.subsurface.surfaceThicknessScale", 1.0f);
}

void registerInstanceOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
}

void applySceneScale(capture::geometry::GeometryCaptureConfig& config) {
    config.sceneScale = InstanceOptions::sceneScale();
}

} // namespace fuse::relight::scene::instances
