// B6.3 / B6.5 / B6.6 / B6.13 gates — viewport fly/look camera, mouse-look sensitivity, BVH
// front-most picking, viewport resize (headless model), context-menu create/delete with undo,
// inspector coverage of every registered component, and same-frame visibility of inspector edits
// (transform, SDF type, GRIA alpha, mesh material id) in the viewport's scene build.
#include <fuse/core/init.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/entity_context_menu.hpp>
#include <fuse/editor/property_inspector.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/viewport_framebuffer.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/editor/viewport_scene_view.hpp>
#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/sdf_csg.hpp>

#if defined(FUSE_EDITOR_HAS_RHI)
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#include <fuse/renderer/resource_manager.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using fuse::f32;
using fuse::u32;
using fuse::ecs::EntityID;
using fuse::ecs::vec3;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool near(f32 a, f32 b, f32 eps) {
    return std::fabs(a - b) <= eps;
}

bool nearVec(const vec3& a, const vec3& b, f32 eps) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

vec3 v(f32 x, f32 y, f32 z) {
    return {x, y, z, 0.f};
}

vec3 normalized(const vec3& a) {
    const f32 len = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return {a.x / len, a.y / len, a.z / len, 0.f};
}

EntityID addSdfSphere(fuse::ecs::Registry& reg, const vec3& position, f32 radius) {
    const EntityID id = reg.create();
    fuse::ecs::Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    reg.add(id, t);
    fuse::ecs::SDFObject sdf{};
    sdf.type = fuse::ecs::SDFPrimitive::Sphere;
    sdf.params = {radius, 0.f, 0.f, 0.f};
    reg.add(id, sdf);
    return id;
}

EntityID addMeshBox(fuse::ecs::Registry& reg, const vec3& position, const vec3& half, const fuse::ecs::quat& rot = {0.f, 0.f, 0.f, 1.f}) {
    const EntityID id = reg.create();
    fuse::ecs::Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    t.rotation = rot;
    reg.add(id, t);
    fuse::ecs::Mesh mesh{};
    mesh.aabb_min = {-half.x, -half.y, -half.z, 0.f};
    mesh.aabb_max = {half.x, half.y, half.z, 0.f};
    reg.add(id, mesh);
    return id;
}

/// Camera at the origin looking down +Z (yaw 0, pitch 0).
void placeCameraAtOrigin(fuse::editor::ViewportPanel& viewport) {
    viewport.setDimensions(1280, 720);
    viewport.camera().positionX = 0.f;
    viewport.camera().positionY = 0.f;
    viewport.camera().positionZ = 0.f;
    viewport.camera().yaw = 0.f;
    viewport.camera().pitch = 0.f;
}

// ---------------------------------------------------------------------------------------------
// Row 4762: B6.3 viewport on FUSE APIs — fly / look camera logic.
void testFlyLookCamera() {
    fuse::editor::ViewportPanel viewport;
    placeCameraAtOrigin(viewport);
    viewport.camera().moveSpeed = 10.f;

    expectTrue(nearVec(viewport.forward(), v(0.f, 0.f, 1.f), 1e-6f), "yaw 0 / pitch 0 looks down +Z");
    // right = forward x up, as ecs::look_at builds its side axis (right-handed).
    expectTrue(nearVec(viewport.right(), v(-1.f, 0.f, 0.f), 1e-6f), "right axis matches look_at side");
    expectTrue(nearVec(viewport.up(), v(0.f, 1.f, 0.f), 1e-6f), "camera up is world up at pitch 0");

    // View matrix agrees with the basis: forward maps to view -Z, right to view +X.
    const fuse::ecs::mat4 view = viewport.viewMatrix();
    const vec3 ahead = fuse::ecs::transform_point(view, {0.f, 0.f, 5.f, 1.f});
    const vec3 side = fuse::ecs::transform_point(view, {-2.f, 0.f, 0.f, 1.f});
    expectTrue(near(ahead.z, -5.f, 1e-5f) && near(ahead.x, 0.f, 1e-5f), "forward is view -Z");
    expectTrue(near(side.x, 2.f, 1e-5f), "right is view +X");

    // Mouse travel without the look button does nothing (no accidental orbit while clicking).
    fuse::editor::ViewportInput idle{};
    idle.mouseDeltaX = 250.f;
    idle.moveForward = true;
    viewport.submitInput(idle);
    viewport.tick(1.f);
    expectTrue(viewport.camera().yaw == 0.f && viewport.camera().positionZ == 0.f && !viewport.camera().isFlying,
               "no look / move while the look button is released");

    // Fly forward 1 s at 10 u/s.
    fuse::editor::ViewportInput fly{};
    fly.lookHeld = true;
    fly.moveForward = true;
    viewport.submitInput(fly);
    viewport.tick(1.f);
    expectTrue(viewport.camera().isFlying, "look button enters fly mode");
    expectTrue(nearVec(viewport.position(), v(0.f, 0.f, 10.f), 1e-4f), "W moves moveSpeed units/s along forward");

    // Diagonal (W + D) is normalised: same speed, direction forward + right.
    fly.moveRight = true;
    viewport.submitInput(fly);
    viewport.tick(0.5f);
    const f32 d = 5.f / std::sqrt(2.f);
    expectTrue(nearVec(viewport.position(), v(-d, 0.f, 10.f + d), 1e-4f), "diagonal movement is not faster");

    // Fast modifier and vertical movement.
    fuse::editor::ViewportInput up{};
    up.lookHeld = true;
    up.moveUp = true;
    up.fast = true;
    viewport.submitInput(up);
    viewport.tick(0.25f);
    expectTrue(near(viewport.position().y, 10.f, 1e-4f), "E + Shift rises at moveSpeed x fastMultiplier");

    // Turning 90 degrees right: forward becomes the old right axis; W now moves along it.
    fuse::editor::ViewportPanel turned;
    placeCameraAtOrigin(turned);
    fuse::editor::ViewportInput look{};
    look.lookHeld = true;
    look.mouseDeltaX = 90.f / turned.camera().lookSensitivity;
    turned.submitInput(look);
    turned.tick(0.f);
    expectTrue(nearVec(turned.forward(), v(-1.f, 0.f, 0.f), 1e-5f), "yaw +90 turns toward the old right axis");

    // Pitch is clamped short of the pole so the basis never degenerates.
    look.mouseDeltaX = 0.f;
    look.mouseDeltaY = -10000.f;
    turned.submitInput(look);
    turned.tick(0.f);
    expectTrue(turned.camera().pitch == fuse::editor::ViewportCamera::kMaxPitchDeg, "pitch clamps at +89");
    expectTrue(std::isfinite(turned.right().x) && near(std::fabs(turned.right().x) + std::fabs(turned.right().z), 1.f, 1e-3f),
               "right axis stays well defined at the pitch clamp");

    // Pixel rays and projection agree with the view-projection matrix the renderer uses.
    fuse::editor::ViewportPanel cam;
    placeCameraAtOrigin(cam);
    cam.camera().yaw = 30.f;
    cam.camera().pitch = -15.f;
    const fuse::editor::ViewportRay centre = cam.screenRay(640.f, 360.f);
    expectTrue(nearVec(centre.direction, cam.forward(), 1e-5f), "centre pixel ray is the forward axis");
    const vec3 world{3.f, -1.f, 8.f, 1.f};
    f32 px = 0.f;
    f32 py = 0.f;
    expectTrue(cam.worldToScreen(world, px, py), "point in front projects");
    const fuse::ecs::mat4 vp = cam.viewProjection();
    const f32* m = vp.data.data();
    const f32 cx = m[0] * world.x + m[4] * world.y + m[8] * world.z + m[12];
    const f32 cy = m[1] * world.x + m[5] * world.y + m[9] * world.z + m[13];
    const f32 cw = m[3] * world.x + m[7] * world.y + m[11] * world.z + m[15];
    const f32 mx = (cx / cw + 1.f) * 0.5f * 1280.f;
    const f32 my = (1.f - cy / cw) * 0.5f * 720.f;
    expectTrue(near(px, mx, 1e-2f) && near(py, my, 1e-2f), "worldToScreen matches perspective * look_at");
    const fuse::editor::ViewportRay back = cam.screenRay(px, py);
    const vec3 toWorld = normalized(v(world.x - back.origin.x, world.y - back.origin.y, world.z - back.origin.z));
    expectTrue(nearVec(back.direction, toWorld, 1e-4f), "screenRay inverts worldToScreen");
}

// ---------------------------------------------------------------------------------------------
// Row 5284: mouse look angular rate matches the sensitivity setting exactly.
void testMouseLookSensitivity() {
    for (const f32 sensitivity : {0.05f, 0.2f, 0.75f}) {
        fuse::editor::ViewportPanel viewport;
        placeCameraAtOrigin(viewport);
        viewport.camera().lookSensitivity = sensitivity;

        fuse::editor::ViewportInput look{};
        look.lookHeld = true;
        look.mouseDeltaX = 100.f;
        look.mouseDeltaY = -40.f;
        viewport.submitInput(look);
        viewport.tick(1.f / 60.f);
        expectTrue(viewport.camera().yaw == 100.f * sensitivity, "yaw delta == pixels x sensitivity (exact)");
        expectTrue(viewport.camera().pitch == 40.f * sensitivity, "pitch delta == pixels x sensitivity (exact)");

        // Rate: 5 px/frame at 60 Hz for 1 s == 300 px/s -> 300 * sensitivity deg/s.
        fuse::editor::ViewportPanel rate;
        placeCameraAtOrigin(rate);
        rate.camera().lookSensitivity = sensitivity;
        look.mouseDeltaY = 0.f;
        look.mouseDeltaX = 5.f;
        for (int frame = 0; frame < 60; ++frame) {
            rate.submitInput(look);
            rate.tick(1.f / 60.f);
        }
        expectTrue(near(rate.camera().yaw, 300.f * sensitivity - (300.f * sensitivity > 180.f ? 360.f : 0.f), 1e-3f),
                   "angular rate == mouse speed x sensitivity");

        // Frame-rate independence: the same travel split over more frames / several input events
        // in one frame gives the same angle (look is per pixel, not per second).
        fuse::editor::ViewportPanel split;
        placeCameraAtOrigin(split);
        split.camera().lookSensitivity = sensitivity;
        look.mouseDeltaX = 25.f;
        for (int event = 0; event < 4; ++event) {
            split.submitInput(look); // four events before one tick accumulate
        }
        split.tick(1.f / 30.f);
        expectTrue(split.camera().yaw == viewport.camera().yaw, "accumulated deltas give identical angle");
    }
}

// ---------------------------------------------------------------------------------------------
// Row 5286: picking finds the front-most entity, including with overlapping bounds.
void testPickFrontMost() {
    fuse::editor::EditorScene scene;
    scene.init();
    fuse::ecs::Registry& reg = scene.registry();
    fuse::editor::ViewportPanel viewport;
    placeCameraAtOrigin(viewport);
    fuse::editor::ViewportSceneView view;

    // (a) Nested / overlapping objects on the centre ray: a small SDF sphere inside the bounds of a
    // big mesh box, and a big SDF sphere behind; the nearest *surface* wins in any creation order.
    const EntityID bigBox = addMeshBox(reg, v(0.f, 0.f, 12.f), v(3.f, 3.f, 3.f));   // surface z = 9
    const EntityID small = addSdfSphere(reg, v(0.f, 0.f, 8.f), 0.5f);              // surface z = 7.5
    const EntityID bigSphere = addSdfSphere(reg, v(0.f, 0.f, 14.f), 5.f);          // surface z = 9
    (void)bigBox;
    (void)bigSphere;
    fuse::editor::ViewportPickResult pick = view.pick(scene, viewport, 640.f, 360.f);
    expectTrue(pick.hit && pick.entity == small, "centre pick returns the front-most (small) sphere");
    expectTrue(near(pick.distance, 7.5f, 1e-3f), "pick distance is the surface, not the bounds");

    // (b) Bounds overlap trap: the ray enters a big sphere's AABB first but misses the sphere and
    // hits a mesh behind it. A bounds-only BVH ray cast reports the sphere; the pick must not.
    fuse::editor::EditorScene trap;
    trap.init();
    const EntityID decoy = addSdfSphere(trap.registry(), v(0.f, 0.f, 10.f), 2.f);
    const EntityID behind = addMeshBox(trap.registry(), v(2.5f, 2.5f, 14.f), v(1.f, 1.f, 1.f));
    fuse::editor::ViewportSceneView trapView;
    fuse::editor::ViewportRay ray{};
    ray.origin = {0.f, 0.f, 0.f, 1.f};
    ray.direction = normalized(v(0.18f, 0.18f, 1.f));
    pick = trapView.pickRay(trap, ray);
    fuse::spatial::BVHLeaf naiveLeaf{};
    f32 naiveT = 0.f;
    const bool naiveHit = trapView.bvh().ray_cast(ray.origin, ray.direction, 1000.f, naiveLeaf, naiveT);
    expectTrue(naiveHit && naiveLeaf.entity == decoy, "bounds-only cast is fooled by the overlapping AABB");
    expectTrue(pick.hit && pick.entity == behind, "exact pick skips the missed sphere and finds the mesh behind");

    // Same through the screen path: aim at the mesh's visible face from the viewport.
    fuse::editor::ViewportPanel trapCam;
    placeCameraAtOrigin(trapCam);
    f32 px = 0.f;
    f32 py = 0.f;
    expectTrue(trapCam.worldToScreen({1.8f * 13.f / 10.f, 1.8f * 13.f / 10.f, 13.f, 1.f}, px, py), "target on screen");
    pick = trapView.pick(trap, trapCam, px, py);
    expectTrue(pick.hit && pick.entity == behind, "pixel pick through overlapping bounds hits the mesh");

    // (c) Rotated mesh: the world AABB of a 45-degree box covers a corner the box does not.
    fuse::editor::EditorScene rotated;
    rotated.init();
    const f32 s = std::sin(3.14159265f / 8.f);
    const f32 c = std::cos(3.14159265f / 8.f);
    const EntityID diamond = addMeshBox(rotated.registry(), v(0.f, 0.f, 5.f), v(1.f, 1.f, 1.f), {0.f, s, 0.f, c});
    const EntityID wall = addMeshBox(rotated.registry(), v(0.f, 0.f, 20.f), v(20.f, 20.f, 0.5f));
    fuse::editor::ViewportSceneView rotatedView;
    ray.direction = normalized(v(0.351f, 0.f, 1.f));
    pick = rotatedView.pickRay(rotated, ray);
    expectTrue(pick.hit && pick.entity == wall, "ray through the rotated box's empty AABB corner hits the wall");
    ray.direction = v(0.f, 0.f, 1.f);
    pick = rotatedView.pickRay(rotated, ray);
    expectTrue(pick.hit && pick.entity == diamond && near(pick.distance, 5.f - std::sqrt(2.f), 1e-3f),
               "centre ray hits the rotated box's front edge");

    // (d) Click-select, miss clears, hidden objects are not pickable, edits are seen immediately.
    fuse::editor::EditorState state;
    pick = view.pickAndSelect(scene, viewport, 640.f, 360.f, state);
    expectTrue(state.primarySelection == small && state.selectedEntities.size() == 1u, "click selects the hit");
    view.pickAndSelect(scene, viewport, 5.f, 5.f, state);
    expectTrue(!state.primarySelection.valid() && state.selectedEntities.empty(), "click on empty space clears");
    reg.get<fuse::ecs::SDFObject>(small)->visible = false;
    pick = view.pick(scene, viewport, 640.f, 360.f);
    expectTrue(pick.hit && pick.entity != small && near(pick.distance, 9.f, 1e-3f), "hidden objects are not picked");
    reg.get<fuse::ecs::SDFObject>(small)->visible = true;
    fuse::ecs::Transform* moved = reg.get<fuse::ecs::Transform>(small);
    moved->position = {0.f, 0.f, 2.f, 1.f};
    moved->dirty = true;
    pick = view.pick(scene, viewport, 640.f, 360.f);
    expectTrue(pick.hit && pick.entity == small && near(pick.distance, 1.5f, 1e-3f), "pick follows a moved entity at once");
}

// ---------------------------------------------------------------------------------------------
// Row 5287 (headless model): resize rebuilds the framebuffer and the camera projection.
void testViewportResizeHeadless() {
    fuse::editor::ViewportPanel viewport;
    fuse::editor::ViewportFramebuffer framebuffer;
    viewport.setDimensions(1280, 720);
    expectTrue(viewport.needsResize(), "resize raises the rebuild flag");
    expectTrue(framebuffer.sync(viewport) && framebuffer.width() == 1280u && framebuffer.height() == 720u,
               "framebuffer sized to the panel");
    expectTrue(!viewport.needsResize() && framebuffer.builtGeneration() == viewport.resizeGeneration(),
               "sync clears the flag for the generation it built");
    const u32 rebuilds = framebuffer.rebuildCount();
    viewport.setDimensions(1280, 720);
    expectTrue(!viewport.needsResize() && framebuffer.sync(viewport) && framebuffer.rebuildCount() == rebuilds,
               "same-size resize is a no-op");
    viewport.setDimensions(0, 0);
    expectTrue(viewport.width() == 1u && viewport.height() == 1u, "zero size clamps to 1x1 (minimised)");
    framebuffer.sync(viewport);
    viewport.setDimensions(720, 1280);
    expectTrue(framebuffer.sync(viewport) && framebuffer.width() == 720u && framebuffer.height() == 1280u &&
                   framebuffer.rebuildCount() == rebuilds + 2u,
               "portrait resize rebuilds again");

    // Aspect follows the new size: a world point on the right edge at 16:9 is off-screen at 9:16.
    placeCameraAtOrigin(viewport); // back to 1280x720
    framebuffer.sync(viewport);
    const fuse::editor::ViewportRay edge = viewport.screenRay(1280.f, 360.f);
    const vec3 edgePoint{edge.direction.x * 10.f, edge.direction.y * 10.f, edge.direction.z * 10.f, 1.f};
    viewport.setDimensions(720, 1280);
    framebuffer.sync(viewport);
    f32 px = 0.f;
    f32 py = 0.f;
    expectTrue(viewport.worldToScreen(edgePoint, px, py) && px > 720.f, "projection aspect follows the resize");
    expectTrue(near(viewport.aspect(), 720.f / 1280.f, 1e-6f), "aspect == width / height");
}

// ---------------------------------------------------------------------------------------------
// Row 5297: right-click context menu creates / deletes entities, one undo step each.
void testContextMenu() {
    fuse::editor::EditorScene scene;
    scene.init();
    fuse::ecs::Registry& reg = scene.registry();
    fuse::editor::EditorState state;
    fuse::editor::UndoStack undo;
    fuse::editor::ViewportPanel viewport;
    placeCameraAtOrigin(viewport);
    fuse::editor::ViewportSceneView view;
    fuse::editor::EntityContextMenu menu;

    // Right-click on empty space: create items enabled, Delete / Duplicate disabled.
    menu.openInViewport(scene, view, viewport, 640.f, 360.f, state);
    expectTrue(menu.isOpen() && !menu.clickedEntity().valid(), "menu opens over empty space");
    expectTrue(menu.find(fuse::editor::ContextMenuAction::Delete) != nullptr &&
                   !menu.find(fuse::editor::ContextMenuAction::Delete)->enabled,
               "Delete disabled without a selection");
    expectTrue(!menu.activate(fuse::editor::ContextMenuAction::Delete, scene, state, undo), "disabled item is inert");
    expectTrue(nearVec(menu.spawnPoint(), v(0.f, 0.f, fuse::editor::EntityContextMenu::kSpawnDistance), 1e-4f),
               "empty-space spawn is in front of the camera");

    struct Expect {
        fuse::editor::ContextMenuAction action;
        bool mesh;
        bool sdf;
        bool point;
        bool dir;
        bool camera;
    };
    const Expect creates[] = {
        {fuse::editor::ContextMenuAction::CreateEmpty, false, false, false, false, false},
        {fuse::editor::ContextMenuAction::CreateCube, true, false, false, false, false},
        {fuse::editor::ContextMenuAction::CreateSdfSphere, false, true, false, false, false},
        {fuse::editor::ContextMenuAction::CreateSdfBox, false, true, false, false, false},
        {fuse::editor::ContextMenuAction::CreatePointLight, false, false, true, false, false},
        {fuse::editor::ContextMenuAction::CreateDirectionalLight, false, false, false, true, false},
        {fuse::editor::ContextMenuAction::CreateCamera, false, false, false, false, true},
    };
    std::vector<EntityID> created;
    for (const Expect& e : creates) {
        menu.openInViewport(scene, view, viewport, 100.f, 100.f, state);
        const std::size_t before = reg.count();
        expectTrue(menu.activate(e.action, scene, state, undo), "create action runs");
        const EntityID id = menu.lastCreated();
        expectTrue(reg.alive(id) && reg.count() == before + 1u, "create adds exactly one entity");
        expectTrue(reg.has<fuse::ecs::Transform>(id), "created entity has a Transform");
        expectTrue(reg.has<fuse::ecs::Mesh>(id) == e.mesh && reg.has<fuse::ecs::SDFObject>(id) == e.sdf &&
                       reg.has<fuse::ecs::PointLight>(id) == e.point &&
                       reg.has<fuse::ecs::DirectionalLight>(id) == e.dir && reg.has<fuse::ecs::Camera>(id) == e.camera,
                   "created entity carries the menu item's components");
        expectTrue(state.primarySelection == id, "new entity becomes the selection");
        created.push_back(id);
    }
    expectTrue(undo.undoCount() == 7u, "each create is one undo step");

    // Right-click on the SDF sphere spawns at its surface and selects it.
    const EntityID sphere = addSdfSphere(reg, v(0.f, 0.f, 20.f), 1.f);
    menu.openInViewport(scene, view, viewport, 640.f, 360.f, state);
    expectTrue(menu.clickedEntity() == sphere && state.primarySelection == sphere, "right-click selects the hit");
    expectTrue(nearVec(menu.spawnPoint(), v(0.f, 0.f, 19.f), 1e-3f), "spawn point is the clicked surface");
    expectTrue(menu.find(fuse::editor::ContextMenuAction::Delete)->enabled, "Delete enabled with a selection");

    // Delete (with a transform child that must be re-linked on undo).
    const EntityID child = reg.create();
    fuse::ecs::Transform childT{};
    childT.parent = sphere;
    reg.add(child, childT);
    fuse::ecs::SDFObject edited = *reg.get<fuse::ecs::SDFObject>(sphere);
    edited.blend_alpha = 0.125f;
    edited.material_id = 9u;
    *reg.get<fuse::ecs::SDFObject>(sphere) = edited;
    const std::size_t beforeDelete = reg.count();
    expectTrue(menu.activate(fuse::editor::ContextMenuAction::Delete, scene, state, undo), "Delete runs");
    expectTrue(!reg.alive(sphere) && reg.count() == beforeDelete - 1u && !state.primarySelection.valid(),
               "Delete removes the entity and clears the selection");
    expectTrue(!reg.get<fuse::ecs::Transform>(child)->parent.valid(), "child orphaned on delete");
    undo.undo();
    expectTrue(reg.alive(sphere), "undo delete revives the same entity id");
    const fuse::ecs::SDFObject* restored = reg.get<fuse::ecs::SDFObject>(sphere);
    expectTrue(restored != nullptr && restored->blend_alpha == 0.125f && restored->material_id == 9u,
               "undo restores the components exactly");
    expectTrue(reg.get<fuse::ecs::Transform>(child)->parent == sphere, "undo re-links the child");
    undo.redo();
    expectTrue(!reg.alive(sphere), "redo deletes again");
    undo.undo();

    // Multi-selection delete is a single undo step.
    state.selectedEntities = {created[1], created[2], created[3]};
    state.primarySelection = created[3];
    menu.openAt(created[3], v(0.f, 0.f, 0.f), state);
    const u32 stepsBefore = undo.undoCount();
    expectTrue(menu.activate(fuse::editor::ContextMenuAction::Delete, scene, state, undo), "multi delete runs");
    expectTrue(!reg.alive(created[1]) && !reg.alive(created[2]) && !reg.alive(created[3]) &&
                   undo.undoCount() == stepsBefore + 1u,
               "multi delete removes every selected entity in one step");
    undo.undo();
    expectTrue(reg.alive(created[1]) && reg.alive(created[2]) && reg.alive(created[3]), "one undo restores all");

    // Duplicate copies components; undoing creates removes them; redo revives the same ids.
    state.selectedEntities = {created[2]};
    state.primarySelection = created[2];
    menu.openAt(created[2], v(0.f, 0.f, 0.f), state);
    expectTrue(menu.activate(fuse::editor::ContextMenuAction::Duplicate, scene, state, undo), "duplicate runs");
    const EntityID copy = menu.lastCreated();
    expectTrue(copy != created[2] && reg.has<fuse::ecs::SDFObject>(copy) &&
                   reg.get<fuse::ecs::SDFObject>(copy)->type == reg.get<fuse::ecs::SDFObject>(created[2])->type,
               "duplicate copies the components");
    while (undo.canUndo()) {
        undo.undo();
    }
    std::size_t aliveCreated = 0;
    for (const EntityID id : created) {
        aliveCreated += reg.alive(id) ? 1u : 0u;
    }
    expectTrue(aliveCreated == 0u && !reg.alive(copy), "undoing all creates removes every created entity");
    undo.redo();
    expectTrue(reg.alive(created[0]), "redo create revives the same entity id");
}

// ---------------------------------------------------------------------------------------------
// Row 5298: every registered component type renders in the inspector.
struct ModuleComponent {
    static constexpr const char* component_name = "GateModuleComponent";
    u32 a = 0xA1B2C3D4u;
    f32 b = 2.5f;
};

template <typename T>
void addDefault(fuse::ecs::Registry& reg, EntityID id) {
    reg.add<T>(id, T{});
}

template <typename T>
bool renderSingle(fuse::editor::EditorScene& scene) {
    fuse::ecs::Registry& reg = scene.registry();
    const EntityID id = reg.create();
    reg.add<T>(id, T{});
    fuse::editor::EditorState state;
    state.primarySelection = id;
    fuse::editor::PropertyInspector inspector;
    inspector.sync(state, scene);
    const bool ok = inspector.sections().size() == 1u && inspector.sections()[0].componentName == T::component_name &&
                    inspector.sections()[0].exposedFieldCount == inspector.sections()[0].fields.size();
    reg.destroy_entity(id);
    return ok;
}

void testInspectorAllComponents() {
    fuse::ecs::register_builtin_components();
    fuse::ecs::ComponentTypes::register_type<ModuleComponent>();

    fuse::editor::EditorScene scene;
    scene.init();
    fuse::ecs::Registry& reg = scene.registry();
    const EntityID everything = reg.create();
    addDefault<fuse::ecs::Transform>(reg, everything);
    fuse::ecs::Mesh mesh{};
    mesh.material_id = 42u;
    reg.add(everything, mesh);
    addDefault<fuse::ecs::RigidBody>(reg, everything);
    fuse::ecs::SDFObject sdf{};
    sdf.type = fuse::ecs::SDFPrimitive::Torus;
    reg.add(everything, sdf);
    addDefault<fuse::ecs::Camera>(reg, everything);
    addDefault<fuse::ecs::DirectionalLight>(reg, everything);
    addDefault<fuse::ecs::PointLight>(reg, everything);
    addDefault<fuse::ecs::SpotLight>(reg, everything);
    addDefault<fuse::ecs::SpawnMarker>(reg, everything);
    addDefault<fuse::ecs::Collider>(reg, everything);
    addDefault<fuse::ecs::TagStatic>(reg, everything);
    addDefault<fuse::ecs::TagPlayer>(reg, everything);
    addDefault<fuse::ecs::TagKinematic>(reg, everything);
    addDefault<fuse::ecs::TagDestroy>(reg, everything);
    addDefault<ModuleComponent>(reg, everything);

    fuse::editor::EditorState state;
    state.primarySelection = everything;
    fuse::editor::PropertyInspector inspector;
    inspector.sync(state, scene);

    const std::vector<fuse::ecs::ComponentTypeInfo> registered = fuse::ecs::ComponentTypes::all();
    expectTrue(registered.size() >= 15u, "all built-in + module component types are registered");
    expectTrue(inspector.sections().size() == registered.size(), "one inspector section per registered type");
    for (const fuse::ecs::ComponentTypeInfo& info : registered) {
        const auto it = std::find_if(inspector.sections().begin(), inspector.sections().end(),
                                     [&](const auto& section) { return section.componentName == info.name; });
        const bool found = it != inspector.sections().end();
        expectTrue(found, info.name);
        if (!found) {
            continue;
        }
        const std::string_view name{info.name};
        const bool tag = name.rfind("Tag", 0) == 0;
        expectTrue(it->exposedFieldCount == it->fields.size(), "field count matches rows");
        expectTrue(tag ? it->fields.empty() : !it->fields.empty(), "non-tag sections expose fields");
        for (const auto& field : it->fields) {
            expectTrue(!field.name.empty() && !field.value.empty(), "every field row is labelled and formatted");
        }
    }
    const auto find = [&](const char* name) {
        return std::find_if(inspector.sections().begin(), inspector.sections().end(),
                            [&](const auto& section) { return section.componentName == name; });
    };
    const auto meshSection = find("Mesh");
    expectTrue(meshSection != inspector.sections().end() && meshSection->fields[0].name == "material_id" &&
                   meshSection->fields[0].value == "42",
               "typed section shows live values");
    const auto sdfSection = find("SDFObject");
    expectTrue(sdfSection != inspector.sections().end() && sdfSection->fields[0].value == "Torus", "enum field named");
    const auto module = find("GateModuleComponent");
    expectTrue(module != inspector.sections().end() && module->generic && module->fields[0].value == std::to_string(sizeof(ModuleComponent)) &&
                   module->fields[1].value.rfind("d4c3b2a1", 0) == 0,
               "module-registered type renders generically (size + raw bytes)");

    // Each built-in also renders on its own (no cross-component assumptions).
    bool allSingles = renderSingle<fuse::ecs::Transform>(scene) && renderSingle<fuse::ecs::Mesh>(scene) &&
                      renderSingle<fuse::ecs::RigidBody>(scene) && renderSingle<fuse::ecs::SDFObject>(scene) &&
                      renderSingle<fuse::ecs::Camera>(scene) && renderSingle<fuse::ecs::DirectionalLight>(scene) &&
                      renderSingle<fuse::ecs::PointLight>(scene) && renderSingle<fuse::ecs::SpotLight>(scene) &&
                      renderSingle<fuse::ecs::SpawnMarker>(scene) && renderSingle<fuse::ecs::Collider>(scene) &&
                      renderSingle<fuse::ecs::TagStatic>(scene) && renderSingle<fuse::ecs::TagPlayer>(scene) &&
                      renderSingle<fuse::ecs::TagKinematic>(scene) && renderSingle<fuse::ecs::TagDestroy>(scene) &&
                      renderSingle<ModuleComponent>(scene);
    expectTrue(allSingles, "every component renders as the only component of an entity");

    // Null raw data (defensive) does not crash.
    const auto section = fuse::editor::PropertyInspector::describeComponent("Mesh", sizeof(fuse::ecs::Mesh), nullptr);
    expectTrue(section.generic && section.fields.empty(), "missing data renders an empty generic section");
}

// ---------------------------------------------------------------------------------------------
// Rows 5299-5302: inspector edits are visible in the same frame's scene build.
const fuse::ecs::SceneSdfObject* findSdf(const fuse::ecs::SceneData& frame, EntityID id) {
    for (const auto& item : frame.sdf_objects) {
        if (item.entity == id) {
            return &item;
        }
    }
    return nullptr;
}

const fuse::ecs::DrawItem* findDraw(const fuse::ecs::SceneData& frame, EntityID id) {
    for (const auto& item : frame.draw_items) {
        if (item.entity == id) {
            return &item;
        }
    }
    return nullptr;
}

void testSameFrameEdits() {
    fuse::editor::EditorHost host;
    fuse::editor::EditorScene& scene = host.editorScene();
    fuse::ecs::Registry& reg = scene.registry();
    fuse::editor::ViewportPanel viewport;
    placeCameraAtOrigin(viewport);
    fuse::editor::ViewportSceneView view;

    const EntityID sphere = addSdfSphere(reg, v(-1.2f, 0.f, 10.f), 1.f);
    const EntityID blend = addSdfSphere(reg, v(1.2f, 0.f, 10.f), 1.f);
    fuse::ecs::SDFObject* blendSdf = reg.get<fuse::ecs::SDFObject>(blend);
    blendSdf->op = fuse::ecs::SDFCsgOp::SmoothUnion;
    blendSdf->blend_radius = 1.f;
    blendSdf->csg_order = 1u;
    const EntityID cube = addMeshBox(reg, v(0.f, 3.f, 12.f), v(0.5f, 0.5f, 0.5f));
    reg.get<fuse::ecs::Mesh>(cube)->material_id = 1u;

    view.buildFrame(scene, viewport);
    expectTrue(findSdf(view.frame(), sphere) != nullptr && findDraw(view.frame(), cube) != nullptr,
               "baseline frame contains the objects");

    fuse::editor::PropertyInspector inspector;
    host.editorState().primarySelection = sphere;
    inspector.sync(host.editorState(), scene);

    // Row 5299: Transform DragFloat -> same frame.
    const u32 frameBefore = view.frameCount();
    expectTrue(inspector.setTransformPosition(v(-1.2f, 0.5f, 9.f), scene, host.commandStack()), "position edit");
    const fuse::ecs::SceneData& f1 = view.buildFrame(scene, viewport);
    const fuse::ecs::SceneSdfObject* moved = findSdf(f1, sphere);
    expectTrue(view.frameCount() == frameBefore + 1u && moved != nullptr &&
                   near(moved->transform.data[12], -1.2f, 1e-6f) && near(moved->transform.data[13], 0.5f, 1e-6f) &&
                   near(moved->transform.data[14], 9.f, 1e-6f),
               "transform edit visible in the very next frame build (world matrix updated)");
    fuse::ecs::SdfCsgScene csg;
    csg.build(reg);
    expectTrue(near(csg.distance({-1.2f, 0.5f, 9.f, 1.f}), -1.f, 1e-4f), "CPU SDF scene sees the moved centre");
    // Drag several steps within one frame: the frame shows the latest value.
    inspector.setTransformPosition(v(-1.2f, 0.f, 9.5f), scene, host.commandStack());
    inspector.setTransformPosition(v(-1.2f, 0.f, 10.f), scene, host.commandStack());
    expectTrue(near(findSdf(view.buildFrame(scene, viewport), sphere)->transform.data[14], 10.f, 1e-6f),
               "consecutive drag values: frame shows the latest");

    // Row 5300: SDF type change -> rendered shape changes within one frame.
    const vec3 corner{-1.2f + 0.8f, 0.8f, 10.f - 0.8f, 1.f}; // inside a unit box, outside a unit sphere
    csg.build(reg);
    const f32 sphereD = csg.distance(corner);
    expectTrue(inspector.setSdfType(fuse::ecs::SDFPrimitive::Box, scene, host.commandStack()), "type edit");
    const fuse::ecs::SceneSdfObject* boxed = findSdf(view.buildFrame(scene, viewport), sphere);
    expectTrue(boxed != nullptr && boxed->type == fuse::ecs::SDFPrimitive::Box, "scene build carries the new type");
    expectTrue(boxed != nullptr && boxed->params.x == 1.f && boxed->params.y == 1.f && boxed->params.z == 1.f,
               "sphere -> box keeps the size (radius becomes the half extents)");
    csg.build(reg);
    const f32 boxD = csg.distance(corner);
    expectTrue(sphereD > 0.f && boxD < 0.f, "shape changes in the same frame (corner inside box, outside sphere)");
    host.undoPropertyEdit();
    expectTrue(reg.get<fuse::ecs::SDFObject>(sphere)->type == fuse::ecs::SDFPrimitive::Sphere &&
                   reg.get<fuse::ecs::SDFObject>(sphere)->params.y == 0.f &&
                   findSdf(view.buildFrame(scene, viewport), sphere)->type == fuse::ecs::SDFPrimitive::Sphere,
               "SDF type edit undoes (type + params) through the command stack");
    host.redoPropertyEdit();
    expectTrue(reg.get<fuse::ecs::SDFObject>(sphere)->type == fuse::ecs::SDFPrimitive::Box, "SDF type redo");
    host.undoPropertyEdit();

    // Row 5301: GRIA alpha slider -> smooth blending in the same frame, monotonic in alpha.
    host.editorState().primarySelection = blend;
    inspector.sync(host.editorState(), scene);
    const vec3 gap{0.f, 0.f, 10.f, 1.f}; // midpoint between the spheres (0.2 from each surface)
    f32 previous = 1e30f;
    f32 previousRadius = -1.f;
    bool monotonic = true;
    for (const f32 alpha : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        inspector.setSdfBlendAlpha(alpha, scene, host.commandStack());
        const fuse::ecs::SceneSdfObject* item = findSdf(view.buildFrame(scene, viewport), blend);
        csg.build(reg);
        const f32 d = csg.distance(gap);
        monotonic = monotonic && item != nullptr && item->blend_alpha == alpha &&
                    near(item->blend_radius, 1.f * alpha / 0.5f, 1e-6f) && item->blend_radius > previousRadius &&
                    d < previous + (alpha == 0.f ? 1e30f : 0.f) && (alpha == 0.f || d < previous);
        if (alpha == 0.f) {
            expectTrue(near(d, 0.2f, 1e-4f), "alpha 0 (exact) is a hard union");
        }
        if (alpha == 0.5f) {
            expectTrue(near(item->blend_radius, 1.f, 1e-6f), "default alpha applies the authored radius");
        }
        previous = d;
        previousRadius = item != nullptr ? item->blend_radius : previousRadius;
    }
    expectTrue(monotonic, "each alpha step re-blends in the same frame, more alpha = smoother (lower) gap distance");
    expectTrue(previous < 0.f, "alpha 1 fills the gap between the spheres");

    // Row 5302: mesh material id -> draw item and resolved material row change in the same frame.
    host.editorState().primarySelection = cube;
    inspector.sync(host.editorState(), scene);
    expectTrue(inspector.trySetMeshMaterialId(2u, 3u, scene, host.commandStack()), "material id edit");
    const fuse::ecs::DrawItem* draw = findDraw(view.buildFrame(scene, viewport), cube);
    expectTrue(draw != nullptr && draw->material_id == 2u, "draw item uses the new material id this frame");
    // `draw` points into the view's frame data; the next buildFrame() reallocates it (ASan UAF).
    [[maybe_unused]] const u32 drawMaterialId = draw != nullptr ? draw->material_id : 0u;
    expectTrue(!inspector.trySetMeshMaterialId(7u, 3u, scene, host.commandStack()) &&
                   findDraw(view.buildFrame(scene, viewport), cube)->material_id == 2u,
               "out-of-catalog id is rejected and the draw keeps its material");
#if defined(FUSE_EDITOR_HAS_RHI)
    fuse::renderer::ResourceManager resources; // not initialised: CPU rows only, no GPU buffer
    fuse::renderer::MaterialSystem materials;
    materials.init(resources);
    for (u32 i = 0; i < 3u; ++i) {
        fuse::renderer::Material material{};
        material.baseColor = {0.1f * static_cast<f32>(i + 1u), 0.2f, 0.3f};
        material.roughness = 0.2f + 0.3f * static_cast<f32>(i);
        materials.registerMaterial(material);
    }
    materials.flushGpuBuffer();
    const auto& row = materials.gpuMaterials()[drawMaterialId];
    const auto sample = fuse::renderer::MaterialEval::sample(row, {0.f, 3.f, 12.f});
    expectTrue(near(sample.albedo.x, 0.3f, 1e-6f) && near(sample.roughness, 0.8f, 1e-6f),
               "the draw's material row resolves to material 2's parameters");
    materials.destroy();
#endif
    host.undoPropertyEdit();
    expectTrue(findDraw(view.buildFrame(scene, viewport), cube)->material_id == 1u, "material id undo reaches the frame");
}

} // namespace

int main() {
    fuse::core::initialize();

    testFlyLookCamera();
    testMouseLookSensitivity();
    testPickFrontMost();
    testViewportResizeHeadless();
    testContextMenu();
    testInspectorAllComponents();
    testSameFrameEdits();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_viewport_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_viewport_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
