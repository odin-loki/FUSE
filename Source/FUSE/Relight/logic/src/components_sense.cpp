/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/components/{camera,keyboard_input,mesh_hash_checker,
// texture_hash_checker,light_hash_checker,fog_hash_checker,mesh_proximity,ray_mesh_intersection,angle_to_mesh,
// read_transform,read_bone_transform,sphere_light_override}.h@0867d3c
//
// FUSE Relight RL-3.5: "Sense" components (and the non-functional SphereLightOverride). The renderer queries of
// upstream (SceneManager, RtCamera, LightManager, BLAS bounding boxes, skinning data, the developer-menu key state)
// are read from FrameInputs / PrimSnapshot (logic_context.hpp); the computations on them are upstream's.
#include <fuse/relight/logic/animation_utils.hpp>
#include <fuse/relight/logic/keybind.hpp>

#include "component_list_internal.hpp"
#include "component_macros.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace fuse::relight::logic::components {

namespace {

using PT = PropertyType;

// ---- Camera ---------------------------------------------------------------------------------------------------------

#define LIST_INPUTS(X)
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), position, "Position", "The current camera position in world space.")        \
    X(PT::Float3, Vector3(0.0f, 0.0f, -1.0f), forward, "Forward", "The camera's normalized forward direction vector in world space.") \
    X(PT::Float3, Vector3(1.0f, 0.0f, 0.0f), right, "Right", "The camera's normalized right direction vector in world space.") \
    X(PT::Float3, Vector3(0.0f, 1.0f, 0.0f), up, "Up", "The camera's normalized up direction vector in world space.")    \
    X(PT::Float, kDefaultFovRadians, fovRadians, "FOV (radians)",                                                       \
      "The Y axis (vertical) Field of View of the camera in radians. Note this value will always be positive.")         \
    X(PT::Float, 60.0f, fovDegrees, "FOV (degrees)",                                                                    \
      "The Y axis (vertical) Field of View of the camera in degrees. Note this value will always be positive.")         \
    X(PT::Float, 1.0f, aspectRatio, "Aspect Ratio", "The camera's aspect ratio (width/height).")                        \
    X(PT::Float, 0.1f, nearPlane, "Near Plane", "The camera's near clipping plane distance.")                           \
    X(PT::Float, 1000.0f, farPlane, "Far Plane", "The camera's far clipping plane distance.")
FUSE_LOGIC_COMPONENT(Camera, "Camera", "Sense",
                     "Provides information about the camera's position, direction, and field of view.\n\nOutputs current camera "
                     "properties including position, orientation vectors, and projection parameters.\n\nUses free camera when "
                     "both 'rtx.camera.useFreeCameraForComponents' and free camera are enabled.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
void Camera::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    const CameraSense defaults;
    const CameraSense& c = ctx.inputs().camera.valid ? ctx.inputs().camera : defaults;
    for (std::size_t i = start; i < end; i++) {
        m_position[i] = c.position;
        m_forward[i] = c.forward;
        m_right[i] = c.right;
        m_up[i] = c.up;
        m_fovRadians[i] = c.fovRadians;
        m_fovDegrees[i] = c.fovRadians * (180.0f / kPi);
        m_aspectRatio[i] = c.aspectRatio;
        m_nearPlane[i] = c.nearPlane;
        m_farPlane[i] = c.farPlane;
    }
}

// ---- KeyboardInput --------------------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::String, std::string("A"), keyString, "Key String",                                                            \
      "The key combination string to detect.\nExamples: 'A', 'CTRL, A', 'SHIFT, SPACE'.\nFull list of key names "        \
      "available in `src/util/util_keybind.h`.")
#define LIST_STATES(X) \
    X(PT::Bool, false, wasPressedLastFrame, "", "Internal state to track if the key was pressed in the previous frame.")
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Bool, false, isPressed, "Is Pressed", "True if the key combination is currently being pressed.")              \
    X(PT::Bool, false, wasJustPressed, "Was Just Pressed", "True if the key combination was just pressed this frame.")   \
    X(PT::Bool, false, wasClicked, "Was Clicked",                                                                       \
      "True for one frame after the key combination is released (press then release cycle).")
FUSE_LOGIC_COMPONENT(KeyboardInput, "Keyboard Input", "Sense",
                     "Detects when keyboard keys are pressed, held, or released.\n\nChecks the state of a keyboard key or key "
                     "combination using the same format as RTX options.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
void KeyboardInput::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    const FrameInputs& in = ctx.inputs();
    for (std::size_t i = start; i < end; i++) {
        std::vector<std::uint32_t> keys;
        if (parseVirtualKeys(m_keyString[i], keys) && !keys.empty()) {
            bool currentlyPressed = true;
            for (std::uint32_t k : keys) {
                currentlyPressed = currentlyPressed && in.keysDown.count(k) != 0;
            }
            // Upstream checkHotkeyState(keys, false): the combination is held and its last key went down this frame.
            const bool justPressed = currentlyPressed && in.keysPressed.count(keys.back()) != 0;
            const bool wasClicked = m_wasPressedLastFrame[i] && !currentlyPressed;
            m_isPressed[i] = currentlyPressed;
            m_wasJustPressed[i] = justPressed;
            m_wasClicked[i] = wasClicked;
            m_wasPressedLastFrame[i] = currentlyPressed;
        } else {
            logOnce(LogSeverity::Error, "Failed to parse key string: '" + m_keyString[i] + "'");
            m_isPressed[i] = false;
            m_wasJustPressed[i] = false;
            m_wasClicked[i] = false;
            m_wasPressedLastFrame[i] = false;
        }
    }
}

// ---- Hash checkers --------------------------------------------------------------------------------------------------

std::uint32_t usage(const std::map<std::uint64_t, std::uint32_t>& table, std::uint64_t hash) {
    const auto it = table.find(hash);
    return it == table.end() ? 0u : it->second;
}

#define LIST_INPUTS(X) X(PT::Hash, 0x0, meshHash, "Mesh Hash", "The mesh hash to check for usage in the current frame.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Bool, false, isUsed, "Is Used", "True if the mesh hash was used in the current frame.")                       \
    X(PT::Float, 0.0f, usageCount, "Usage Count", "Number of times the mesh hash was used in the current frame.")
FUSE_LOGIC_COMPONENT(MeshHashChecker, "Mesh Hash Checker", "Sense",
                     "Detects if a specific mesh is currently being drawn in the scene.\n\nThis checks all meshes that the game "
                     "sends to Remix in the current frame, which will probably include meshes that are off camera or occluded.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void MeshHashChecker::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        const std::uint32_t count = m_meshHash[i] == 0 ? 0u : usage(ctx.inputs().meshHashUsage, m_meshHash[i]);
        m_isUsed[i] = count > 0;
        m_usageCount[i] = static_cast<float>(count);
    }
}

#define LIST_INPUTS(X) X(PT::Hash, 0x0, textureHash, "Texture Hash", "The texture hash to check for usage in the current frame.")
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Bool, false, isUsed, "Is Used", "True if the texture hash was used in the current frame.")                    \
    X(PT::Float, 0.0f, usageCount, "Usage Count", "Number of times the texture hash was used in the current frame.")
FUSE_LOGIC_COMPONENT(TextureHashChecker, "Texture Hash Checker", "Sense",
                     "Detects if a specific texture is being used in the current frame.\n\nChecks if a specific texture hash was "
                     "used for material replacement in the current frame. This includes textures in all categories, including "
                     "ignored textures.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void TextureHashChecker::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        const std::uint32_t count = m_textureHash[i] == 0 ? 0u : usage(ctx.inputs().textureHashUsage, m_textureHash[i]);
        m_isUsed[i] = count > 0;
        m_usageCount[i] = static_cast<float>(count);
    }
}

#define LIST_INPUTS(X) X(PT::Hash, 0x0, lightHash, "Light Hash", "The light hash to check for usage in the current frame.")
#define LIST_OUTPUTS(X) X(PT::Bool, false, isUsed, "Is Used", "True if the light hash was used in the current frame.")
FUSE_LOGIC_COMPONENT(LightHashChecker, "Light Hash Checker", "Sense",
                     "Detects if a specific light is currently active in the scene.\n\nChecks if a specific light hash is present "
                     "in the current frame's light table.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void LightHashChecker::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_isUsed[i] = ctx.inputs().lightHashes.count(m_lightHash[i]) != 0;
    }
}

#define LIST_INPUTS(X) X(PT::Hash, 0x0, fogHash, "Fog Hash", "The fog hash to check against the current frame's fog hash.")
#define LIST_OUTPUTS(X) X(PT::Bool, false, isMatch, "Is Match", "True if the given fog hash matches the current frame's fog hash.")
FUSE_LOGIC_COMPONENT(FogHashChecker, "Fog Hash Checker", "Sense",
                     "Detects if a specific fog state is currently active in the scene.\n\nChecks if a given fog hash matches the "
                     "current frame's fog hash.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
#undef LIST_STATES
void FogHashChecker::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_isMatch[i] = m_fogHash[i] == ctx.inputs().fogHash;
    }
}

// ---- Mesh queries ---------------------------------------------------------------------------------------------------

/// Signed distance from a point to an AABB (positive outside, negative inside), upstream calculateSignedDistanceToAABB.
float signedDistanceToAabb(const Vector3& point, const AxisAlignedBoundingBox& aabb) {
    const Vector3 minDist = aabb.minPos - point;
    const Vector3 maxDist = point - aabb.maxPos;
    const Vector3 distToFaces = max(minDist, maxDist);
    if (distToFaces.x <= 0.0f && distToFaces.y <= 0.0f && distToFaces.z <= 0.0f) {
        return std::max(distToFaces.x, std::max(distToFaces.y, distToFaces.z));
    }
    const Vector3 clampedPoint = clamp(point, aabb.minPos, aabb.maxPos);
    return length(point - clampedPoint);
}

const PrimSnapshot* meshWithBounds(const PrimSnapshot* prim) {
    if (prim == nullptr || prim->kind != PrimSnapshot::Kind::Mesh || !prim->bounds.isValid()) {
        return nullptr;
    }
    return prim;
}

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Prim, kInvalidPrimTarget, target, "Target",                                                                   \
      "The mesh prim to get bounding box from. Must be a UsdGeomMesh prim (the actual geometry).",                     \
      property.allowedPrimTypes = {PrimType::UsdGeomMesh})                                                              \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), worldPosition, "World Position",                                           \
      "The world space position to test against the mesh bounding box. This is often the `Position` output from the "  \
      "`Camera` component, if checking how close the camera is to the mesh.")                                           \
    X(PT::Float, 1.0f, inactiveDistance, "Inactive Distance",                                                           \
      "When the `World Position` is this far from the mesh's bounding box, `Activation Strength` will be 0. Positive "  \
      "numbers represent mean `World Position` is outside the box, negative numbers mean it is inside the box.",        \
      property.optional = true)                                                                                         \
    X(PT::Float, 0.0f, fullActivationDistance, "Full Activation Distance",                                              \
      "When the `World Position` is this far from the mesh's bounding box, `Activation Strength` will be 1. Positive "  \
      "numbers represent mean `World Position` is outside the box, negative numbers mean it is inside the box.",        \
      property.optional = true)                                                                                         \
    X(PT::Enum, static_cast<std::uint32_t>(InterpolationType::Linear), easingType, "Easing Type",                        \
      "The type of easing to apply to the `Activation Strength` output.  ", property.optional = true,                   \
      property.enumValues = interpolationTypeEnumValues())
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float, 0.0f, signedDistance, "Signed Distance",                                                               \
      "Distance in object space to the nearest point on the surface of the bounding box. Positive when outside the "    \
      "bounding box, negative when inside. Outputs a very large number when no valid bounding box is found. Because "   \
      "this is in object space, if the object is scaled, the distance may not correspond to world units.")              \
    X(PT::Float, 0.0f, activationStrength, "Activation Strength",                                                       \
      "Normalized 0-1 value: 0 when `Signed Distance` = `Inactive Distance`, 1 when it equals `Full Activation "        \
      "Distance`. This is often passed directly to the `Blend Strength` of `Rtx Option Layer Action` to enable a conf " \
      "layer when the camera is close to the mesh.")
FUSE_LOGIC_COMPONENT(MeshProximity, "Mesh Proximity", "Sense",
                     "Measures how far a point is from a mesh's bounding box. This can be used to determine if the camera is "
                     "close to a mesh, or inside of a room.\n\nCalculates the signed distance from a world position to a mesh's "
                     "bounding box. Positive values indicate the point is outside the bounding box, negative values indicate "
                     "it's inside.\n\nNote that the output is in object space, so if the mesh is scaled, the distance may not "
                     "correspond to world units.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void MeshProximity::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        float signedDistance = FLT_MAX;
        if (const PrimSnapshot* mesh = meshWithBounds(m_batch.resolvePrimTarget(ctx, i, m_target[i]))) {
            const Matrix4 worldToObject = inverse(mesh->objectToWorld);
            const Vector3 objectSpacePoint = transform(worldToObject, Vector4(m_worldPosition[i], 1.0f)).xyz();
            signedDistance = signedDistanceToAabb(objectSpacePoint, mesh->bounds);
        }
        if (signedDistance == FLT_MAX) {
            logOnce(LogSeverity::Error, "MeshProximity: No valid bounding box found.");
            m_signedDistance[i] = FLT_MAX;
            m_activationStrength[i] = 0.0f;
            continue;
        }
        m_signedDistance[i] = signedDistance;
        float activationStrength = 0.0f;
        const float inactiveDistance = m_inactiveDistance[i];
        const float fullActivationDistance = m_fullActivationDistance[i];
        if (fullActivationDistance != inactiveDistance) {
            float normalizedValue = (signedDistance - inactiveDistance) / (fullActivationDistance - inactiveDistance);
            normalizedValue = clampf(normalizedValue, 0.0f, 1.0f);
            activationStrength = applyInterpolation(static_cast<InterpolationType>(m_easingType[i]), normalizedValue);
        } else {
            activationStrength = (signedDistance <= fullActivationDistance) ? 1.0f : 0.0f;
        }
        m_activationStrength[i] = activationStrength;
    }
}

enum class IntersectionType : std::uint32_t {
    BoundingBox = 0,
};
const PropertySpec::EnumPropertyMap& intersectionTypeEnumValues() {
    static const PropertySpec::EnumPropertyMap kValues{
        {"Bounding Box", {IntersectionType::BoundingBox, "Test intersection against the mesh's axis-aligned bounding box."}}};
    return kValues;
}

/// Slab test (upstream rayIntersectsAABB).
bool rayIntersectsAabb(const Vector3& rayOrigin, const Vector3& rayDirection, const AxisAlignedBoundingBox& aabb) {
    if (!aabb.isValid()) {
        return false;
    }
    float tmin = 0.0f;
    float tmax = FLT_MAX;
    for (std::size_t i = 0; i < 3; i++) {
        if (std::abs(rayDirection[i]) < 1e-8f) {
            if (rayOrigin[i] < aabb.minPos[i] || rayOrigin[i] > aabb.maxPos[i]) {
                return false;
            }
        } else {
            const float invD = 1.0f / rayDirection[i];
            float t1 = (aabb.minPos[i] - rayOrigin[i]) * invD;
            float t2 = (aabb.maxPos[i] - rayOrigin[i]) * invD;
            if (t1 > t2) {
                std::swap(t1, t2);
            }
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) {
                return false;
            }
        }
    }
    return true;
}

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), rayOrigin, "Ray Origin", "The origin point of the ray in world space.")      \
    X(PT::Float3, Vector3(0.0f, 0.0f, 1.0f), rayDirection, "Ray Direction",                                             \
      "The direction of the ray in world space. Should be normalized.")                                                 \
    X(PT::Prim, kInvalidPrimTarget, target, "Target", "The mesh prim to test intersection against. Must be a mesh prim.", \
      property.allowedPrimTypes = {PrimType::UsdGeomMesh})                                                              \
    X(PT::Enum, static_cast<std::uint32_t>(IntersectionType::BoundingBox), intersectionType, "Intersection Type",       \
      "The type of intersection test to perform.", property.optional = true,                                            \
      property.enumValues = intersectionTypeEnumValues())
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Bool, false, intersects, "Intersects",                                                                        \
      "True if the ray intersects the mesh (based on the selected intersection type).")
FUSE_LOGIC_COMPONENT(RayMeshIntersection, "Ray Mesh Intersection", "Sense",
                     "Tests if a ray intersects with a mesh.\n\nPerforms a ray-mesh intersection test. Currently supports bounding "
                     "box intersection tests. Returns true if the ray intersects the mesh's bounding box.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void RayMeshIntersection::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        bool intersects = false;
        if (const PrimSnapshot* mesh = meshWithBounds(m_batch.resolvePrimTarget(ctx, i, m_target[i]))) {
            const Matrix4 worldToObject = inverse(mesh->objectToWorld);
            const Vector3 origin = transform(worldToObject, Vector4(m_rayOrigin[i], 1.0f)).xyz();
            const Vector3 direction = normalize(transform(worldToObject, Vector4(m_rayDirection[i], 0.0f)).xyz());
            if (static_cast<IntersectionType>(m_intersectionType[i]) == IntersectionType::BoundingBox) {
                intersects = rayIntersectsAabb(origin, direction, mesh->bounds);
            }
        }
        m_intersects[i] = intersects;
    }
}

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), worldPosition, "World Position",                                           \
      "The world space position to use as the origin of the ray.")                                                      \
    X(PT::Float3, Vector3(0.0f, 0.0f, 1.0f), direction, "Direction",                                                    \
      "The direction vector of the ray (does not need to be normalized).")                                              \
    X(PT::Prim, kInvalidPrimTarget, target, "Target", "The mesh prim to get the centroid from. Must be a mesh prim.",   \
      property.allowedPrimTypes = {PrimType::UsdGeomMesh})
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float, 0.0f, angleDegrees, "Angle (Degrees)",                                                                 \
      "The angle in degrees between the ray direction and the direction to the mesh centroid.")                         \
    X(PT::Float, 0.0f, angleRadians, "Angle (Radians)",                                                                 \
      "The angle in radians between the ray direction and the direction to the mesh centroid.")                         \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), directionToCentroid, "Direction to Centroid",                              \
      "The normalized direction vector from the world position to the mesh centroid.")
FUSE_LOGIC_COMPONENT(AngleToMesh, "Angle to Mesh", "Sense",
                     "Measures the angle between a ray and a mesh's center point.  This can be used to determine if the camera "
                     "is looking at a mesh.\n\nCalculates the angle between a ray (from position + direction) and the direction "
                     "to a mesh's transformed centroid.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void AngleToMesh::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        float angleDegrees = 0.0f;
        float angleRadians = 0.0f;
        Vector3 dirToCentroid(0.0f, 0.0f, 0.0f);
        const PrimSnapshot* prim = m_batch.resolvePrimTarget(ctx, i, m_target[i]);
        if (prim != nullptr && prim->kind == PrimSnapshot::Kind::Mesh) {
            if (prim->bounds.isValid()) {
                const Vector3 worldSpaceCentroid = prim->bounds.getTransformedCentroid(prim->objectToWorld);
                const Vector3 toCentroid = worldSpaceCentroid - m_worldPosition[i];
                const float distanceToCentroid = length(toCentroid);
                if (distanceToCentroid > 0.0f) {
                    dirToCentroid = toCentroid / distanceToCentroid;
                    const float rayDirLength = length(m_direction[i]);
                    if (rayDirLength > 0.0f) {
                        const Vector3 normalizedRayDir = m_direction[i] / rayDirLength;
                        const float dotProduct = clampf(dot(normalizedRayDir, dirToCentroid), -1.0f, 1.0f);
                        angleRadians = std::acos(dotProduct);
                        angleDegrees = angleRadians * (180.0f / kPi);
                    } else {
                        logOnce(LogSeverity::Warning, "AngleToMesh: Direction vector has zero length.");
                    }
                } else {
                    logOnce(LogSeverity::Warning, "AngleToMesh: World position is at the centroid.");
                }
            } else {
                logOnce(LogSeverity::Error, "AngleToMesh: Bounding box is invalid.");
            }
        } else {
            logOnce(LogSeverity::Error, "AngleToMesh: resolvePrimTarget failed is null.");
        }
        m_angleDegrees[i] = angleDegrees;
        m_angleRadians[i] = angleRadians;
        m_directionToCentroid[i] = dirToCentroid;
    }
}

// ---- Transforms -----------------------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Prim, kInvalidPrimTarget, target, "Target", "The mesh or light prim to read the transform from.",             \
      property.allowedPrimTypes = {PrimType::UsdGeomMesh, PrimType::UsdLuxSphereLight, PrimType::UsdLuxCylinderLight,   \
                                   PrimType::UsdLuxDiskLight, PrimType::UsdLuxDistantLight, PrimType::UsdLuxRectLight})
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), position, "Position", "The world space position of the target.")           \
    X(PT::Float4, Vector4(0.0f, 0.0f, 0.0f, 1.0f), rotation, "Rotation",                                                \
      "The world space rotation of the target as a quaternion (x, y, z, w).")                                           \
    X(PT::Float3, Vector3(1.0f, 1.0f, 1.0f), scale, "Scale", "The world space scale of the target.")
FUSE_LOGIC_COMPONENT(ReadTransform, "Read Transform", "Sense",
                     "Reads the transform (position, rotation, scale) of a mesh or light in world space.\n\nExtracts the "
                     "transform information from a given mesh or light prim. Outputs position, rotation (as quaternion), and "
                     "scale in world space.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void ReadTransform::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        Vector3 position(0.0f, 0.0f, 0.0f);
        Vector4 rotation(0.0f, 0.0f, 0.0f, 1.0f);
        Vector3 scale(1.0f, 1.0f, 1.0f);
        if (const PrimSnapshot* prim = m_batch.resolvePrimTarget(ctx, i, m_target[i])) {
            if (prim->kind == PrimSnapshot::Kind::Mesh) {
                decomposeMatrix(prim->objectToWorld, position, rotation, scale);
            } else if (prim->kind == PrimSnapshot::Kind::Light) {
                position = prim->lightPosition;
            }
        }
        m_position[i] = position;
        m_rotation[i] = rotation;
        m_scale[i] = scale;
    }
}

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Prim, kInvalidPrimTarget, target, "Target", "The mesh prim to read the bone transform from. Must be a skinned mesh.", \
      property.allowedPrimTypes = {PrimType::UsdGeomMesh})                                                              \
    X(PT::Float, 0.0f, boneIndex, "Bone Index", "The index of the bone to read. Will be rounded to the nearest integer.", \
      property.hardMin = 0.0f, property.softMax = 256.0f, property.uiStep = 1.0f)
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), position, "Position", "The world space position of the bone.")             \
    X(PT::Float4, Vector4(0.0f, 0.0f, 0.0f, 1.0f), rotation, "Rotation",                                                \
      "The world space rotation of the bone as a quaternion (x, y, z, w).")                                             \
    X(PT::Float3, Vector3(1.0f, 1.0f, 1.0f), scale, "Scale", "The world space scale of the bone.")
FUSE_LOGIC_COMPONENT(ReadBoneTransform, "Read Bone Transform", "Sense",
                     "Reads the transform (position, rotation, scale) of a bone from a skinned mesh.\n\nExtracts the transform "
                     "information for a specific bone from a skinned mesh prim. Outputs position, rotation (as quaternion), and "
                     "scale in world space. Returns identity transform if the target is not a skinned mesh or the bone index is "
                     "invalid.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void ReadBoneTransform::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        Vector3 position(0.0f, 0.0f, 0.0f);
        Vector4 rotation(0.0f, 0.0f, 0.0f, 1.0f);
        Vector3 scale(1.0f, 1.0f, 1.0f);
        const PrimSnapshot* prim = m_batch.resolvePrimTarget(ctx, i, m_target[i]);
        if (prim != nullptr && prim->kind == PrimSnapshot::Kind::Mesh) {
            const auto boneIdx = static_cast<std::uint32_t>(std::round(m_boneIndex[i]));
            const std::size_t numBones = prim->boneMatrices.size();
            if (numBones > 0 && boneIdx < numBones) {
                const Matrix4 worldBoneTransform = multiply(prim->objectToWorld, prim->boneMatrices[boneIdx]);
                decomposeMatrix(worldBoneTransform, position, rotation, scale);
            } else if (boneIdx >= numBones) {
                logOnce(LogSeverity::Warning, "ReadBoneTransform: Bone index " + std::to_string(boneIdx) +
                                                  " is out of range. Mesh has " + std::to_string(numBones) + " bones.");
            } else {
                logOnce(LogSeverity::Warning, "ReadBoneTransform: Target mesh is not a skinned mesh.");
            }
        }
        m_position[i] = position;
        m_rotation[i] = rotation;
        m_scale[i] = scale;
    }
}
#undef LIST_STATES

// ---- SphereLightOverride (non-functional upstream) -----------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Bool, true, enabled, "Enabled", "If true, the overrides will be applied.", property.optional = true)          \
    X(PT::Float, 0.0f, radius, "Radius", "The radius of the sphere light.", property.optional = true)                   \
    X(PT::Prim, kInvalidPrimTarget, target, "Target", "The sphere light to override.",                                  \
      property.allowedPrimTypes = {PrimType::UsdLuxSphereLight})
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)
class SphereLightOverride : public RegisteredComponentBatch<SphereLightOverride> {
    FUSE_LOGIC_GENERATE_PROP_TYPES(LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    FUSE_LOGIC_COMPONENT_BODY(SphereLightOverride, "[Non Functional] Sphere Light", "TODO",
                              "Modifies properties of a sphere light, such as its radius.\n\nNote: This component is currently "
                              "non-functional and should not be used.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS, spec.applySceneOverrides = applySceneOverrides)
    void updateRange(const LogicContext& /*ctx*/, std::size_t /*start*/, std::size_t /*end*/) final {}
    static void applySceneOverrides(const LogicContext& ctx, ComponentBatch& batch, std::size_t start, std::size_t end) {
        auto& self = static_cast<SphereLightOverride&>(batch);
        for (std::size_t i = start; i < end; i++) {
            if (!self.m_enabled[i]) {
                continue;
            }
            const PrimSnapshot* prim = self.m_batch.resolvePrimTarget(ctx, i, self.m_target[i]);
            if (prim == nullptr || prim->kind != PrimSnapshot::Kind::Light) {
                logOnce(LogSeverity::Error, "SphereLightOverride: target prim was invalid (not a sphere light, or not part of "
                                            "the same replacement hierarchy.)");
            }
            // As upstream: lights expose no writable sphere radius, so nothing is applied.
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

} // namespace

void registerSenseComponents() {
    Camera::registerType();
    KeyboardInput::registerType();
    MeshHashChecker::registerType();
    TextureHashChecker::registerType();
    LightHashChecker::registerType();
    FogHashChecker::registerType();
    MeshProximity::registerType();
    RayMeshIntersection::registerType();
    AngleToMesh::registerType();
    ReadTransform::registerType();
    ReadBoneTransform::registerType();
    SphereLightOverride::registerType();
}

} // namespace fuse::relight::logic::components
