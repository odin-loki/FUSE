// GUI-2/3 headless models behind the Qt shell: TransformGizmo (layout, hit test, translate / rotate /
// scale drags, snapping) and MaterialCatalog (defaults, undoable edits, sidecar round-trip).
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/editor/material_catalog.hpp>
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

} // namespace

int main() {
    testGizmo();
    testMaterials();
    std::printf("gui2 models: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
