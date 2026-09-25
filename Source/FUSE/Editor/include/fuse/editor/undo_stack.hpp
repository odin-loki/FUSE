#pragma once

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace fuse::editor {

/// Captured `UndoStack` metadata for snapshot/restore (B6.2 deepen).
/// Restore rewinds live undo/redo depth; command cloning remains deferred.
struct UndoStackSnapshot {
    u32 undoCount = 0;
    u32 redoCount = 0;
    u32 evictedCount = 0;
    u32 coalescedOps = 0;
    u32 coalescedOpsAtBaseline = 0;
    u32 baselineUndoCount = 0;
    u32 baselineRedoCount = 0;
    bool baselineConfigured = false;
    bool baselineLost = false;
    bool dirty = false;
    u32 dirtyRevision = 0;
    std::vector<std::string> undoDescriptions;
    std::vector<std::string> redoDescriptions;
};

/// Undo/redo command interface (B6.2). Game-thread mutations only.
class UndoCommand {
public:
    virtual ~UndoCommand() = default;

    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual std::string description() const = 0;

    /// Merge consecutive commands (e.g. transform drags). Returns true when merged.
    virtual bool merge(const UndoCommand& /*other*/) { return false; }
};

/// Macro / compound command: children execute in order and undo in reverse as one step (B6.2).
class CompoundCommand final : public UndoCommand {
public:
    explicit CompoundCommand(std::string description) : m_description(std::move(description)) {}

    /// Appends an already-executed child; merges into the previous child when it accepts.
    void addExecuted(std::unique_ptr<UndoCommand> command);

    void execute() override;
    void undo() override;
    std::string description() const override { return m_description; }

    [[nodiscard]] bool empty() const { return m_children.empty(); }
    [[nodiscard]] usize childCount() const { return m_children.size(); }

private:
    std::string m_description;
    std::vector<std::unique_ptr<UndoCommand>> m_children;
};

/// LIFO undo/redo stack for reversible scene mutations (B6.2).
class UndoStack {
public:
    static constexpr u32 kMaxHistory = 256;

    void execute(std::unique_ptr<UndoCommand> command);
    void push(std::unique_ptr<UndoCommand> command) { execute(std::move(command)); }
    void undo();
    void redo();

    /// Groups every command executed until the matching `endMacro()` into one undo step.
    /// Nested begin/end pairs fold into the outermost macro.
    void beginMacro(std::string description);
    void endMacro();
    [[nodiscard]] bool isRecordingMacro() const { return m_macroDepth > 0u; }

    /// N-level history bound (default `kMaxHistory`, minimum 1); trims the oldest steps.
    void setMaxHistory(u32 maxHistory);
    [[nodiscard]] u32 maxHistory() const { return m_maxHistory; }

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    /// True when the undo branch has no recorded steps (B6.2 deepen — empty-stack early-out).
    [[nodiscard]] bool isEmpty() const { return m_undo.empty(); }
    /// True when the redo branch has no recorded steps (B6.2 deepen — empty-stack early-out).
    [[nodiscard]] bool isRedoEmpty() const { return m_redo.empty(); }
    /// True after `set_baseline_state` has recorded a saved-document depth (B6.2 deepen).
    [[nodiscard]] bool isBaselineConfigured() const { return m_baselineConfigured; }

    u32 undoCount() const { return static_cast<u32>(m_undo.size()); }
    u32 redoCount() const { return static_cast<u32>(m_redo.size()); }
    u32 evictedCount() const { return m_evictedCount; }
    u32 coalescedOps() const { return m_coalescedOps; }
    /// Coalesce events recorded since the last `set_baseline_state` call.
    u32 coalescedOpsSinceBaseline() const;

    [[nodiscard]] bool isDirty() const { return m_dirty; }
    u32 dirtyRevision() const { return m_dirtyRevision; }
    /// True when `dirtyRevision()` has advanced past `revision` (B6.2 deepen).
    [[nodiscard]] bool isDirtySince(u32 revision) const { return dirtyRevision() > revision; }
    void markClean();
    /// Records the current undo/redo depth as the saved-document baseline (B6.2 deepen).
    void set_baseline_state();
    u32 baselineUndoCount() const { return m_baselineUndoCount; }
    u32 baselineRedoCount() const { return m_baselineRedoCount; }
    [[nodiscard]] bool isAtBaseline() const;

    /// True when undo/redo depth differs from the last `set_baseline_state` call.
    [[nodiscard]] bool hasUnsavedChanges() const { return !isAtBaseline(); }

    std::string peekUndoDescription() const;
    std::string peekRedoDescription() const;

    void clear();

    UndoStackSnapshot captureSnapshot() const;
    void restoreSnapshot(const UndoStackSnapshot& snapshot);

private:
    void evictOldestIfNeeded_();
    void pushExecuted_(std::unique_ptr<UndoCommand> command);
    /// Drops the redo branch; the saved baseline is unreachable when it lived in that branch.
    void discardRedo_();

    void markDirty_();
    void markDirtyAndBump_();
    /// Clears dirty when undo depth matches the saved baseline; otherwise marks dirty.
    void syncBaselineDirty_();

    std::vector<std::unique_ptr<UndoCommand>> m_undo;
    std::vector<std::unique_ptr<UndoCommand>> m_redo;
    std::unique_ptr<CompoundCommand> m_macro;
    u32 m_macroDepth = 0;
    u32 m_maxHistory = kMaxHistory;
    bool m_baselineLost = false;
    u32 m_evictedCount = 0;
    u32 m_coalescedOps = 0;
    u32 m_coalescedOpsAtBaseline = 0;
    u32 m_baselineUndoCount = 0;
    u32 m_baselineRedoCount = 0;
    bool m_baselineConfigured = false;
    bool m_dirty = false;
    u32 m_dirtyRevision = 0;
};

/// Rename an object and restore the previous name on undo.
class SetObjectNameCommand final : public UndoCommand {
public:
    SetObjectNameCommand(Object& object, std::string before, std::string after);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    Object& m_object;
    std::string m_before;
    std::string m_after;
};

/// True when `candidate` is `object` or one of its descendants (reparent would form a cycle).
bool isSelfOrDescendant(const Object& object, const Object* candidate);
/// True when parenting `entity` under `newParent` would form a Transform::parent cycle.
bool wouldCreateParentCycle(const ecs::Registry& registry, ecs::EntityID entity,
                            ecs::EntityID newParent);

/// Reparent an object in the scene graph. Rejects cycles (execute is a no-op) and restores the
/// original sibling order on undo. The parent to restore is read from the object at execute
/// time (`oldParent` is only a hint for callers).
class ReparentObjectCommand final : public UndoCommand {
public:
    ReparentObjectCommand(Object& object, Object* newParent, Object* oldParent);

    void execute() override;
    void undo() override;
    std::string description() const override;

    /// False when the requested parent is the object itself or one of its descendants.
    [[nodiscard]] bool isValid() const;

private:
    Object& m_object;
    Object* m_newParent = nullptr;
    Object* m_oldParent = nullptr;
    usize m_oldSiblingIndex = 0;
    bool m_applied = false;
};

/// Reparent an ECS entity via Transform::parent (U6 game-thread apply). Rejects cycles.
class ReparentEntityCommand final : public UndoCommand {
public:
    ReparentEntityCommand(ecs::Registry& registry, ecs::EntityID entity, ecs::EntityID newParent,
                          ecs::EntityID oldParent);

    void execute() override;
    void undo() override;
    std::string description() const override;

private:
    ecs::Registry& m_registry;
    ecs::EntityID m_entity = ecs::EntityID::null();
    ecs::EntityID m_newParent = ecs::EntityID::null();
    ecs::EntityID m_oldParent = ecs::EntityID::null();
    bool m_applied = false;
};

/// Local TRS edit of an ECS Transform with before/after state; consecutive edits of the same
/// entity merge into one undo step (gizmo drags).
class TransformCommand final : public UndoCommand {
public:
    struct State {
        ecs::vec3 position{};
        ecs::quat rotation{};
        ecs::vec3 scale{1.f, 1.f, 1.f, 0.f};
    };

    static State capture(const ecs::Transform& transform);

    TransformCommand(ecs::Registry& registry, ecs::EntityID entity, const State& before,
                     const State& after);

    void execute() override;
    void undo() override;
    std::string description() const override;
    bool merge(const UndoCommand& other) override;

    [[nodiscard]] const State& before() const { return m_before; }
    [[nodiscard]] const State& after() const { return m_after; }

private:
    void apply_(const State& state);

    ecs::Registry& m_registry;
    ecs::EntityID m_entity = ecs::EntityID::null();
    State m_before{};
    State m_after{};
};

/// Replace an entity's SDFObject (sculpt Paint / Roughen edits) with before/after values.
class SdfObjectEditCommand final : public UndoCommand {
public:
    SdfObjectEditCommand(ecs::Registry& registry, ecs::EntityID entity, const ecs::SDFObject& before,
                         const ecs::SDFObject& after, std::string description = "Edit SDF object");

    void execute() override;
    void undo() override;
    std::string description() const override { return m_description; }

private:
    ecs::Registry& m_registry;
    ecs::EntityID m_entity = ecs::EntityID::null();
    ecs::SDFObject m_before{};
    ecs::SDFObject m_after{};
    std::string m_description;
};

/// Every built-in component an entity may carry (matches `ecs::register_builtin_components`).
using EntityComponentSet =
    std::tuple<std::optional<ecs::Transform>, std::optional<ecs::Mesh>, std::optional<ecs::RigidBody>,
               std::optional<ecs::SDFObject>, std::optional<ecs::Camera>,
               std::optional<ecs::DirectionalLight>, std::optional<ecs::PointLight>,
               std::optional<ecs::SpotLight>, std::optional<ecs::SpawnMarker>,
               std::optional<ecs::Collider>, std::optional<ecs::TagStatic>,
               std::optional<ecs::TagPlayer>, std::optional<ecs::TagKinematic>,
               std::optional<ecs::TagDestroy>>;

EntityComponentSet captureEntityComponents(const ecs::Registry& registry, ecs::EntityID entity);
void restoreEntityComponents(ecs::Registry& registry, ecs::EntityID entity,
                             const EntityComponentSet& components);

/// Create an ECS entity carrying `components`; undo destroys it, redo revives the same id via
/// `Registry::create_at` (a fresh id only if the slot was reused meanwhile, see `createdEntity()`).
class CreateEntityCommand final : public UndoCommand {
public:
    CreateEntityCommand(ecs::Registry& registry, EntityComponentSet components,
                        std::string description = "Create entity");
    ~CreateEntityCommand() override;

    void execute() override;
    void undo() override;
    std::string description() const override { return m_description; }

    [[nodiscard]] ecs::EntityID createdEntity() const { return m_entity; }

private:
    ecs::Registry& m_registry;
    EntityComponentSet m_components{};
    std::string m_description;
    ecs::EntityID m_entity = ecs::EntityID::null();
};

/// Destroy an ECS entity and orphan its transform children (U6 game-thread apply). Undo recreates
/// the entity with every built-in component restored byte-for-byte and re-links the children;
/// redo destroys it again. The slot is reserved while deleted (`destroy_entity_reserved`) and undo
/// revives the SAME id via `Registry::create_at`, so earlier undo history that refers to the
/// entity keeps working; the reservation is released when the command is dropped. The registry
/// must outlive the command.
class DeleteEntityCommand final : public UndoCommand {
public:
    DeleteEntityCommand(ecs::Registry& registry, ecs::EntityID entity);
    ~DeleteEntityCommand() override;

    void execute() override;
    void undo() override;
    std::string description() const override;

    [[nodiscard]] ecs::EntityID liveEntity() const { return m_liveEntity; }

private:
    ecs::Registry& m_registry;
    ecs::EntityID m_entity = ecs::EntityID::null();
    ecs::EntityID m_liveEntity = ecs::EntityID::null();
    std::vector<ecs::EntityID> m_orphanedChildren;
    EntityComponentSet m_components{};
    bool m_deleted = false;
};

} // namespace fuse::editor
