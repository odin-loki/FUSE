// GUI-2/3 headless models behind the Qt shell: TransformGizmo (layout, hit test, translate / rotate /
// scale drags, snapping) and MaterialCatalog (defaults, undoable edits, sidecar round-trip).
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/editor/editor_timeline.hpp>
#include <fuse/editor/material_catalog.hpp>
#include <fuse/world3d/world_3d.hpp>
#include <fuse/editor/transform_gizmo.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/viewport_panel.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

using namespace fuse;
using namespace fuse::editor;

ecs::Transform inFront(const ViewportPanel& panel, f32 distance) {
    ecs::Transform t;
    const ecs::vec3 p = panel.position(), f = panel.forward();
    t.position = ecs::vec3{p.x + f.x * distance, p.y + f.y * distance, p.z + f.z * distance, 1.f};
    return t;
}

TransformCommand::State dragAxis(TransformGizmo& g, const ecs::Transform& t, int axis, f32 along, f32 across) {
    const GizmoHandle2D& h = g.handles()[axis];
    const f32 dx = h.x1 - h.x0, dy = h.y1 - h.y0, len = std::hypot(dx, dy);
    const f32 ux = dx / len, uy = dy / len;
    const f32 sx = (h.x0 + h.x1) * 0.5f, sy = (h.y0 + h.y1) * 0.5f;
    g.beginDrag(axis, sx, sy, t);
    const TransformCommand::State s = g.dragTo(sx + ux * along - uy * across, sy + uy * along + ux * across);
    g.endDrag();
    return s;
}

void testGizmo() {
    ViewportPanel panel;
    panel.setDimensions(1280, 720);
    const ecs::Transform t = inFront(panel, 6.f);
    TransformGizmo g;
    expect(g.layout(panel, t), "gizmo: lays out in front of the camera");
    const GizmoHandle2D& hx = g.handles()[0];
    expect(std::fabs(std::hypot(hx.x1 - hx.x0, hx.y1 - hx.y0) - g.settings().handlePixels) < 2.f ||
               !hx.visible || std::hypot(hx.x1 - hx.x0, hx.y1 - hx.y0) < g.settings().handlePixels + 1.f,
           "gizmo: handle length ~ handlePixels");
    expect(g.hitTest((hx.x0 + hx.x1) * 0.5f, (hx.y0 + hx.y1) * 0.5f) == 0, "gizmo: hit X handle midpoint");
    expect(g.hitTest(hx.x0 + 500.f, hx.y0 + 500.f) == -1, "gizmo: miss far away");

    const TransformCommand::State moved = dragAxis(g, t, 0, 40.f, 0.f);
    expect(moved.position.x > t.position.x && std::fabs(moved.position.y - t.position.y) < 1e-5f,
           "gizmo: translate along X only");
    g.settings().snap = true;
    const TransformCommand::State snapped = dragAxis(g, t, 0, 37.f, 0.f);
    const f32 d = snapped.position.x - t.position.x;
    expect(std::fabs(d / 0.5f - std::round(d / 0.5f)) < 1e-4f, "gizmo: snapped translate on 0.5 steps");
    g.settings().snap = false;

    g.settings().tool = TransformTool::Rotate;
    g.layout(panel, t);
    const TransformCommand::State rotated = dragAxis(g, t, 1, 0.f, 40.f);
    expect(std::fabs(rotated.rotation.y) > 0.05f && std::fabs(rotated.rotation.x) < 1e-5f, "gizmo: rotate about Y");

    g.settings().tool = TransformTool::Scale;
    g.layout(panel, t);
    const TransformCommand::State scaled = dragAxis(g, t, 0, 30.f, 0.f);
    expect(scaled.scale.x > 1.f && std::fabs(scaled.scale.y - 1.f) < 1e-6f, "gizmo: scale X only (Z faces the camera)");

    g.settings().tool = TransformTool::Select;
    expect(!g.layout(panel, t), "gizmo: Select tool draws nothing");
    ecs::Transform behind = inFront(panel, -6.f);
    g.settings().tool = TransformTool::Translate;
    expect(!g.layout(panel, behind), "gizmo: hidden behind the camera");

    const ecs::quat q = TransformGizmo::axisAngle(ecs::vec3{0.f, 1.f, 0.f, 0.f}, 3.14159265f * 0.5f);
    const ecs::vec3 r = TransformGizmo::rotate(q, ecs::vec3{1.f, 0.f, 0.f, 0.f});
    expect(std::fabs(r.z + 1.f) < 1e-5f, "gizmo: quaternion rotate +X by 90° about Y -> -Z");
}

void testMaterials() {
    MaterialCatalog catalog;
    expect(catalog.count() == 5u && catalog.entry(0).name == "Default", "material: default palette");
    UndoStack undo;
    MaterialEditState s = catalog.entry(1).state;
    const f32 r0 = s.roughness;
    s.roughness = 0.9f;
    undo.execute(std::make_unique<MaterialEditCommand>(catalog, 1u, catalog.entry(1).state, s));
    s.metallic = 0.5f;
    undo.execute(std::make_unique<MaterialEditCommand>(catalog, 1u, catalog.entry(1).state, s));
    expect(undo.undoCount() == 1u, "material: consecutive edits of one id merge");
    undo.undo();
    expect(std::fabs(catalog.entry(1).state.roughness - r0) < 1e-6f, "material: undo restores");
    undo.execute(std::make_unique<AddMaterialCommand>(catalog, MaterialCatalogEntry{"Mine \"quoted\"", s}));
    expect(catalog.count() == 6u, "material: add");

    ecs::Registry reg;
    const ecs::EntityID e = reg.create();
    reg.add<ecs::SDFObject>(e, ecs::SDFObject{});
    undo.execute(std::make_unique<AssignMaterialCommand>(reg, e, 0u, 5u));
    u32 id = 0;
    expect(AssignMaterialCommand::materialOf(reg, e, id) && id == 5u, "material: assign to SDFObject");
    undo.undo();
    AssignMaterialCommand::materialOf(reg, e, id);
    expect(id == 0u, "material: undo assignment");

    const auto path = std::filesystem::temp_directory_path() / "fuse_gui2_models.materials";
    expect(catalog.save(path.string()), "material: save sidecar");
    MaterialCatalog loaded;
    expect(loaded.load(path.string()) && loaded.count() == 6u && loaded.entry(5).name == "Mine \"quoted\"" &&
               std::fabs(loaded.entry(5).state.metallic - 0.5f) < 1e-6f,
           "material: round-trip");
    std::filesystem::remove(path);
    expect(!loaded.load((path.string() + ".missing")) && loaded.count() == 6u, "material: missing file leaves catalog");
}

void testChildGizmo() {
    // Parent at (0,0,6) in front of the camera, yawed 90° about Y and scaled 2×; child at local (1,0,0).
    ViewportPanel panel;
    panel.setDimensions(1280, 720);
    ecs::Transform parent = inFront(panel, 6.f);
    parent.rotation = TransformGizmo::axisAngle(ecs::vec3{0.f, 1.f, 0.f, 0.f}, 3.14159265f * 0.5f);
    parent.scale = ecs::vec3{2.f, 2.f, 2.f, 0.f};
    ecs::TransformSystem::recompute_world_matrix(parent, ecs::mat4::identity());
    ecs::Transform child;
    child.position = ecs::vec3{1.f, 0.f, 0.f, 1.f};
    child.parent = ecs::EntityID{1, 0};
    ecs::TransformSystem::recompute_world_matrix(child, parent.local_to_world);

    TransformGizmo g;
    g.setParentFrame(parent.local_to_world, parent.world_to_local);
    expect(g.layout(panel, child), "child gizmo: lays out");
    const ecs::vec3 o = g.worldOrigin(child);
    const auto& m = child.local_to_world.data;
    expect(std::fabs(o.x - m[12]) < 1e-4f && std::fabs(o.y - m[13]) < 1e-4f && std::fabs(o.z - m[14]) < 1e-4f,
           "child gizmo: origin at the child's world position");

    // World-space translate along X: the child's world position moves along world X only.
    const TransformCommand::State moved = dragAxis(g, child, 0, 40.f, 0.f);
    ecs::Transform after = child;
    after.position = moved.position;
    ecs::TransformSystem::recompute_world_matrix(after, parent.local_to_world);
    const auto& w = after.local_to_world.data;
    expect(w[12] > m[12] + 1e-3f && std::fabs(w[13] - m[13]) < 1e-4f && std::fabs(w[14] - m[14]) < 1e-4f,
           "child gizmo: world X drag moves the child along world X (parent rotated + scaled)");

    // Local space: X handle follows parent rotation (world -Z for a +90° yaw).
    g.settings().space = TransformSpace::Local;
    g.layout(panel, child);
    const ecs::vec3 ax = g.axisDirection(0, child.rotation);
    expect(std::fabs(ax.z + 1.f) < 1e-4f, "child gizmo: local X axis follows the parent's rotation");
}

void testMaterialsToRenderer() {
    MaterialCatalog catalog;
    world3d::World3D world;
    const u32 rev = catalog.revision();
    expect(catalog.applyTo(world) == catalog.count() && world.materials().size() >= catalog.count(),
           "material->renderer: every catalog row becomes a World3D material");
    expect(std::fabs(world.materials()[1].baseColor[0] - catalog.entry(1).state.baseColorR) < 1e-6f &&
               std::fabs(world.materials()[1].roughness - catalog.entry(1).state.roughness) < 1e-6f,
           "material->renderer: colour + roughness carried over");
    MaterialEditState s = catalog.entry(1).state;
    s.metallic = 0.75f;
    catalog.setState(1, s);
    expect(catalog.revision() != rev, "material->renderer: edits bump the revision");
    MaterialCatalog fresh;
    expect(fresh.revision() != catalog.revision(), "material->renderer: a new catalog never reuses a revision");
}

void testTimeline() {
    ecs::Registry reg;
    const ecs::EntityID e = reg.create();
    ecs::Transform& t = reg.add<ecs::Transform>(e, ecs::Transform{});
    EditorTimeline tl;
    UndoStack undo;
    auto before = tl.tracks();
    expect(tl.keyTransform(reg, e, 0), "timeline: key at 0 ms");
    reg.get<ecs::Transform>(e)->position.x = 4.f;
    reg.get<ecs::Transform>(e)->rotation = EditorTimeline::fromEulerDeg(0.f, 90.f, 0.f);
    tl.keyTransform(reg, e, 2000);
    undo.execute(std::make_unique<TimelineEditCommand>(tl, before, tl.tracks(), "Key"));
    expect(tl.tracks().size() == kTimelineChannels.size() && tl.keyCount() == 18u, "timeline: 9 channels × 2 keys");
    expect(tl.apply(reg, 1000) == 1u && std::fabs(reg.get<ecs::Transform>(e)->position.x - 2.f) < 1e-4f,
           "timeline: apply samples halfway");
    const auto eu = EditorTimeline::eulerDeg(reg.get<ecs::Transform>(e)->rotation);
    expect(std::fabs(eu[1] - 45.f) < 0.5f, "timeline: rotation channel interpolates (Euler Y 45°)");

    const int tx = tl.findTrack(e.index, "position.x");
    before = tl.tracks();
    const int k = tl.moveKey(tx, 1, 4000);
    undo.execute(std::make_unique<TimelineEditCommand>(tl, before, tl.tracks(), "Move key"));
    expect(k == 1 && tl.tracks()[static_cast<size_t>(tx)].keyframes()[1].time_ms == 4000 && tl.durationMs() >= 4000,
           "timeline: move key");
    tl.apply(reg, 2000);
    expect(std::fabs(reg.get<ecs::Transform>(e)->position.x - 2.f) < 1e-4f, "timeline: moved key retimes the curve");
    undo.undo();
    expect(tl.tracks()[static_cast<size_t>(tx)].keyframes()[1].time_ms == 2000, "timeline: undo move");

    const auto path = std::filesystem::temp_directory_path() / "fuse_gui2_models.timeline";
    expect(tl.save(path.string()), "timeline: save sidecar");
    EditorTimeline loaded;
    expect(loaded.load(path.string()) && loaded.keyCount() == 18u &&
               loaded.tracks()[static_cast<size_t>(loaded.findTrack(e.index, "position.x"))].keyframes()[1].value == 4.f,
           "timeline: round-trip");
    std::filesystem::remove(path);
    before = tl.tracks();
    expect(tl.deleteKey(tx, 0), "timeline: delete key");
    undo.execute(std::make_unique<TimelineEditCommand>(tl, before, tl.tracks(), "Delete key"));
    expect(tl.keyCount() == 17u, "timeline: 17 keys left");
    undo.undo();
    undo.undo();
    expect(tl.keyCount() == 0u, "timeline: undo back to empty");
    (void)t;
}

} // namespace

int main() {
    testChildGizmo();
    testMaterialsToRenderer();
    testTimeline();
    testGizmo();
    testMaterials();
    std::printf("gui2 models: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
