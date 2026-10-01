// E20 (MP-B6-QT-INSPECTOR / UNI-U6-INSP-1) headless gates, CPU only — the command side of the
// editable Qt inspector and hierarchy drag-and-drop:
//  - component schema: every typed field of every inspectable component reads, writes and
//    round-trips its text value; bad input is rejected without touching the component
//  - SetProperty through CommandQueue -> game tick -> registry for schema fields (RigidBody mass
//    keeps inv_mass consistent, SDF type adapts params, Euler <-> quaternion), a coalesced drag is
//    one CommandStack undo step and undo restores the exact value
//  - AddComponent / RemoveComponent command kinds: UndoStack steps, undo restores removed
//    components byte-for-byte, redo re-adds, Transform is not removable, no-op cases add no step
//  - ReparentObject with kReparentKeepWorldPose keeps the world matrix (rotated + scaled parents,
//    un-parenting) as one undo step; undo restores parent and local TRS exactly; cycles rejected
//  - an AudioSource added in the editor saves to .fuselevel v3 and loads back (when audio is linked)
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/component_schema.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/property_inspector.hpp>

#include <fuse/core/temp_path.hpp>
#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>

#if defined(FUSE_WORLD3D_HAS_AUDIO)
#include <fuse/audio/audio_components.hpp>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

using fuse::f32;
using fuse::u32;
namespace ecs = fuse::ecs;
namespace editor = fuse::editor;
namespace fs = std::filesystem;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    } else {
        std::printf("ok   %s\n", message);
    }
}

fuse::Handle<fuse::Object> handleOf(ecs::EntityID id) {
    return fuse::Handle<fuse::Object>(id.index, id.generation);
}

ecs::EntityID makeEntity(ecs::Registry& registry, ecs::vec3 position = {}) {
    const ecs::EntityID id = registry.create();
    ecs::Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    registry.add(id, t);
    return id;
}

bool closeTo(f32 a, f32 b, f32 eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

bool sameMatrix(const ecs::mat4& a, const ecs::mat4& b, f32 eps) {
    for (u32 i = 0; i < 16u; ++i) {
        if (!closeTo(a.data[i], b.data[i], eps)) {
            return false;
        }
    }
    return true;
}

template <typename T>
bool sameBytes(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// ---- schema ---------------------------------------------------------------------------------------

void testSchemaRoundTrip() {
    editor::registerEditorComponentTypes();
    ecs::Registry registry;
    registry.init(64);
    const ecs::EntityID e = registry.create();

    u32 kinds = 0;
    u32 fields = 0;
    bool allRoundTrip = true;
    for (const editor::ComponentKindDesc& kind : editor::editorComponentKinds()) {
        ++kinds;
        kind.add(registry, e, nullptr);
        if (!kind.has(registry, e)) {
            std::fprintf(stderr, "  add failed: %s\n", kind.name);
            allRoundTrip = false;
            continue;
        }
        for (const editor::PropertyFieldDesc& field : kind.fields) {
            ++fields;
            std::string before;
            if (!editor::readComponentProperty(registry, e, field.propertyName, before)) {
                std::fprintf(stderr, "  read failed: %s\n", field.propertyName);
                allRoundTrip = false;
                continue;
            }
            // A value different from the default, per type.
            std::string value;
            switch (field.type) {
            case editor::PropertyFieldType::Float:
                value = "0.75";
                break;
            case editor::PropertyFieldType::Vec3:
            case editor::PropertyFieldType::Color:
                value = "0.25,0.5,2";
                break;
            case editor::PropertyFieldType::Euler:
                value = editor::formatPropertyQuat(editor::quatFromEulerDeg(10.f, 20.f, 30.f));
                break;
            case editor::PropertyFieldType::Enum:
                value = std::to_string(field.enumCount - 1u);
                break;
            case editor::PropertyFieldType::Bool:
                value = before == "1" ? "0" : "1";
                break;
            case editor::PropertyFieldType::UInt:
                value = "7";
                break;
            case editor::PropertyFieldType::String:
            case editor::PropertyFieldType::AssetPath:
                value = "scripts/e20_mover.lua";
                break;
            }
            std::string after;
            const bool wrote = editor::writeComponentProperty(registry, e, field.propertyName, value);
            const bool reread = editor::readComponentProperty(registry, e, field.propertyName, after);
            // The SDF type field reads back as the full shape text ("type;params").
            const bool matches = field.type == editor::PropertyFieldType::Enum
                                     ? std::strtoul(after.c_str(), nullptr, 10) == field.enumCount - 1u
                                     : after == value;
            // Restoring the captured text gives back the original value (undo contract).
            std::string restored;
            const bool undoOk = editor::writeComponentProperty(registry, e, field.propertyName, before) &&
                                editor::readComponentProperty(registry, e, field.propertyName, restored) &&
                                restored == before;
            if (!wrote || !reread || !matches || !undoOk) {
                std::fprintf(stderr, "  round trip failed: %s (%s -> %s -> %s)\n", field.propertyName, before.c_str(),
                             value.c_str(), after.c_str());
                allRoundTrip = false;
            }
        }
    }
    std::printf("schema: %u component kinds, %u typed fields\n", kinds, fields);
    expectTrue(kinds >= 15u && fields >= 50u, "schema: covers Transform, Mesh, SDF, RigidBody, Collider, Camera, lights, "
                                              "Script, SpawnMarker, tags (+ AudioSource)");
    expectTrue(allRoundTrip, "schema: every field writes, reads back and restores its captured text");

    // Bad input is rejected and leaves the component untouched.
    ecs::RigidBody* body = registry.get<ecs::RigidBody>(e);
    const ecs::RigidBody bodyBefore = *body;
    expectTrue(!editor::writeComponentProperty(registry, e, "RigidBody.mass", "heavy") &&
                   !editor::writeComponentProperty(registry, e, "RigidBody.mass", "-1") &&
                   !editor::writeComponentProperty(registry, e, "RigidBody.velocity", "1,2") &&
                   !editor::writeComponentProperty(registry, e, "Collider.shape", "8") &&
                   !editor::writeComponentProperty(registry, e, "Camera.is_active", "maybe") &&
                   !editor::writeComponentProperty(registry, e, "transform.rotation", "0,0,0,0") &&
                   sameBytes(*registry.get<ecs::RigidBody>(e), bodyBefore),
               "schema: malformed values rejected without side effects");
    expectTrue(editor::writeComponentProperty(registry, e, "RigidBody.mass", "4") &&
                   closeTo(registry.get<ecs::RigidBody>(e)->inv_mass, 0.25f) &&
                   editor::writeComponentProperty(registry, e, "RigidBody.mass", "0") &&
                   registry.get<ecs::RigidBody>(e)->inv_mass == 0.f,
               "schema: RigidBody mass keeps inv_mass consistent (0 = immovable)");

    // Euler <-> quaternion.
    bool eulerOk = true;
    const f32 samples[][3] = {{0.f, 0.f, 0.f}, {30.f, 45.f, 60.f}, {-90.f, 10.f, 170.f}, {12.5f, -80.f, -33.f}};
    for (const auto& s : samples) {
        const ecs::vec3 back = editor::eulerDegFromQuat(editor::quatFromEulerDeg(s[0], s[1], s[2]));
        eulerOk = eulerOk && closeTo(back.x, s[0], 1e-3f) && closeTo(back.y, s[1], 1e-3f) && closeTo(back.z, s[2], 1e-3f);
    }
    const ecs::quat yaw90 = editor::quatFromEulerDeg(0.f, 90.f, 0.f);
    eulerOk = eulerOk && closeTo(yaw90.y, std::sqrt(0.5f)) && closeTo(yaw90.w, std::sqrt(0.5f));
    expectTrue(eulerOk, "schema: XYZ Euler degrees <-> quaternion round trip");

    // SDF type edit adapts params (Sphere r=2 -> Box 2,2,2) and the captured text restores both.
    ecs::SDFObject* sdf = registry.get<ecs::SDFObject>(e);
    sdf->type = ecs::SDFPrimitive::Sphere;
    sdf->params = {2.f, 0.f, 0.f, 0.f};
    std::string shapeBefore;
    editor::readComponentProperty(registry, e, "SDFObject.type", shapeBefore);
    editor::writeComponentProperty(registry, e, "SDFObject.type", "1");
    sdf = registry.get<ecs::SDFObject>(e);
    const bool boxed = sdf->type == ecs::SDFPrimitive::Box && sdf->params.x == 2.f && sdf->params.y == 2.f &&
                       sdf->params.z == 2.f;
    editor::writeComponentProperty(registry, e, "SDFObject.type", shapeBefore);
    sdf = registry.get<ecs::SDFObject>(e);
    expectTrue(boxed && sdf->type == ecs::SDFPrimitive::Sphere && sdf->params.y == 0.f,
               "schema: SDF type edit adapts params; captured shape text restores type + params");
    registry.destroy();
}

// ---- SetProperty through the host -----------------------------------------------------------------

void testSetPropertyThroughHost() {
    editor::EditorHost host;
    host.gameTick();
    ecs::Registry& registry = host.editorScene().registry();
    const ecs::EntityID e = makeEntity(registry, {1.f, 2.f, 3.f});
    registry.add(e, ecs::RigidBody{});
    registry.add(e, ecs::PointLight{});
    const u32 depth0 = host.commandStack().undoDepth();

    // A drag: many posts of the same field -> one undo step, last value wins.
    for (int i = 1; i <= 10; ++i) {
        host.postFromUi(editor::makeSetPropertyCommand(handleOf(e), "RigidBody.mass", std::to_string(i)));
    }
    host.gameTick();
    expectTrue(registry.get<ecs::RigidBody>(e)->mass == 10.f && closeTo(registry.get<ecs::RigidBody>(e)->inv_mass, 0.1f),
               "host: SetProperty RigidBody.mass reaches the registry on the game tick");
    for (int i = 11; i <= 20; ++i) { // later ticks of the same drag
        host.postFromUi(editor::makeSetPropertyCommand(handleOf(e), "RigidBody.mass", std::to_string(i)));
        host.gameTick();
    }
    expectTrue(registry.get<ecs::RigidBody>(e)->mass == 20.f && host.commandStack().undoDepth() == depth0 + 1u,
               "host: a coalesced drag across ticks is one CommandStack undo step");
    expectTrue(host.isSceneDirty(), "host: inspector edit marks the scene dirty");
    host.undoPropertyEdit();
    expectTrue(registry.get<ecs::RigidBody>(e)->mass == 1.f && registry.get<ecs::RigidBody>(e)->inv_mass == 1.f,
               "host: undo restores the pre-drag value exactly");
    host.redoPropertyEdit();
    expectTrue(registry.get<ecs::RigidBody>(e)->mass == 20.f, "host: redo re-applies the drag result");

    // Colour + Euler rotation (quaternion payload) + a different field is a new step.
    host.postFromUi(editor::makeSetPropertyCommand(handleOf(e), "PointLight.color", "1,0.5,0.25"));
    host.postFromUi(editor::makeSetPropertyCommand(
        handleOf(e), "transform.rotation", editor::formatPropertyQuat(editor::quatFromEulerDeg(0.f, 90.f, 0.f))));
    host.gameTick();
    const ecs::PointLight* light = registry.get<ecs::PointLight>(e);
    const ecs::Transform* t = registry.get<ecs::Transform>(e);
    expectTrue(light->color.x == 1.f && light->color.y == 0.5f && light->color.z == 0.25f &&
                   closeTo(t->rotation.y, std::sqrt(0.5f)) && t->dirty,
               "host: colour and Euler rotation edits applied (Transform marked dirty for the viewport)");
    expectTrue(host.commandStack().undoDepth() == depth0 + 3u, "host: edits of different fields are separate steps");
    host.undoPropertyEdit();
    host.undoPropertyEdit();
    t = registry.get<ecs::Transform>(e);
    light = registry.get<ecs::PointLight>(e);
    expectTrue(t->rotation.w == 1.f && t->rotation.y == 0.f && light->color.y == 1.f,
               "host: undo restores rotation and colour");

    // Unknown property / component: nothing recorded.
    const u32 depth1 = host.commandStack().undoDepth();
    host.postFromUi(editor::makeSetPropertyCommand(handleOf(e), "Camera.fov_deg", "60")); // no Camera
    host.gameTick();
    expectTrue(!registry.has<ecs::Camera>(e) && host.commandStack().undoDepth() == depth1,
               "host: edit of a missing component does not create it and records no undo step");
}

// ---- Add / Remove component -----------------------------------------------------------------------

void testAddRemoveComponent() {
    editor::EditorHost host;
    host.gameTick();
    ecs::Registry& registry = host.editorScene().registry();
    const ecs::EntityID e = makeEntity(registry);
    const u32 undo0 = host.undoStack().undoCount();

    host.postFromUi(editor::makeAddComponentCommand(handleOf(e), "RigidBody"));
    host.gameTick();
    expectTrue(registry.has<ecs::RigidBody>(e) && host.undoStack().undoCount() == undo0 + 1u,
               "add: AddComponent RigidBody is one UndoStack step");
    host.postFromUi(editor::makeAddComponentCommand(handleOf(e), "RigidBody"));
    host.postFromUi(editor::makeAddComponentCommand(handleOf(e), "NoSuchComponent"));
    host.postFromUi(editor::makeRemoveComponentCommand(handleOf(e), "Transform"));
    host.postFromUi(editor::makeRemoveComponentCommand(handleOf(e), "Camera"));
    host.gameTick();
    expectTrue(host.undoStack().undoCount() == undo0 + 1u && registry.has<ecs::Transform>(e),
               "add/remove: duplicate add, unknown name, Transform removal and absent removal are no-ops");

    // Edit, then remove: undo restores the edited values byte-for-byte.
    ecs::Collider collider{};
    collider.shape = ecs::Collider::Box;
    collider.params = {1.f, 2.f, 3.f, 0.f};
    collider.friction_static = 0.9f;
    collider.is_trigger = true;
    registry.add(e, collider);
    const ecs::Collider captured = *registry.get<ecs::Collider>(e);
    host.postFromUi(editor::makeRemoveComponentCommand(handleOf(e), "Collider"));
    host.gameTick();
    expectTrue(!registry.has<ecs::Collider>(e), "remove: RemoveComponent Collider");
    host.undoStack().undo();
    expectTrue(registry.has<ecs::Collider>(e) && sameBytes(*registry.get<ecs::Collider>(e), captured),
               "remove: undo restores the component byte-for-byte");
    host.undoStack().redo();
    expectTrue(!registry.has<ecs::Collider>(e), "remove: redo removes it again");
    host.undoStack().undo(); // collider back
    host.undoStack().undo(); // rigid body add undone
    expectTrue(!registry.has<ecs::RigidBody>(e) && registry.has<ecs::Collider>(e), "add: undo removes the added component");
    registry.get<ecs::Collider>(e)->scalar = 5.f;
    host.undoStack().redo();
    expectTrue(registry.has<ecs::RigidBody>(e), "add: redo re-adds");

    // Script (E10) and lights via the same path.
    for (const char* name : {"Script", "PointLight", "SpotLight", "DirectionalLight", "Camera", "SDFObject", "Mesh",
                             "TagStatic"}) {
        host.postFromUi(editor::makeAddComponentCommand(handleOf(e), name));
    }
    host.gameTick();
    expectTrue(registry.has<ecs::Script>(e) && registry.has<ecs::PointLight>(e) && registry.has<ecs::SpotLight>(e) &&
                   registry.has<ecs::DirectionalLight>(e) && registry.has<ecs::Camera>(e) &&
                   registry.has<ecs::SDFObject>(e) && registry.has<ecs::Mesh>(e),
               "add: Script, lights, Camera, SDFObject, Mesh, tag added through commands");
    host.postFromUi(editor::makeSetPropertyCommand(handleOf(e), "Script.script_path", "scripts/mover.lua"));
    host.gameTick();
    expectTrue(registry.get<ecs::Script>(e)->path() == "scripts/mover.lua", "add: Script path edited via the asset field");
    host.undoPropertyEdit();
    expectTrue(registry.get<ecs::Script>(e)->path().empty(), "add: undo of the first script path restores the empty path");

    // Inspector sections follow add / remove.
    editor::PropertyInspector inspector;
    host.editorState().primarySelection = e;
    inspector.sync(host.editorState(), host.editorScene());
    bool sawScript = false;
    for (const auto& section : inspector.sections()) {
        sawScript = sawScript || section.componentName == "Script";
    }
    expectTrue(sawScript && inspector.sections().size() >= 9u, "add: PropertyInspector lists the added components");
}

// ---- reparent keeping world pose ------------------------------------------------------------------

void testReparentKeepsWorldPose() {
    editor::EditorHost host;
    host.gameTick();
    ecs::Registry& registry = host.editorScene().registry();
    const ecs::EntityID parent = makeEntity(registry, {10.f, 0.f, -4.f});
    const ecs::EntityID grand = makeEntity(registry, {0.f, 5.f, 0.f});
    const ecs::EntityID child = makeEntity(registry, {1.f, 2.f, 3.f});
    {
        ecs::Transform* p = registry.get<ecs::Transform>(parent);
        p->rotation = editor::quatFromEulerDeg(0.f, 90.f, 0.f);
        p->scale = {2.f, 2.f, 2.f, 0.f};
        p->parent = grand;
        ecs::Transform* g = registry.get<ecs::Transform>(grand);
        g->rotation = editor::quatFromEulerDeg(30.f, 0.f, 15.f);
        ecs::Transform* c = registry.get<ecs::Transform>(child);
        c->rotation = editor::quatFromEulerDeg(10.f, 20.f, 30.f);
        c->scale = {0.5f, 0.5f, 0.5f, 0.f};
    }
    const ecs::Transform childBefore = *registry.get<ecs::Transform>(child);
    const ecs::mat4 world0 = editor::entityWorldMatrix(registry, child);
    const u32 undo0 = host.undoStack().undoCount();

    host.postFromUi(editor::makeReparentCommand(handleOf(child), handleOf(parent)));
    host.gameTick();
    const ecs::Transform* c = registry.get<ecs::Transform>(child);
    const ecs::mat4 world1 = editor::entityWorldMatrix(registry, child);
    expectTrue(c->parent == parent, "reparent: parent set");
    expectTrue(!closeTo(c->position.x, childBefore.position.x, 1e-3f) || !closeTo(c->position.z, childBefore.position.z, 1e-3f),
               "reparent: local TRS re-expressed under the new parent");
    expectTrue(sameMatrix(world0, world1, 1e-4f), "reparent: world matrix unchanged (rotated + scaled parent chain)");
    expectTrue(host.undoStack().undoCount() == undo0 + 1u, "reparent: parent + TRS are one undo step");

    host.undoStack().undo();
    c = registry.get<ecs::Transform>(child);
    expectTrue(!c->parent.valid() && sameBytes(c->position, childBefore.position) &&
                   sameBytes(c->rotation, childBefore.rotation) && sameBytes(c->scale, childBefore.scale),
               "reparent: undo restores parent and exact local TRS");
    host.undoStack().redo();
    expectTrue(registry.get<ecs::Transform>(child)->parent == parent &&
                   sameMatrix(editor::entityWorldMatrix(registry, child), world0, 1e-4f),
               "reparent: redo");

    // Drop to the root keeps the world pose too.
    host.postFromUi(editor::makeReparentCommand(handleOf(child), fuse::Handle<fuse::Object>::invalid()));
    host.gameTick();
    expectTrue(!registry.get<ecs::Transform>(child)->parent.valid() &&
                   sameMatrix(editor::entityWorldMatrix(registry, child), world0, 1e-4f),
               "reparent: un-parent keeps world pose");

    // Cycles are refused: grand under its own grandchild.
    const u32 undo1 = host.undoStack().undoCount();
    host.postFromUi(editor::makeReparentCommand(handleOf(grand), handleOf(parent)));
    host.gameTick();
    expectTrue(!registry.get<ecs::Transform>(grand)->parent.valid() && host.undoStack().undoCount() == undo1,
               "reparent: cycle refused, no undo step");

    // Legacy flag-less reparent keeps the local TRS (existing semantics).
    const ecs::Transform local = *registry.get<ecs::Transform>(child);
    host.postFromUi(editor::makeReparentCommand(handleOf(child), handleOf(parent), false));
    host.gameTick();
    expectTrue(registry.get<ecs::Transform>(child)->parent == parent &&
                   sameBytes(registry.get<ecs::Transform>(child)->position, local.position),
               "reparent: without kReparentKeepWorldPose the local TRS is kept");
}

// ---- save / load of an inspector-added module component -------------------------------------------

void testAudioSourceSaveLoad(const fs::path& dir) {
#if defined(FUSE_WORLD3D_HAS_AUDIO)
    const std::string path = (dir / "audio.fuselevel").string();
    {
        editor::EditorHost host;
        host.gameTick();
        ecs::Registry& registry = host.editorScene().registry();
        const ecs::EntityID e = makeEntity(registry, {4.f, 5.f, 6.f});
        host.postFromUi(editor::makeAddComponentCommand(handleOf(e), "AudioSource"));
        host.gameTick();
        host.postFromUi(editor::makeSetPropertyCommand(handleOf(e), "AudioSource.volume", "0.4"));
        host.postFromUi(editor::makeSetPropertyCommand(handleOf(e), "AudioSource.bus", "2"));
        host.gameTick();
        expectTrue(registry.has<fuse::audio::AudioSource>(e) &&
                       registry.get<fuse::audio::AudioSource>(e)->desc.volume == 0.4f,
                   "audio: AudioSource added and edited through commands");
        expectTrue(host.saveSceneAs(path), "audio: scene with an AudioSource saves (type registered)");
    }
    editor::EditorHost reload;
    reload.gameTick();
    expectTrue(reload.openScene(path), "audio: scene reloads in a fresh host");
    bool found = false;
    reload.editorScene().registry().each<fuse::audio::AudioSource>(
        [&](ecs::EntityID, fuse::audio::AudioSource& source) {
            found = found || (source.desc.volume == 0.4f && source.desc.bus == fuse::audio::AudioBus::Music);
        });
    expectTrue(found, "audio: AudioSource values survive save -> load");
#else
    (void)dir;
    std::printf("skip audio: fuse_audio not linked\n");
#endif
}

} // namespace

int main() {
    const fs::path dir = fuse::test::makeUniqueTempDir("fuse_e20_inspector");
    testSchemaRoundTrip();
    testSetPropertyThroughHost();
    testAddRemoveComponent();
    testReparentKeepsWorldPose();
    testAudioSourceSaveLoad(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("e20 inspector commands: %s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
