/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_options.h@0867d3c and src/d3d9/d3d9_rtx.h@0867d3c
// (the options draw classification reads; names, types, defaults and descriptions as upstream).
//
// Every option here is the RL-0.6 twin of the Remix option of the same full name, so rtx.conf /
// user.conf / dxvk.conf lines written for Remix drive the classifier unchanged. Other Relight
// packages that need one of these must read it from here (ClassifyOptions::x()), not declare it
// again: the options registry keeps only the first declaration of a name.
//
// Borrowed (declared by the owning package, read by name with its Remix default as fallback):
//   rtx.terrainBaker.enableBaking   (terrain baker, plan terrain/)
#pragma once

#include <fuse/relight/options/option.hpp>

#include <cstdint>

namespace fuse::relight::scene {

/// Remix SkyAutoDetectMode.
enum class SkyAutoDetectMode : std::int32_t {
    None = 0,
    CameraPosition,
    CameraPositionAndDepthFlags,
};

struct ClassifyOptions {
private:
    // Remix RtxOptions::*DecalTexturesOnChange: migrate the deprecated lists into rtx.decalTextures.
    static void onDynamicDecalTexturesChanged(void* context);
    static void onSingleOffsetDecalTexturesChanged(void* context);
    static void onNonOffsetDecalTexturesChanged(void* context);

public:
    using HashSet = options::HashSet;
    using Vec2i = options::Vec2i;

    FUSE_RELIGHT_OPTION("rtx", bool, orthographicIsUI, true, "When enabled, draw calls that are orthographic will be considered as UI.");
    FUSE_RELIGHT_OPTION("rtx", bool, preTransformedVerticesIsUI, false, "When enabled, draw calls using pre-transformed (screen-space) vertices will be considered as UI. This is typical for D3D8/D3D9 games that render UI with RHW vertices.");
    FUSE_RELIGHT_OPTION("rtx", bool, allowCubemaps, false, "When enabled, cubemaps from the game are processed through Remix, but they may not render correctly.");
    FUSE_RELIGHT_OPTION("rtx", bool, useVertexCapture, true, "When enabled, injects code into the original vertex shader to capture final shaded vertex positions.  Is useful for games using simple vertex shaders, that still also set the fixed function transform matrices.");
    FUSE_RELIGHT_OPTION_ENV("rtx", bool, enableRaytracing, true, "DXVK_ENABLE_RAYTRACING",
                        "Globally enables or disables ray tracing. When set to false the original game should render mostly as it would in DXVK typically.\n"
                        "Some artifacts may still appear however compared to the original game either due to issues with the underlying DXVK translation or issues in Remix itself.");
    FUSE_RELIGHT_OPTION("rtx", bool, skipDrawCallsPostRTXInjection, false, "Ignores all draw calls recorded after RTX Injection, the location of which varies but is currently based on when tagged UI textures begin to draw.");
    FUSE_RELIGHT_OPTION("rtx", bool, enableAlphaBlend, true, "Enable rendering alpha blended geometry, used for partial opacity and other blending effects on various surfaces in many games.");
    FUSE_RELIGHT_OPTION("rtx", bool, enableAlphaTest, true, "Enable rendering alpha tested geometry, used for cutout style opacity in some games.");
    FUSE_RELIGHT_OPTION("rtx", Vec2i, drawCallRange, Vec2i(0, INT32_MAX), "");
    FUSE_RELIGHT_OPTION("rtx", bool, ignoreAllVertexColorBakedLighting, false, "If true, all baked lighting bound to all vertex colors will be ignored.");
    FUSE_RELIGHT_OPTION("rtx", std::uint32_t, skyDrawcallIdThreshold, 0, "It's common in games to render the skybox first, and so, this value provides a simple mechanism to identify those early draw calls that are untextured (textured draw calls can still use the Sky Textures functionality.");
    FUSE_RELIGHT_OPTION("rtx", float, skyMinZThreshold, 1.f, "If a draw call's viewport has min depth greater than or equal to this threshold, then assume that it's a sky.");
    FUSE_RELIGHT_OPTION("rtx", SkyAutoDetectMode, skyAutoDetect, SkyAutoDetectMode::None,
                        "Automatically tag sky draw calls using various heuristics.\n"
                        "0 = None\n"
                        "1 = CameraPosition - assume the first seen camera position is a sky camera.\n"
                        "2 = CameraPositionAndDepthFlags - assume the first seen camera position is a sky camera, if its draw call's depth test is disabled. If it's enabled, assume no sky camera.\n"
                        "Note: if all draw calls are marked as sky, then assume that there's no sky camera at all.");
    FUSE_RELIGHT_OPTION("rtx", float, skyAutoDetectUniqueCameraDistance, 1.0f,
                        "If multiple cameras are found, this threshold distance (in game units) is used to distinguish a sky camera from a main camera. "
                        "Active if sky auto-detect is set to CameraPosition / CameraPositionAndDepthFlags.");
    FUSE_RELIGHT_OPTION("rtx.terrain", bool, terrainAsDecalsEnabledIfNoBaker, false, "If terrain baker is disabled, attempt to blend with the decals.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, raytracedRenderTargetTextures, {},
                        "DescriptorHashes for Render Targets. (Screens that should display the output of another camera).");
    FUSE_RELIGHT_OPTION("rtx", HashSet, lightmapTextures, {},
                        "Textures used for lightmapping (baked static lighting on surfaces) in older games.\n"
                        "These textures will be ignored when attempting to determine the desired textures from a draw to use for ray tracing.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, skyBoxTextures, {},
                        "Textures on draw calls used for the sky or are otherwise intended to be very far away from the camera at all times (no parallax).\n"
                        "Any draw calls using a texture in this list will be treated as sky and rendered as such in a manner different from typical geometry.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, skyBoxGeometries, {},
                        "Geometries from draw calls used for the sky or are otherwise intended to be very far away from the camera at all times (no parallax).\n"
                        "Any draw calls using a geometry hash in this list will be treated as sky and rendered as such in a manner different from typical geometry.\n"
                        "The geometry hash being used for sky detection is based off of the asset hash rule, see: \"rtx.geometryAssetHashRuleString\".");
    FUSE_RELIGHT_OPTION("rtx", HashSet, ignoreTextures, {},
                        "Textures on draw calls that should be ignored.\n"
                        "Any draw call using an ignore texture will be skipped and not ray traced, useful for removing undesirable rasterized effects or geometry not suitable for ray tracing.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, ignoreLights, {},
                        "Lights that should be ignored.\nAny matching light will be skipped and not added to be ray traced.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, uiTextures, {},
                        "Textures on draw calls that should be treated as screenspace UI elements.\n"
                        "All exclusively UI-related textures should be classified this way and doing so allows the UI to be rasterized on top of the ray traced scene like usual.\n"
                        "Note that currently the first UI texture encountered triggers RTX injection (though this may change in the future as this does cause issues with games that draw UI mid-frame).");
    FUSE_RELIGHT_OPTION("rtx", HashSet, worldSpaceUiTextures, {},
                        "Textures on draw calls that should be treated as worldspace UI elements.\n"
                        "Unlike typical UI textures this option is useful for improved rendering of UI elements which appear as part of the scene (moving around in 3D space rather than as a screenspace element).");
    FUSE_RELIGHT_OPTION("rtx", HashSet, worldSpaceUiBackgroundTextures, {},
                        "Hack/workaround option for dynamic world space UI textures with a coplanar background.\n"
                        "Apply to backgrounds if the foreground material is a dynamic world texture rendered in UI that is unpredictable and rapidly changing.\n"
                        "This offsets the background texture backwards.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, hideInstanceTextures, {},
                        "Textures on draw calls that should be hidden from rendering, but not totally ignored.\n"
                        "This is similar to rtx.ignoreTextures but instead of completely ignoring such draw calls they are only hidden from rendering, allowing for the hidden objects to still appear in captures.\n"
                        "As such, this is mostly only a development tool to hide objects during development until they are properly replaced, otherwise the objects should be ignored with rtx.ignoreTextures instead for better performance.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, playerModelTextures, {},
                        "Textures on draw calls that are part of the third-person player model, such as body, head, or held equipment.\n"
                        "By default, tagged instances appear in shadows and reflections but are not drawn in the primary camera view.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, playerModelBodyTextures, {},
                        "Textures on the body/root of the third-person player model.\n"
                        "Apply together with third_person_player_model to give the body the same visibility behavior. This category also identifies the anchor Remix uses to filter nearby player-model parts and position virtual instances through portals.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, particleTextures, {},
                        "Textures on draw calls that should be treated as particles.\n"
                        "When objects are marked as particles more approximate rendering methods are leveraged allowing for more effecient and typically better looking particle rendering.\n"
                        "Generally any billboard-like blended particle objects in the original application should be classified this way.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, hairCardTextures, {},
                        "Textures on draw calls that should be treated as alpha-tested hair cards.\n"
                        "Tagged materials preserve fine texture detail, render as cutouts instead of alpha blends, and disable backface culling.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, beamTextures, {},
                        "Textures on draw calls that are already particles or emissively blended and have beam-like geometry.\n"
                        "Typically objects marked as particles or objects using emissive blending will be rendered with a special method which allows re-orientation of the billboard geometry assumed to make up the draw call in indirect rays (reflections for example).\n"
                        "This method works fine for typical particles, but some (e.g. a laser beam) may not be well-represented with the typical billboard assumption of simply needing to rotate around its centroid to face the view direction.\n"
                        "To handle such cases a different beam mode is used to treat objects as more of a cylindrical beam and re-orient around its main spanning axis, allowing for better rendering of these beam-like effect objects.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, decalTextures, {},
                        "Textures on draw calls used for static geometric decals or decals with complex topology.\n"
                        "These materials will be blended over the materials underneath them when decal material blending is enabled.\n"
                        "A small configurable offset is applied to each flat/co-planar part of these decals to prevent coplanar geometric cases (which poses problems for ray tracing).");
    FUSE_RELIGHT_OPTION_ARGS("rtx", HashSet, dynamicDecalTextures, {},
                        "Warning: This option is deprecated, please use rtx.decalTextures instead.\n"
                        "Textures on draw calls used for dynamically spawned geometric decals, such as bullet holes.\n"
                        "These materials will be blended over the materials underneath them when decal material blending is enabled.\n"
                        "A small configurable offset is applied to each quad part of these decals to prevent coplanar geometric cases (which poses problems for ray tracing).",
                        args.onChangeCallback = &onDynamicDecalTexturesChanged);
    FUSE_RELIGHT_OPTION_ARGS("rtx", HashSet, singleOffsetDecalTextures, {},
                        "Warning: This option is deprecated, please use rtx.decalTextures instead.\n"
                        "Textures on draw calls used for geometric decals that don't inter-overlap for a given texture hash. Textures must be tagged as \"Decal Texture\" or \"Dynamic Decal Texture\" to apply.\n"
                        "Applies a single shared offset to all the batched decal geometry rendered in a given draw call, rather than increasing offset per decal within the batch (i.e. a quad in case of \"Dynamic Decal Texture\").\n"
                        "Note, the offset adds to the global offset among all decals drawn with different draw calls.\n"
                        "The decal textures tagged this way must not inter-overlap within a batch / single draw call since the same offset is applied to all of them.\n"
                        "Applying a single offset is useful for stabilizing decal offsets when a game dynamically batches decals together.\n"
                        "In addition, it makes the global decal offset index grow slower and thus it minimizes a chance of hitting the \"rtx.decals.maxOffsetIndex limit\".",
                        args.onChangeCallback = &onSingleOffsetDecalTexturesChanged);
    FUSE_RELIGHT_OPTION_ARGS("rtx", HashSet, nonOffsetDecalTextures, {},
                        "Warning: This option is deprecated, please use rtx.decalTextures instead.\n"
                        "Textures on draw calls used for geometric decals with arbitrary topology that are already offset from the base geometry.\n"
                        "These materials will be blended over the materials underneath them when decal material blending is enabled.\n"
                        "Unlike typical decals however these decals have no offset applied to them due assuming the offset is already being done by whatever is passing data to Remix.",
                        args.onChangeCallback = &onNonOffsetDecalTexturesChanged);
    FUSE_RELIGHT_OPTION("rtx", HashSet, terrainTextures, {}, "Albedo textures that are baked blended together to form a unified terrain texture used during ray tracing.\n"
                        "Put albedo textures into this category if the game renders terrain as a blend of multiple textures.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, opacityMicromapIgnoreTextures, {}, "Textures to ignore when generating Opacity Micromaps. This generally does not have to be set and is only useful for black listing problematic cases for Opacity Micromap usage.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, animatedWaterTextures, {},
                        "Textures on draw calls to be treated as \"animated water\".\n"
                        "Objects with this flag applied will animate their normals to fake a basic water effect based on the layered water material parameters, and only when rtx.opaqueMaterial.layeredWaterNormalEnable is set to true.\n"
                        "Should typically be used on static water planes that the original application may have relied on shaders to animate water on.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, ignoreBakedLightingTextures, {},
                        "Textures for which to ignore two types of baked lighting, Texture Factors and Vertex Color.\n\n"
                        "Texture Factor disablement:\n"
                        "Using this feature on selected textures will eliminate the texture factors.\n"
                        "For instance, if a game bakes lighting information into the Texture Factor for particular textures, applying this option will remove them.\n"
                        "This becomes useful when unexpected results occur due to the Texture Factor.\n"
                        "Consider an example where the original texture contains red tints baked into the Texture Factor. If a user replaces the texture, it will blend with the red tints, resulting in an undesirable reddish outcome.\n"
                        "In such cases, users can employ this option to eliminate the unwanted tints from their replacement textures.\n"
                        "Similarly, users can tag textures if shadows are baked into the Texture Factor, causing the replacing texture to appear darker than anticipated.\n\n"
                        "Vertex Color disablement:\n"
                        "Using this feature on selected textures will eliminate the vertex colors.\n\n"
                        "Note, enabling this setting will automatically disable multiple-stage texture factor blendings for the selected textures.\n"
                        "Only use this option when necessary, as the Texture Factor and Vertex Color can be used for simulating various texture effects, tagging a texture with this option will unexpectedly eliminate these effects.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, ignoreAlphaOnTextures, {},
                        "Textures for which to ignore the alpha channel of the legacy colormap. Textures will be rendered fully opaque as a result.");
    FUSE_RELIGHT_OPTION("rtx.antiCulling", HashSet, antiCullingTextures, {},
                        "Textures that are forced to extend life length when anti-culling is enabled.\n"
                        "Some games use different culling methods we can't fully match, use this option to manually add textures to force extend their life when anti-culling fails.");
    FUSE_RELIGHT_OPTION("rtx.postfx", HashSet, motionBlurMaskOutTextures, {}, "Disable motion blur for meshes with specific texture.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, particleEmitterTextures, {}, "Objects rendered with these textures will emit particles that inherit the material of the object itself.");
    FUSE_RELIGHT_OPTION("rtx", HashSet, smoothNormalsTextures, {},
                        "Textures on draw calls whose geometry should have smooth normals generated on the GPU.\n"
                        "This is useful for older D3D9 games where the geometry may be missing smooth normals, especially when using the VertexShader Capture mechanism.\n"
                        "When a draw call matches, area-weighted smooth normals will be computed from the triangle mesh and used for ray tracing.");

    /// rtx.raytracedRenderTarget.* (Remix RtxOptions::RaytracedRenderTarget).
    struct RaytracedRenderTarget {
        FUSE_RELIGHT_OPTION("rtx.raytracedRenderTarget", bool, enable, true,
                            "Enables or disables raytracing for render-to-texture effects.  The render target to be raytraced must be specified in the texture selection menu.");
    };

    /// rtx.terrainBaker.enableBaking (borrowed, Remix default true).
    static bool terrainBakerEnableBaking();
    /// Remix TerrainBaker::needsTerrainBaking(): enableBaking() && terrainTextures non-empty.
    static bool needsTerrainBaking();
};

} // namespace fuse::relight::scene
