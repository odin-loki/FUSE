#include <fuse/editor/undo_stack.hpp>

#include <fuse/ecs/components/transform.hpp>

#include <algorithm>

namespace fuse::editor {

namespace {

const std::string kEmptyDescription;

} // namespace

void CompoundCommand::addExecuted(std::unique_ptr<UndoCommand> command) {
    if (!command) {
        return;
    }
    if (!m_children.empty() && m_children.back()->merge(*command)) {
        return;
    }
    m_children.push_back(std::move(command));
}

void CompoundCommand::execute() {
    for (const std::unique_ptr<UndoCommand>& child : m_children) {
        child->execute();
    }
}

void CompoundCommand::undo() {
    for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
        (*it)->undo();
    }
}

void UndoStack::evictOldestIfNeeded_() {
    while (m_undo.size() > m_maxHistory) {
        m_undo.erase(m_undo.begin());
        ++m_evictedCount;
        if (m_baselineConfigured) {
            if (m_baselineUndoCount > 0u) {
                --m_baselineUndoCount;
            } else {
                // The saved state predates the oldest retained step — no undo depth reaches it.
                m_baselineLost = true;
            }
        }
    }
}

void UndoStack::discardRedo_() {
    if (m_redo.empty()) {
        return;
    }
    if (m_baselineConfigured && m_baselineUndoCount > undoCount()) {
        m_baselineLost = true;
    }
    m_redo.clear();
}

void UndoStack::pushExecuted_(std::unique_ptr<UndoCommand> command) {
    discardRedo_();
    m_undo.push_back(std::move(command));
    evictOldestIfNeeded_();
    markDirtyAndBump_();
}

void UndoStack::beginMacro(std::string description) {
    if (m_macroDepth++ == 0u) {
        m_macro = std::make_unique<CompoundCommand>(std::move(description));
    }
}

void UndoStack::endMacro() {
    if (m_macroDepth == 0u) {
        return;
    }
    if (--m_macroDepth > 0u) {
        return;
    }

    std::unique_ptr<CompoundCommand> macro = std::move(m_macro);
    if (macro && !macro->empty()) {
        pushExecuted_(std::move(macro));
    }
}

void UndoStack::setMaxHistory(u32 maxHistory) {
    m_maxHistory = maxHistory == 0u ? 1u : maxHistory;
    evictOldestIfNeeded_();
}

void UndoStack::markDirty_() {
    if (!m_dirty) {
        ++m_dirtyRevision;
    }
    m_dirty = true;
}

void UndoStack::markDirtyAndBump_() {
    m_dirty = true;
    ++m_dirtyRevision;
}

void UndoStack::syncBaselineDirty_() {
    if (!m_baselineConfigured) {
        markDirtyAndBump_();
        return;
    }

    if (isAtBaseline()) {
        markClean();
        return;
    }

    markDirtyAndBump_();
}

u32 UndoStack::coalescedOpsSinceBaseline() const {
    if (m_coalescedOps < m_coalescedOpsAtBaseline) {
        return 0u;
    }

    return m_coalescedOps - m_coalescedOpsAtBaseline;
}

void UndoStack::execute(std::unique_ptr<UndoCommand> command) {
    if (!command) {
        return;
    }

    if (m_macro) {
        command->execute();
        m_macro->addExecuted(std::move(command));
        return;
    }

    if (canUndo() && m_undo.back()->merge(*command)) {
        m_undo.back()->execute();
        ++m_coalescedOps;
        // A merge is a new edit: the redo branch no longer follows from the current state, and a
        // baseline taken at this depth no longer describes the merged result.
        discardRedo_();
        if (m_baselineConfigured && m_baselineUndoCount == undoCount()) {
            m_baselineLost = true;
        }
        markDirty_();
        return;
    }

    command->execute();
    pushExecuted_(std::move(command));
}

void UndoStack::set_baseline_state() {
    if (m_baselineConfigured && isAtBaseline() && m_baselineRedoCount == redoCount() &&
        m_coalescedOpsAtBaseline == m_coalescedOps) {
        markClean();
        return;
    }

    m_baselineUndoCount = undoCount();
    m_baselineRedoCount = redoCount();
    m_coalescedOpsAtBaseline = m_coalescedOps;
    m_baselineConfigured = true;
    m_baselineLost = false;
    markClean();
}

bool UndoStack::isAtBaseline() const {
    return !m_baselineLost && undoCount() == m_baselineUndoCount;
}

void UndoStack::undo() {
    if (m_undo.empty() || m_macro) {
        return;
    }

    std::unique_ptr<UndoCommand> command = std::move(m_undo.back());
    m_undo.pop_back();
    command->undo();
    m_redo.push_back(std::move(command));
    syncBaselineDirty_();
}

void UndoStack::redo() {
    if (m_redo.empty() || m_macro) {
        return;
    }

    std::unique_ptr<UndoCommand> command = std::move(m_redo.back());
    m_redo.pop_back();
    command->execute();
    m_undo.push_back(std::move(command));
    evictOldestIfNeeded_();
    syncBaselineDirty_();
}

std::string UndoStack::peekUndoDescription() const {
    if (m_undo.empty()) {
        return kEmptyDescription;
    }
    return m_undo.back()->description();
}

std::string UndoStack::peekRedoDescription() const {
    if (m_redo.empty()) {
        return kEmptyDescription;
    }
    return m_redo.back()->description();
}

void UndoStack::clear() {
    if (isEmpty() && m_redo.empty() && !m_macro && !m_baselineConfigured && m_coalescedOps == 0u &&
        !m_dirty && m_dirtyRevision == 0u) {
        return;
    }

    m_undo.clear();
    m_redo.clear();
    m_macro.reset();
    m_macroDepth = 0;
    m_baselineLost = false;
    m_evictedCount = 0;
    m_coalescedOps = 0;
    m_coalescedOpsAtBaseline = 0;
    m_baselineUndoCount = 0;
    m_baselineRedoCount = 0;
    m_baselineConfigured = false;
    m_dirty = false;
    m_dirtyRevision = 0;
}

void UndoStack::markClean() {
    if (!m_dirty) {
        return;
    }
    m_dirty = false;
}

UndoStackSnapshot UndoStack::captureSnapshot() const {
    UndoStackSnapshot snapshot;
    snapshot.undoCount = undoCount();
    snapshot.redoCount = redoCount();
    snapshot.evictedCount = m_evictedCount;
    snapshot.coalescedOps = m_coalescedOps;
    snapshot.coalescedOpsAtBaseline = m_coalescedOpsAtBaseline;
    snapshot.baselineUndoCount = m_baselineUndoCount;
    snapshot.baselineRedoCount = m_baselineRedoCount;
    snapshot.baselineConfigured = m_baselineConfigured;
    snapshot.baselineLost = m_baselineLost;
    snapshot.dirty = m_dirty;
    snapshot.dirtyRevision = m_dirtyRevision;
    snapshot.undoDescriptions.reserve(m_undo.size());
    for (const auto& command : m_undo) {
        snapshot.undoDescriptions.push_back(command->description());
    }
    snapshot.redoDescriptions.reserve(m_redo.size());
    for (const auto& command : m_redo) {
        snapshot.redoDescriptions.push_back(command->description());
    }
    return snapshot;
}

void UndoStack::restoreSnapshot(const UndoStackSnapshot& snapshot) {
    while (undoCount() < snapshot.undoCount && canRedo()) {
        redo();
    }

    while (undoCount() > snapshot.undoCount && canUndo()) {
        undo();
    }

    while (redoCount() > snapshot.redoCount) {
        m_redo.pop_back();
    }

    m_evictedCount = snapshot.evictedCount;
    m_coalescedOps = snapshot.coalescedOps;
    m_coalescedOpsAtBaseline = snapshot.coalescedOpsAtBaseline;
    m_baselineUndoCount = snapshot.baselineUndoCount;
    m_baselineRedoCount = snapshot.baselineRedoCount;
    m_baselineConfigured = snapshot.baselineConfigured;
    m_baselineLost = snapshot.baselineLost;
    m_dirty = snapshot.dirty;
    m_dirtyRevision = snapshot.dirtyRevision;
}

SetObjectNameCommand::SetObjectNameCommand(Object& object, std::string before, std::string after)
    : m_object(object), m_before(std::move(before)), m_after(std::move(after)) {}

void SetObjectNameCommand::execute() {
    m_object.setName(m_after);
}

void SetObjectNameCommand::undo() {
    m_object.setName(m_before);
}

std::string SetObjectNameCommand::description() const {
    return "Rename " + m_before + " to " + m_after;
}

bool isSelfOrDescendant(const Object& object, const Object* candidate) {
    for (const Object* node = candidate; node != nullptr; node = node->parent()) {
        if (node == &object) {
            return true;
        }
    }
    return false;
}

bool wouldCreateParentCycle(const ecs::Registry& registry, ecs::EntityID entity,
                            ecs::EntityID newParent) {
    // Walk up from the new parent; reaching `entity` means it would become its own ancestor.
    // The hop bound also stops on a pre-existing (corrupt) cycle above the new parent.
    ecs::EntityID node = newParent;
    for (usize hops = 0; node.valid() && hops <= registry.count(); ++hops) {
        if (node == entity) {
            return true;
        }
        const ecs::Transform* transform = registry.get<ecs::Transform>(node);
        if (transform == nullptr) {
            return false;
        }
        node = transform->parent;
    }
    return node.valid();
}

ReparentObjectCommand::ReparentObjectCommand(Object& object, Object* newParent, Object* oldParent)
    : m_object(object), m_newParent(newParent), m_oldParent(oldParent) {}

bool ReparentObjectCommand::isValid() const {
    return m_newParent == nullptr || !isSelfOrDescendant(m_object, m_newParent);
}

void ReparentObjectCommand::execute() {
    m_applied = false;
    if (!isValid()) {
        return;
    }

    m_oldParent = m_object.parent();
    if (m_oldParent != nullptr) {
        const std::vector<Object*>& siblings = m_oldParent->children();
        m_oldSiblingIndex = static_cast<usize>(
            std::find(siblings.begin(), siblings.end(), &m_object) - siblings.begin());
    }

    m_object.reparent(m_newParent);
    m_applied = true;
}

void ReparentObjectCommand::undo() {
    if (!m_applied) {
        return;
    }
    m_applied = false;

    if (m_oldParent == nullptr) {
        m_object.reparent(nullptr);
        return;
    }

    // `Object` only appends children: re-append the object, then rotate the siblings that
    // originally followed it back behind it so the original order is restored exactly.
    m_object.reparent(m_oldParent);
    const std::vector<Object*> siblings = m_oldParent->children();
    std::vector<Object*> trailing;
    for (usize i = m_oldSiblingIndex; i < siblings.size(); ++i) {
        if (siblings[i] != &m_object) {
            trailing.push_back(siblings[i]);
        }
    }
    for (Object* sibling : trailing) {
        m_oldParent->removeChild(sibling);
        m_oldParent->addChild(sibling);
    }
}

std::string ReparentObjectCommand::description() const {
    return "Reparent " + m_object.name();
}

ReparentEntityCommand::ReparentEntityCommand(ecs::Registry& registry, ecs::EntityID entity,
                                             ecs::EntityID newParent, ecs::EntityID oldParent)
    : m_registry(registry),
      m_entity(entity),
      m_newParent(newParent),
      m_oldParent(oldParent) {}

void ReparentEntityCommand::execute() {
    m_applied = false;
    ecs::Transform* transform = m_registry.get<ecs::Transform>(m_entity);
    if (transform == nullptr || wouldCreateParentCycle(m_registry, m_entity, m_newParent)) {
        return;
    }

    transform->parent = m_newParent;
    transform->dirty = true;
    m_applied = true;
}

void ReparentEntityCommand::undo() {
    if (!m_applied) {
        return;
    }
    m_applied = false;

    ecs::Transform* transform = m_registry.get<ecs::Transform>(m_entity);
    if (transform == nullptr) {
        return;
    }

    transform->parent = m_oldParent;
    transform->dirty = true;
}

std::string ReparentEntityCommand::description() const {
    return "Reparent entity";
}

TransformCommand::State TransformCommand::capture(const ecs::Transform& transform) {
    return State{transform.position, transform.rotation, transform.scale};
}

TransformCommand::TransformCommand(ecs::Registry& registry, ecs::EntityID entity,
                                   const State& before, const State& after)
    : m_registry(registry), m_entity(entity), m_before(before), m_after(after) {}

void TransformCommand::apply_(const State& state) {
    ecs::Transform* transform = m_registry.get<ecs::Transform>(m_entity);
    if (transform == nullptr) {
        return;
    }
    transform->position = state.position;
    transform->rotation = state.rotation;
    transform->scale = state.scale;
    transform->dirty = true;
}

void TransformCommand::execute() {
    apply_(m_after);
}

void TransformCommand::undo() {
    apply_(m_before);
}

std::string TransformCommand::description() const {
    return "Move entity";
}

bool TransformCommand::merge(const UndoCommand& other) {
    const auto* typed = dynamic_cast<const TransformCommand*>(&other);
    if (typed == nullptr || &typed->m_registry != &m_registry || typed->m_entity != m_entity) {
        return false;
    }
    m_after = typed->m_after;
    return true;
}

SdfObjectEditCommand::SdfObjectEditCommand(ecs::Registry& registry, ecs::EntityID entity,
                                           const ecs::SDFObject& before, const ecs::SDFObject& after,
                                           std::string description)
    : m_registry(registry),
      m_entity(entity),
      m_before(before),
      m_after(after),
      m_description(std::move(description)) {}

void SdfObjectEditCommand::execute() {
    if (ecs::SDFObject* sdf = m_registry.get<ecs::SDFObject>(m_entity)) {
        *sdf = m_after;
    }
}

void SdfObjectEditCommand::undo() {
    if (ecs::SDFObject* sdf = m_registry.get<ecs::SDFObject>(m_entity)) {
        *sdf = m_before;
    }
}

namespace {

template <typename T>
void captureComponent(const ecs::Registry& registry, ecs::EntityID entity, std::optional<T>& out) {
    out.reset();
    if (const T* component = registry.get<T>(entity)) {
        out = *component;
    }
}

template <typename T>
void restoreComponent(ecs::Registry& registry, ecs::EntityID entity, const std::optional<T>& in) {
    if (in.has_value()) {
        registry.add<T>(entity, *in);
    }
}

} // namespace

EntityComponentSet captureEntityComponents(const ecs::Registry& registry, ecs::EntityID entity) {
    EntityComponentSet components{};
    std::apply([&](auto&... slot) { (captureComponent(registry, entity, slot), ...); }, components);
    return components;
}

void restoreEntityComponents(ecs::Registry& registry, ecs::EntityID entity,
                             const EntityComponentSet& components) {
    std::apply([&](const auto&... slot) { (restoreComponent(registry, entity, slot), ...); },
               components);
}

CreateEntityCommand::CreateEntityCommand(ecs::Registry& registry, EntityComponentSet components,
                                         std::string description)
    : m_registry(registry),
      m_components(std::move(components)),
      m_description(std::move(description)) {}

void CreateEntityCommand::execute() {
    if (m_entity.valid() && m_registry.alive(m_entity)) {
        return;
    }
    // Redo revives the id undo destroyed so later history that refers to it stays valid.
    ecs::EntityID revived = m_entity.valid() ? m_registry.create_at(m_entity) : ecs::EntityID::null();
    m_entity = revived.valid() ? revived : m_registry.create();
    if (m_entity.valid()) {
        restoreEntityComponents(m_registry, m_entity, m_components);
    }
}

CreateEntityCommand::~CreateEntityCommand() {
    // Dropped while undone (redo branch discarded / history evicted): the id can never be revived.
    if (m_entity.valid()) {
        m_registry.release_reserved(m_entity);
    }
}

void CreateEntityCommand::undo() {
    if (m_entity.valid() && m_registry.alive(m_entity)) {
        // Reserve the slot so redo can revive the same id even if other entities are created.
        m_registry.destroy_entity_reserved(m_entity);
    }
}

DeleteEntityCommand::DeleteEntityCommand(ecs::Registry& registry, ecs::EntityID entity)
    : m_registry(registry), m_entity(entity), m_liveEntity(entity) {}

DeleteEntityCommand::~DeleteEntityCommand() {
    // Dropped while deleted (history evicted / cleared): release the id's slot for reuse.
    if (m_deleted) {
        m_registry.release_reserved(m_liveEntity);
    }
}

void DeleteEntityCommand::execute() {
    if (m_deleted || !m_liveEntity.valid() || !m_registry.alive(m_liveEntity)) {
        return;
    }

    m_orphanedChildren.clear();
    m_registry.each_query<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& transform) {
        if (transform.parent == m_liveEntity) {
            m_orphanedChildren.push_back(id);
            transform.parent = ecs::EntityID::null();
            transform.dirty = true;
        }
    });

    m_components = captureEntityComponents(m_registry, m_liveEntity);
    // Reserve the slot: entities created meanwhile cannot take the id, so undo revives it exactly.
    m_registry.destroy_entity_reserved(m_liveEntity);
    m_deleted = true;
}

void DeleteEntityCommand::undo() {
    if (!m_deleted) {
        return;
    }

    // Revive the exact id so older history (transform edits, reparents) that refers to it keeps
    // working; fall back to a fresh id only if the slot was reused outside the undo stack.
    m_liveEntity = m_registry.create_at(m_liveEntity);
    if (!m_liveEntity.valid()) {
        m_liveEntity = m_registry.create();
    }
    if (!m_liveEntity.valid()) {
        return;
    }
    restoreEntityComponents(m_registry, m_liveEntity, m_components);
    m_deleted = false;

    for (ecs::EntityID child : m_orphanedChildren) {
        if (!m_registry.alive(child)) {
            continue;
        }
        ecs::Transform* childTransform = m_registry.get<ecs::Transform>(child);
        if (childTransform == nullptr) {
            continue;
        }
        childTransform->parent = m_liveEntity;
        childTransform->dirty = true;
    }
}

std::string DeleteEntityCommand::description() const {
    return "Delete entity";
}

} // namespace fuse::editor
