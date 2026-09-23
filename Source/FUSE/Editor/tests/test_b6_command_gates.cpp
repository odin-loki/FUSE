// B6.2 / B6.6 / B6.13 gates — command system, undo/redo and inspector edit round-trips.
#include <fuse/core/init.hpp>
#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/property_inspector.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/object.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

template <typename T>
bool bytesEqual(const T& lhs, const T& rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
}

bool trsEqual(const fuse::ecs::Transform& lhs, const fuse::ecs::Transform& rhs) {
    return lhs.position.x == rhs.position.x && lhs.position.y == rhs.position.y &&
           lhs.position.z == rhs.position.z && lhs.rotation.x == rhs.rotation.x &&
           lhs.rotation.y == rhs.rotation.y && lhs.rotation.z == rhs.rotation.z &&
           lhs.rotation.w == rhs.rotation.w && lhs.scale.x == rhs.scale.x &&
           lhs.scale.y == rhs.scale.y && lhs.scale.z == rhs.scale.z;
}

using fuse::editor::TransformCommand;

TransformCommand::State makeState(float px, float py, float pz) {
    TransformCommand::State state{};
    state.position = {px, py, pz, 1.f};
    state.rotation = {0.f, 0.f, 0.f, 1.f};
    state.scale = {1.f, 1.f, 1.f, 0.f};
    return state;
}

/// Records execute/undo order into a shared log (LIFO verification).
class LogCommand final : public fuse::editor::UndoCommand {
public:
    LogCommand(std::vector<int>& log, int id) : m_log(log), m_id(id) {}
    void execute() override { m_log.push_back(m_id); }
    void undo() override { m_log.push_back(-m_id); }
    std::string description() const override { return "log " + std::to_string(m_id); }

private:
    std::vector<int>& m_log;
    int m_id = 0;
};

class TestObject final : public fuse::Object {
public:
    TestObject(std::string name, fuse::u32 index) : Object(std::move(name)) {
        setHandle(fuse::Handle<fuse::Object>(index, 1u));
    }
};

// Gate: TransformCommand undo/redo correctly restores exact before/after state.
void testTransformCommandExactRestore() {
    fuse::ecs::Registry registry;
    registry.init(64);
    const fuse::ecs::EntityID entity = registry.create();
    fuse::ecs::Transform initial{};
    initial.position = {1.23456789f, -0.000123456789f, 98765.4321f, 1.f};
    initial.rotation = {0.1826f, 0.3651f, 0.5477f, 0.7303f};
    initial.scale = {0.333333343f, 2.f, 7.77777f, 0.f};
    registry.add(entity, initial);

    TransformCommand::State after = TransformCommand::capture(initial);
    after.position = {-4.5e-7f, 3.14159274f, 1.0e6f, 1.f};
    after.rotation = {0.f, 0.70710677f, 0.f, 0.70710677f};
    after.scale = {1.5f, 1.5f, 0.1f, 0.f};

    fuse::editor::UndoStack stack;
    stack.execute(std::make_unique<TransformCommand>(registry, entity,
                                                     TransformCommand::capture(initial), after));
    const fuse::ecs::Transform* live = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(live != nullptr && bytesEqual(live->position, after.position) &&
                   bytesEqual(live->rotation, after.rotation) && bytesEqual(live->scale, after.scale),
               "TransformCommand execute applies exact after-state");

    stack.undo();
    live = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(bytesEqual(live->position, initial.position) &&
                   bytesEqual(live->rotation, initial.rotation) &&
                   bytesEqual(live->scale, initial.scale),
               "TransformCommand undo restores exact before-state (bitwise)");

    stack.redo();
    live = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(bytesEqual(live->position, after.position) && bytesEqual(live->rotation, after.rotation) &&
                   bytesEqual(live->scale, after.scale),
               "TransformCommand redo restores exact after-state (bitwise)");
    registry.destroy();
}

// Gate: 100 commands execute and fully undo in correct LIFO order — state matches pre-execution.
void testHundredCommandsLifo() {
    fuse::ecs::Registry registry;
    registry.init(64);
    std::vector<fuse::ecs::EntityID> entities;
    std::vector<fuse::ecs::Transform> initial;
    for (int i = 0; i < 3; ++i) {
        const fuse::ecs::EntityID id = registry.create();
        fuse::ecs::Transform transform{};
        transform.position = {static_cast<float>(i), 0.5f * static_cast<float>(i), -1.f, 1.f};
        registry.add(id, transform);
        entities.push_back(id);
        initial.push_back(transform);
    }

    std::vector<int> log;
    fuse::editor::UndoStack stack;
    std::vector<TransformCommand::State> current;
    for (const fuse::ecs::Transform& t : initial) {
        current.push_back(TransformCommand::capture(t));
    }

    for (int i = 0; i < 100; ++i) {
        if (i % 4 == 3) {
            stack.execute(std::make_unique<LogCommand>(log, i));
            continue;
        }
        // Round-robin entities so consecutive commands never merge.
        const std::size_t e = static_cast<std::size_t>(i) % entities.size();
        TransformCommand::State next = current[e];
        next.position.x += 0.1f * static_cast<float>(i + 1);
        next.position.z -= 0.37f;
        stack.execute(std::make_unique<TransformCommand>(registry, entities[e], current[e], next));
        current[e] = next;
    }
    expectTrue(stack.undoCount() == 100u, "100 distinct commands give 100 undo steps");

    log.clear();
    while (stack.canUndo()) {
        stack.undo();
    }
    expectTrue(stack.undoCount() == 0u && stack.redoCount() == 100u, "all 100 undone onto redo");

    bool lifo = log.size() == 25u;
    for (std::size_t k = 0; lifo && k < log.size(); ++k) {
        lifo = log[k] == -(99 - static_cast<int>(k) * 4);
    }
    expectTrue(lifo, "undo runs in LIFO order (99, 95, ..., 3)");

    bool restored = true;
    for (std::size_t e = 0; e < entities.size(); ++e) {
        restored = restored && trsEqual(*registry.get<fuse::ecs::Transform>(entities[e]), initial[e]);
    }
    expectTrue(restored, "entity state after 100 undos matches pre-execution exactly");

    while (stack.canRedo()) {
        stack.redo();
    }
    bool replayed = true;
    for (std::size_t e = 0; e < entities.size(); ++e) {
        const fuse::ecs::Transform* t = registry.get<fuse::ecs::Transform>(entities[e]);
        replayed = replayed && bytesEqual(t->position, current[e].position);
    }
    expectTrue(replayed, "redo of all 100 replays to the final state");
    registry.destroy();
}

// Gate: consecutive transform drags merge into a single undo step — verified by undo count.
void testConsecutiveDragsMerge() {
    fuse::ecs::Registry registry;
    registry.init(16);
    const fuse::ecs::EntityID entity = registry.create();
    const fuse::ecs::EntityID other = registry.create();
    registry.add(entity, fuse::ecs::Transform{});
    registry.add(other, fuse::ecs::Transform{});

    fuse::editor::UndoStack stack;
    TransformCommand::State previous = makeState(0.f, 0.f, 0.f);
    for (int frame = 1; frame <= 60; ++frame) {
        const TransformCommand::State next = makeState(0.25f * static_cast<float>(frame), 1.f, 0.f);
        stack.execute(std::make_unique<TransformCommand>(registry, entity, previous, next));
        previous = next;
    }
    expectTrue(stack.undoCount() == 1u, "60 drag frames on one entity merge into one undo step");
    expectTrue(stack.coalescedOps() == 59u, "59 merges recorded");
    expectTrue(registry.get<fuse::ecs::Transform>(entity)->position.x == 15.f,
               "merged drag leaves the latest position applied");

    stack.execute(std::make_unique<TransformCommand>(registry, other, makeState(0.f, 0.f, 0.f),
                                                     makeState(9.f, 9.f, 9.f)));
    expectTrue(stack.undoCount() == 2u, "drag of a different entity starts a new step");

    stack.undo();
    stack.undo();
    expectTrue(registry.get<fuse::ecs::Transform>(entity)->position.x == 0.f &&
                   registry.get<fuse::ecs::Transform>(entity)->position.y == 0.f,
               "undoing the merged step restores the pre-drag state");
    stack.redo();
    expectTrue(registry.get<fuse::ecs::Transform>(entity)->position.x == 15.f,
               "redo of the merged step re-applies the final drag state");
    registry.destroy();
}

// Gate: DeleteEntity undo correctly restores all components exactly.
void testDeleteEntityRestoresAllComponents() {
    fuse::ecs::Registry registry;
    registry.init(64);

    const fuse::ecs::EntityID parent = registry.create();
    registry.add(parent, fuse::ecs::Transform{});

    const fuse::ecs::EntityID victim = registry.create();
    fuse::ecs::Transform transform{};
    transform.position = {1.5f, -2.25f, 3.125f, 1.f};
    transform.rotation = {0.f, 0.38268343f, 0.f, 0.9238795f};
    transform.parent = parent;
    registry.add(victim, transform);
    fuse::ecs::Mesh mesh{};
    mesh.material_id = 17u;
    registry.add(victim, mesh);
    fuse::ecs::RigidBody body{};
    body.velocity = {1.f, 2.f, 3.f, 0.f};
    body.mass = 42.f;
    body.inv_mass = 1.f / 42.f;
    registry.add(victim, body);
    fuse::ecs::SDFObject sdf{};
    sdf.type = fuse::ecs::SDFPrimitive::Torus;
    sdf.params = {2.f, 0.5f, 0.f, 0.f};
    sdf.blend_alpha = 0.125f;
    registry.add(victim, sdf);
    fuse::ecs::PointLight light{};
    light.intensity = 3.5f;
    registry.add(victim, light);
    fuse::ecs::Collider collider{};
    registry.add(victim, collider);
    registry.add(victim, fuse::ecs::TagPlayer{});

    const fuse::ecs::EntityID child = registry.create();
    fuse::ecs::Transform childTransform{};
    childTransform.parent = victim;
    registry.add(child, childTransform);

    const fuse::editor::EntityComponentSet before =
        fuse::editor::captureEntityComponents(registry, victim);

    fuse::editor::UndoStack stack;
    auto command = std::make_unique<fuse::editor::DeleteEntityCommand>(registry, victim);
    fuse::editor::DeleteEntityCommand* deleteCommand = command.get();
    stack.execute(std::move(command));
    expectTrue(!registry.alive(victim), "delete destroys the entity");
    expectTrue(!registry.get<fuse::ecs::Transform>(child)->parent.valid(),
               "delete orphans transform children");

    stack.undo();
    const fuse::ecs::EntityID restored = deleteCommand->liveEntity();
    expectTrue(registry.alive(restored), "undo recreates the entity");
    const fuse::editor::EntityComponentSet after =
        fuse::editor::captureEntityComponents(registry, restored);

    const auto* t0 = &*std::get<std::optional<fuse::ecs::Transform>>(before);
    const auto& t1 = std::get<std::optional<fuse::ecs::Transform>>(after);
    expectTrue(t1.has_value() && bytesEqual(*t0, *t1), "Transform restored byte-for-byte (incl. parent)");
    const auto& m1 = std::get<std::optional<fuse::ecs::Mesh>>(after);
    expectTrue(m1.has_value() && bytesEqual(*std::get<std::optional<fuse::ecs::Mesh>>(before), *m1),
               "Mesh restored byte-for-byte");
    const auto& r1 = std::get<std::optional<fuse::ecs::RigidBody>>(after);
    expectTrue(r1.has_value() &&
                   bytesEqual(*std::get<std::optional<fuse::ecs::RigidBody>>(before), *r1),
               "RigidBody restored byte-for-byte");
    const auto& s1 = std::get<std::optional<fuse::ecs::SDFObject>>(after);
    expectTrue(s1.has_value() &&
                   bytesEqual(*std::get<std::optional<fuse::ecs::SDFObject>>(before), *s1),
               "SDFObject restored byte-for-byte");
    const auto& l1 = std::get<std::optional<fuse::ecs::PointLight>>(after);
    expectTrue(l1.has_value() &&
                   bytesEqual(*std::get<std::optional<fuse::ecs::PointLight>>(before), *l1),
               "PointLight restored byte-for-byte");
    expectTrue(std::get<std::optional<fuse::ecs::Collider>>(after).has_value(), "Collider restored");
    expectTrue(std::get<std::optional<fuse::ecs::TagPlayer>>(after).has_value(), "TagPlayer restored");
    expectTrue(!std::get<std::optional<fuse::ecs::Camera>>(after).has_value() &&
                   !std::get<std::optional<fuse::ecs::TagStatic>>(after).has_value(),
               "no components added that the entity never had");
    expectTrue(registry.get<fuse::ecs::Transform>(child)->parent == restored,
               "undo re-links orphaned children to the restored entity");

    stack.redo();
    expectTrue(!registry.alive(restored), "redo deletes the restored entity again");
    expectTrue(!registry.get<fuse::ecs::Transform>(child)->parent.valid(), "redo re-orphans child");

    stack.undo();
    expectTrue(registry.alive(deleteCommand->liveEntity()) &&
                   registry.get<fuse::ecs::Mesh>(deleteCommand->liveEntity())->material_id == 17u,
               "second undo restores again");
    registry.destroy();
}

// Gate: command stack respects MAX_HISTORY — oldest commands dropped correctly.
void testMaxHistoryDropsOldest() {
    fuse::ecs::Registry registry;
    registry.init(8);
    const fuse::ecs::EntityID a = registry.create();
    const fuse::ecs::EntityID b = registry.create();
    registry.add(a, fuse::ecs::Transform{});
    registry.add(b, fuse::ecs::Transform{});

    constexpr int kCommands = 300;
    const int kMax = static_cast<int>(fuse::editor::UndoStack::kMaxHistory);
    fuse::editor::UndoStack stack;
    for (int i = 0; i < kCommands; ++i) {
        const fuse::ecs::EntityID target = (i % 2 == 0) ? a : b;
        const float before = registry.get<fuse::ecs::Transform>(target)->position.x;
        stack.execute(std::make_unique<TransformCommand>(
            registry, target, makeState(before, 0.f, 0.f), makeState(static_cast<float>(i + 1), 0.f, 0.f)));
    }
    expectTrue(stack.undoCount() == fuse::editor::UndoStack::kMaxHistory, "undo depth capped at MAX_HISTORY");
    expectTrue(stack.evictedCount() == static_cast<fuse::u32>(kCommands - kMax), "44 oldest steps evicted");

    while (stack.canUndo()) {
        stack.undo();
    }
    // Oldest retained step is command index 44 (targets a); undoing everything lands on the state
    // after command 43 (a = 43, b = 44).
    expectTrue(registry.get<fuse::ecs::Transform>(a)->position.x == 43.f &&
                   registry.get<fuse::ecs::Transform>(b)->position.x == 44.f,
               "full undo stops at the oldest retained step (evicted history not replayable)");

    // N-level history: shrinking the bound trims the oldest steps immediately.
    fuse::editor::UndoStack small;
    small.setMaxHistory(10);
    std::vector<int> log;
    for (int i = 0; i < 25; ++i) {
        small.execute(std::make_unique<LogCommand>(log, i + 1));
    }
    expectTrue(small.undoCount() == 10u && small.evictedCount() == 15u, "N=10 history keeps last 10");
    small.setMaxHistory(4);
    expectTrue(small.undoCount() == 4u && small.peekUndoDescription() == "log 25",
               "lowering N trims oldest, keeps newest");
    registry.destroy();
}

// Redo invalidation: any new edit (including one that merges) drops the redo branch.
void testRedoInvalidation() {
    fuse::ecs::Registry registry;
    registry.init(8);
    const fuse::ecs::EntityID a = registry.create();
    const fuse::ecs::EntityID b = registry.create();
    registry.add(a, fuse::ecs::Transform{});
    registry.add(b, fuse::ecs::Transform{});

    fuse::editor::UndoStack stack;
    stack.execute(std::make_unique<TransformCommand>(registry, a, makeState(0, 0, 0), makeState(1, 0, 0)));
    stack.execute(std::make_unique<TransformCommand>(registry, b, makeState(0, 0, 0), makeState(2, 0, 0)));
    stack.undo();
    expectTrue(stack.redoCount() == 1u, "undo moves b's move onto redo");

    // Dragging `a` again merges into a's step — still a new edit, so b's redo must be dropped.
    stack.execute(std::make_unique<TransformCommand>(registry, a, makeState(1, 0, 0), makeState(5, 0, 0)));
    expectTrue(stack.undoCount() == 1u, "post-undo drag of a merges into a's step");
    expectTrue(stack.redoCount() == 0u, "merge invalidates the redo branch");
    stack.redo();
    expectTrue(registry.get<fuse::ecs::Transform>(b)->position.x == 0.f,
               "stale redo cannot re-apply b's move onto a diverged history");

    // Non-merging new command also clears redo.
    stack.undo();
    stack.execute(std::make_unique<TransformCommand>(registry, b, makeState(0, 0, 0), makeState(3, 0, 0)));
    expectTrue(stack.redoCount() == 0u && stack.undoCount() == 1u, "new command clears redo");

    // CommandStack (EditorCommand envelopes) has the same rule for coalesced property edits.
    fuse::editor::CommandStack envelopes;
    auto setProp = [](fuse::u32 target, const char* name, const char* value) {
        fuse::editor::EditorCommand command;
        command.kind = fuse::editor::CommandKind::SetProperty;
        command.target = fuse::Handle<fuse::Object>(target, 1u);
        command.propertyName = name;
        command.propertyValue = value;
        return command;
    };
    envelopes.execute(setProp(1u, "transform.position", "1,0,0"));
    envelopes.execute(setProp(2u, "transform.position", "2,0,0"));
    envelopes.undo();
    envelopes.execute(setProp(1u, "transform.position", "5,0,0"));
    expectTrue(envelopes.undoDepth() == 1u && envelopes.redoDepth() == 0u,
               "CommandStack coalesce after undo clears redo branch");
    registry.destroy();
}

// Macro / compound commands: one undo step, children undone in reverse order.
void testMacroCommands() {
    std::vector<int> log;
    fuse::editor::UndoStack stack;
    stack.execute(std::make_unique<LogCommand>(log, 1));

    stack.beginMacro("Group edit");
    stack.execute(std::make_unique<LogCommand>(log, 2));
    stack.beginMacro("Nested");
    stack.execute(std::make_unique<LogCommand>(log, 3));
    stack.endMacro();
    expectTrue(stack.isRecordingMacro(), "nested endMacro keeps the outer macro open");
    stack.undo();
    expectTrue(stack.undoCount() == 1u, "undo is ignored while a macro is recording");
    stack.execute(std::make_unique<LogCommand>(log, 4));
    stack.endMacro();

    expectTrue(!stack.isRecordingMacro(), "outer endMacro closes the macro");
    expectTrue(stack.undoCount() == 2u, "macro records as a single undo step");
    expectTrue(stack.peekUndoDescription() == "Group edit", "macro keeps its description");
    expectTrue((log == std::vector<int>{1, 2, 3, 4}), "macro children execute immediately in order");

    log.clear();
    stack.undo();
    expectTrue((log == std::vector<int>{-4, -3, -2}), "macro undo reverses children");
    log.clear();
    stack.redo();
    expectTrue((log == std::vector<int>{2, 3, 4}), "macro redo replays children in order");

    stack.beginMacro("Empty");
    stack.endMacro();
    expectTrue(stack.undoCount() == 2u, "empty macro records nothing");

    // Consecutive transform drags inside a macro merge within the compound.
    fuse::ecs::Registry registry;
    registry.init(4);
    const fuse::ecs::EntityID e = registry.create();
    registry.add(e, fuse::ecs::Transform{});
    stack.beginMacro("Drag");
    stack.execute(std::make_unique<TransformCommand>(registry, e, makeState(0, 0, 0), makeState(1, 0, 0)));
    stack.execute(std::make_unique<TransformCommand>(registry, e, makeState(1, 0, 0), makeState(2, 0, 0)));
    stack.endMacro();
    stack.undo();
    expectTrue(registry.get<fuse::ecs::Transform>(e)->position.x == 0.f, "macro undo restores drag start");
    registry.destroy();
}

// Saved-baseline correctness: a baseline that fell off the history or into a dropped redo branch
// must never be reported as reached again.
void testBaselineNotFalselyReached() {
    std::vector<int> log;
    fuse::editor::UndoStack stack;
    stack.execute(std::make_unique<LogCommand>(log, 1));
    stack.execute(std::make_unique<LogCommand>(log, 2));
    stack.set_baseline_state(); // saved at depth 2
    stack.undo();
    stack.execute(std::make_unique<LogCommand>(log, 3)); // depth 2 again, different document
    expectTrue(!stack.isAtBaseline() && stack.hasUnsavedChanges(),
               "baseline in a discarded redo branch is unreachable");

    fuse::editor::UndoStack capped;
    capped.setMaxHistory(3);
    capped.set_baseline_state(); // saved empty document
    for (int i = 0; i < 5; ++i) {
        capped.execute(std::make_unique<LogCommand>(log, i));
    }
    while (capped.canUndo()) {
        capped.undo();
    }
    expectTrue(!capped.isAtBaseline(), "evicted baseline is not reported after undoing everything");
}

// B6.6: inspector edit -> CommandStack -> game-thread apply -> undo/redo is exact.
void testInspectorRoundTripExact() {
    fuse::editor::EditorHost host;
    fuse::ecs::Registry& registry = host.editorScene().registry();
    const fuse::ecs::EntityID entity = registry.create();
    fuse::ecs::Transform transform{};
    transform.position = {0.1f, 1.0e-7f, -123456.789f, 1.f};
    registry.add(entity, transform);
    fuse::ecs::SDFObject sdf{};
    sdf.blend_alpha = 0.333333343f;
    registry.add(entity, sdf);

    host.editorState().primarySelection = entity;
    fuse::editor::PropertyInspector inspector;
    inspector.sync(host.editorState(), host.editorScene());
    expectTrue(inspector.hasSelection() && inspector.sections().size() == 2u,
               "inspector reflects Transform + SDFObject sections");

    const fuse::ecs::vec3 edited{1.23456789f, -9.87654321e-5f, 42.4242f, 0.f};
    expectTrue(inspector.setTransformPosition(edited, host.editorScene(), host.commandStack()),
               "inspector position edit accepted");
    host.undoPropertyEdit();
    const fuse::ecs::Transform* live = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(live->position.x == 0.1f && live->position.y == 1.0e-7f &&
                   live->position.z == -123456.789f,
               "inspector undo restores the exact float position (no 6-decimal truncation)");
    host.redoPropertyEdit();
    live = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(live->position.x == edited.x && live->position.y == edited.y && live->position.z == edited.z,
               "inspector redo re-applies the exact edited position");

    expectTrue(inspector.setSdfBlendAlpha(0.1f, host.editorScene(), host.commandStack()), "alpha edit");
    host.undoPropertyEdit();
    expectTrue(registry.get<fuse::ecs::SDFObject>(entity)->blend_alpha == 0.333333343f,
               "blend alpha undo is exact");
    host.redoPropertyEdit();
    expectTrue(registry.get<fuse::ecs::SDFObject>(entity)->blend_alpha == 0.1f, "blend alpha redo");

    expectTrue(fuse::editor::formatPropertyFloat(1.0e-7f) != "0.000000" &&
                   std::strtof(fuse::editor::formatPropertyFloat(0.1f).c_str(), nullptr) == 0.1f,
               "property float text round-trips");
}

// Every editor command kind the host applies is undoable (rename / reparent / delete / transform).
void testEveryCommandUndoable() {
    TestObject root("root", 1u);
    TestObject a("a", 2u);
    TestObject b("b", 3u);
    root.addChild(&a);
    root.addChild(&b);

    fuse::ecs::Registry registry;
    registry.init(8);
    const fuse::ecs::EntityID p = registry.create();
    const fuse::ecs::EntityID c = registry.create();
    registry.add(p, fuse::ecs::Transform{});
    registry.add(c, fuse::ecs::Transform{});

    fuse::editor::UndoStack stack;
    stack.execute(std::make_unique<fuse::editor::SetObjectNameCommand>(a, "a", "renamed"));
    stack.execute(std::make_unique<fuse::editor::ReparentObjectCommand>(b, &a, &root));
    stack.execute(std::make_unique<fuse::editor::ReparentEntityCommand>(registry, c, p,
                                                                        fuse::ecs::EntityID::null()));
    stack.execute(std::make_unique<TransformCommand>(registry, p, makeState(0, 0, 0), makeState(4, 5, 6)));
    stack.execute(std::make_unique<fuse::editor::CreateEntityCommand>(registry,
                                                                      fuse::editor::EntityComponentSet{}));
    expectTrue(registry.count() == 3u, "create command spawns");
    while (stack.canUndo()) {
        stack.undo();
    }
    expectTrue(a.name() == "a" && b.parent() == &root && root.children().size() == 2u &&
                   root.children()[1] == &b,
               "object rename/reparent undone");
    expectTrue(!registry.get<fuse::ecs::Transform>(c)->parent.valid() &&
                   registry.get<fuse::ecs::Transform>(p)->position.x == 0.f && registry.count() == 2u,
               "entity reparent/transform/create undone");
    registry.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();

    testTransformCommandExactRestore();
    testHundredCommandsLifo();
    testConsecutiveDragsMerge();
    testDeleteEntityRestoresAllComponents();
    testMaxHistoryDropsOldest();
    testRedoInvalidation();
    testMacroCommands();
    testBaselineNotFalselyReached();
    testInspectorRoundTripExact();
    testEveryCommandUndoable();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_command_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_command_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
