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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_options.h@0867d3c (sceneScale, uniqueObjectDistance,
// numFramesToKeepInstances, numFramesToKeepBLAS, enablePreservePath, AntiCulling::Object / Light) and
// src/dxvk/rtx_render/rtx_point_instancer_system.h@0867d3c (rtx.pointInstancer.*). Names, types, defaults,
// environment variables and descriptions as upstream.
//
// RL-1.7 owns these rtx.* options (RL-0.6 registry; each also answers to its relight.* twin). Other packages
// read them by name (scene/lights: rtx.sceneScale, scene/camera: rtx.uniqueObjectDistance) or from here.
//
// Borrowed (declared by another package, read by name with the Remix default as fallback):
//   rtx.camera.enableFreeCamera          (rtx_camera.h, default false; owner: camera / overlay)
//   rtx.subsurface.enableThinOpaque      (default true; owner: materials)
//   rtx.subsurface.enableDiffusionProfile(default true; owner: materials)
//   rtx.subsurface.surfaceThicknessScale (default 1; owner: materials)
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_types.hpp>

#include <cstdint>

namespace fuse::relight::capture::geometry {
struct GeometryCaptureConfig;
}

namespace fuse::relight::scene::instances {

struct InstanceOptions {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f,
                        "Defines the ratio of rendering unit (1cm) to game unit, i.e. sceneScale = 1cm / GameUnit.");
    // Needs to be > 0
    FUSE_RELIGHT_OPTION_ARGS("rtx", float, uniqueObjectDistance, 300.f,
                             "The distance (in game units) that an object can move in a single frame before it is no longer considered the same object.\n"
                             "If this is too low, fast moving objects may flicker and have bad lighting.  If it's too high, repeated objects may flicker.\n"
                             "This does not account for sceneScale.",
                             args.minValue = 0.f);
    FUSE_RELIGHT_OPTION("rtx", std::uint32_t, numFramesToKeepInstances, 1, "");
    FUSE_RELIGHT_OPTION("rtx", std::uint32_t, numFramesToKeepBLAS, 1, "");
    FUSE_RELIGHT_OPTION("rtx", bool, enablePreservePath, true,
                        "Reuse the instance state of draw calls that are unchanged since the last frame (same geometry, "
                        "material and transform) instead of re-processing them.");

    /// RtxOptions::getUniqueObjectDistanceSqr.
    static float uniqueObjectDistanceSqr() { return uniqueObjectDistance() * uniqueObjectDistance(); }
    /// RtxOptions::getMeterToWorldUnitScale: Remix world units are centimetres.
    static float meterToWorldUnitScale() { return 100.f * sceneScale(); }
    /// RtxOptions::numFramesToKeepGeometryData.
    static std::uint32_t numFramesToKeepGeometryData() { return numFramesToKeepBLAS(); }

    /// rtx.camera.enableFreeCamera (borrowed; false when no package declares it).
    static bool enableFreeCamera();
};

/// RtxOptions::AntiCulling.
struct AntiCullingOptions {
    struct Object {
        FUSE_RELIGHT_OPTION_ENV("rtx.antiCulling.object", bool, enable, false, "RTX_ANTI_CULLING_OBJECTS",
                                "Extends lifetime of objects that go outside the camera frustum (anti-culling frustum).");
        FUSE_RELIGHT_OPTION("rtx.antiCulling.object", bool, enableHighPrecisionAntiCulling, true,
                            "Use robust intersection check with Separate Axis Theorem.\n"
                            "This method is slightly expensive but it effectively addresses object flickering issues that arise from corner cases in the fast intersection check method.\n"
                            "Typically, it's advisable to enable this option unless it results in a notable performance drop; otherwise, the presence of flickering artifacts could significantly diminish the overall image quality.");
        FUSE_RELIGHT_OPTION("rtx.antiCulling.object", bool, enableInfinityFarFrustum, false,
                            "Enable infinity far plane frustum for anti-culling.");
        FUSE_RELIGHT_OPTION("rtx.antiCulling.object", bool, hashInstanceWithBoundingBoxHash, true,
                            "Hash instances with bounding box hash for object duplication check.\n Disable this when the game using primitive culling which may cause flickering.");
        // TODO: This should be a threshold of memory size
        FUSE_RELIGHT_OPTION("rtx.antiCulling.object", std::uint32_t, numObjectsToKeep, 10000,
                            "The maximum number of RayTracing instances to keep when Anti-Culling is enabled.");
        FUSE_RELIGHT_OPTION("rtx.antiCulling.object", float, fovScale, 1.0f,
                            "Scale applied to the FOV of Anti-Culling Frustum for matching the culling frustum in the original game.");
        FUSE_RELIGHT_OPTION("rtx.antiCulling.object", float, farPlaneScale, 10.0f,
                            "Scale applied to the far plane for Anti-Culling Frustum for matching the culling frustum in the original game.");
    };
    struct Light {
        FUSE_RELIGHT_OPTION_ENV("rtx.antiCulling.light", bool, enable, false, "RTX_ANTI_CULLING_LIGHTS",
                                "Enable Anti-Culling for lights.");
        FUSE_RELIGHT_OPTION("rtx.antiCulling.light", std::uint32_t, numLightsToKeep, 1000, "(DEPRECATED)");
        FUSE_RELIGHT_OPTION("rtx.antiCulling.light", std::uint32_t, numFramesToExtendLightLifetime, 1000,
                            "Maximum number of frames to keep  when Anti-Culling is enabled. Make sure not to set this too low (then the anti-culling won't work), nor too high (which will hurt the performance).");
        FUSE_RELIGHT_OPTION("rtx.antiCulling.light", float, fovScale, 1.0f, "Scalar of the FOV of lights Anti-Culling Frustum.");
    };

    static bool isObjectAntiCullingEnabled() { return Object::enable() && !InstanceOptions::enableFreeCamera(); }
    static bool isLightAntiCullingEnabled() { return Light::enable() && !InstanceOptions::enableFreeCamera(); }
};

/// RtxPointInstancerSystem's options.
struct PointInstancerOptions {
    // Ensure fadeStartRadius stays below cullingRadius and cullingRadius above fadeStartRadius. Declared before
    // the options: their initializers are not a complete-class context.
    static void onCullingRadiusChanged(void* context);
    static void onFadeStartRadiusChanged(void* context);

    FUSE_RELIGHT_OPTION("rtx.pointInstancer", bool, enable, true,
                        "Enables radius-based culling for USD PointInstancer replacements. "
                        "When disabled, all instances are submitted to the TLAS regardless of distance.");
    FUSE_RELIGHT_OPTION_ARGS("rtx.pointInstancer", float, cullingRadius, 5000.f,
                             "Maximum distance (in world units) from the camera beyond which "
                             "PointInstancer instances are culled. Instances farther than this "
                             "distance are not included in the TLAS.",
                             args.minValue = 0.f;
                             args.onChangeCallback = &onCullingRadiusChanged);
    FUSE_RELIGHT_OPTION_ARGS("rtx.pointInstancer", float, fadeStartRadius, 0.f,
                             "Distance (in world units) from the camera at which instances begin "
                             "to be stochastically removed to create a smooth density falloff. "
                             "Set to 0 to disable the fade region (hard culling boundary only). "
                             "Must be less than cullingRadius.",
                             args.minValue = 0.f;
                             args.onChangeCallback = &onFadeStartRadiusChanged);

};

/// The rtx.subsurface.* switches the foliage bookkeeping reads (borrowed, Remix defaults as fallback).
struct SubsurfaceOptions {
    static bool enableThinOpaque();
    static bool enableDiffusionProfile();
    static float surfaceThicknessScale();
};

/// Registers every option of this package (odr-use; called by SceneModel). Idempotent.
void registerInstanceOptions();

/// RL-1.3 reads rtx.sceneScale through its config: GeometryCaptureConfig::sceneScale set from rtx.sceneScale.
void applySceneScale(capture::geometry::GeometryCaptureConfig& config);

} // namespace fuse::relight::scene::instances
