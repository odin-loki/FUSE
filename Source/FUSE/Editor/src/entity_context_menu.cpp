#include <fuse/editor/entity_context_menu.hpp>

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <algorithm>
#include <memory>
#include <string>

namespace fuse::editor {

namespace {

bool isCreateAction(ContextMenuAction action) {
    return action != ContextMenuAction::Duplicate && action != ContextMenuAction::Delete;
}

const char* createLabel(ContextMenuAction action) {
    switch (action) {
    case ContextMenuAction::CreateEmpty:
        return "Create Empty";
    case ContextMenuAction::CreateCube:
        return "Create Cube";
    case ContextMenuAction::CreateSdfSphere:
        return "Create SDF Sphere";
    case ContextMenuAction::CreateSdfBox:
        return "Create SDF Box";
    case ContextMenuAction::CreatePointLight:
        return "Create Point Light";
    case ContextMenuAction::CreateDirectionalLight:
        return "Create Directional Light";
    case ContextMenuAction::CreateCamera:
        return "Create Camera";
    case ContextMenuAction::Duplicate:
        return "Duplicate";
    case ContextMenuAction::Delete:
        return "Delete";
    }
    return "";
}

void selectOnly(EditorState& state, ecs::EntityID entity) {
    state.selectedEntities.clear();
    state.primarySelection = entity;
    if (entity.valid()) {
        state.selectedEntities.push_back(entity);
    }
}

} // namespace

EntityComponentSet EntityContextMenu::componentsFor(ContextMenuAction action, const ecs::vec3& position) {
    EntityComponentSet components{};
    ecs::Transform transform{};
    transform.position = {position.x, position.y, position.z, 1.f};
    transform.dirty = true;
    std::get<std::optional<ecs::Transform>>(components) = transform;

    switch (action) {
    case ContextMenuAction::CreateCube: {
        ecs::Mesh mesh{};
        mesh.aabb_min = {-0.5f, -0.5f, -0.5f, 0.f};
        mesh.aabb_max = {0.5f, 0.5f, 0.5f, 0.f};
        std::get<std::optional<ecs::Mesh>>(components) = mesh;
        break;
    }
    case ContextMenuAction::CreateSdfSphere:
    case ContextMenuAction::CreateSdfBox: {
        ecs::SDFObject sdf{};
        sdf.type = action == ContextMenuAction::CreateSdfSphere ? ecs::SDFPrimitive::Sphere : ecs::SDFPrimitive::Box;
        sdf.params = action == ContextMenuAction::CreateSdfSphere ? ecs::vec3{0.5f, 0.f, 0.f, 0.f}
                                                                  : ecs::vec3{0.5f, 0.5f, 0.5f, 0.f};
        std::get<std::optional<ecs::SDFObject>>(components) = sdf;
        break;
    }
    case ContextMenuAction::CreatePointLight:
        std::get<std::optional<ecs::PointLight>>(components) = ecs::PointLight{};
        break;
    case ContextMenuAction::CreateDirectionalLight:
        std::get<std::optional<ecs::DirectionalLight>>(components) = ecs::DirectionalLight{};
        break;
    case ContextMenuAction::CreateCamera:
        std::get<std::optional<ecs::Camera>>(components) = ecs::Camera{};
        break;
    case ContextMenuAction::CreateEmpty:
    case ContextMenuAction::Duplicate:
    case ContextMenuAction::Delete:
        break;
    }
    return components;
}

void EntityContextMenu::rebuildItems_(const EditorState& state) {
    const bool hasSelection = !state.selectedEntities.empty() || state.primarySelection.valid();
    m_items.clear();
    for (ContextMenuAction action :
         {ContextMenuAction::CreateEmpty, ContextMenuAction::CreateCube, ContextMenuAction::CreateSdfSphere,
          ContextMenuAction::CreateSdfBox, ContextMenuAction::CreatePointLight,
          ContextMenuAction::CreateDirectionalLight, ContextMenuAction::CreateCamera}) {
        m_items.push_back({action, createLabel(action), true, false});
    }
    m_items.push_back({ContextMenuAction::Duplicate, createLabel(ContextMenuAction::Duplicate), hasSelection, true});
    m_items.push_back({ContextMenuAction::Delete, createLabel(ContextMenuAction::Delete), hasSelection, false});
}

void EntityContextMenu::openAt(ecs::EntityID clicked, const ecs::vec3& spawnPoint, EditorState& state) {
    m_clicked = clicked;
    m_spawnPoint = spawnPoint;
    if (clicked.valid() &&
        std::find(state.selectedEntities.begin(), state.selectedEntities.end(), clicked) ==
            state.selectedEntities.end()) {
        // Right-clicking outside the selection retargets it (Qt / DCC convention).
        selectOnly(state, clicked);
    }
    rebuildItems_(state);
    m_open = true;
}

void EntityContextMenu::viewportToWindow(const ContextMenuHostGeometry& host, f32 px, f32 py, f32& wx, f32& wy) {
    const f32 dpr = host.devicePixelRatio > 0.f ? host.devicePixelRatio : 1.f;
    wx = host.viewportOriginX + px / dpr;
    wy = host.viewportOriginY + py / dpr;
}

ContextMenuPlacement EntityContextMenu::place(const ContextMenuHostGeometry& host, f32 windowX, f32 windowY,
                                              f32 menuWidth, f32 menuHeight) {
    ContextMenuPlacement out{};
    out.clickX = windowX;
    out.clickY = windowY;
    out.width = menuWidth;
    out.height = menuHeight;
    out.x = windowX;
    out.y = windowY;
    if (host.windowWidth > 0.f && out.x + menuWidth > host.windowWidth) {
        out.x = windowX - menuWidth;
        out.flippedX = true;
        out.x = std::clamp(out.x, 0.f, std::max(host.windowWidth - menuWidth, 0.f));
    }
    if (host.windowHeight > 0.f && out.y + menuHeight > host.windowHeight) {
        out.y = windowY - menuHeight;
        out.flippedY = true;
        out.y = std::clamp(out.y, 0.f, std::max(host.windowHeight - menuHeight, 0.f));
    }
    out.x = std::max(out.x, 0.f);
    out.y = std::max(out.y, 0.f);
    return out;
}

void EntityContextMenu::setMenuSize(f32 width, f32 height) {
    m_placement = place(m_host, m_placement.clickX, m_placement.clickY, width, height);
}

f32 EntityContextMenu::menuHeight() const {
    f32 height = 2.f * kMenuPadding;
    for (const ContextMenuItem& item : m_items) {
        height += kItemHeight + (item.separatorBefore ? kSeparatorHeight : 0.f);
    }
    return height;
}

void EntityContextMenu::openAt(ecs::EntityID clicked, const ecs::vec3& spawnPoint, EditorState& state, f32 windowX,
                               f32 windowY) {
    openAt(clicked, spawnPoint, state);
    m_placement = place(m_host, windowX, windowY, menuWidth(), menuHeight());
}

void EntityContextMenu::openInViewport(EditorScene& scene, ViewportSceneView& view, const ViewportPanel& viewport,
                                       f32 px, f32 py, EditorState& state) {
    const ViewportRay ray = viewport.screenRay(px, py);
    const ViewportPickResult pick = view.pickRay(scene, ray, viewport.camera().farPlane);
    ecs::vec3 spawn{};
    if (pick.hit) {
        spawn = pick.point;
    } else {
        spawn = {ray.origin.x + ray.direction.x * kSpawnDistance, ray.origin.y + ray.direction.y * kSpawnDistance,
                 ray.origin.z + ray.direction.z * kSpawnDistance, 1.f};
    }
    f32 wx = 0.f;
    f32 wy = 0.f;
    viewportToWindow(m_host, px, py, wx, wy);
    openAt(pick.hit ? pick.entity : ecs::EntityID::null(), spawn, state, wx, wy);
}

const ContextMenuItem* EntityContextMenu::find(ContextMenuAction action) const {
    for (const ContextMenuItem& item : m_items) {
        if (item.action == action) {
            return &item;
        }
    }
    return nullptr;
}

bool EntityContextMenu::activate(ContextMenuAction action, EditorScene& scene, EditorState& state,
                                 UndoStack& undo) {
    const ContextMenuItem* item = find(action);
    if (!m_open || item == nullptr || !item->enabled) {
        return false;
    }
    m_open = false;
    ecs::Registry& registry = scene.registry();

    if (isCreateAction(action)) {
        auto command = std::make_unique<CreateEntityCommand>(registry, componentsFor(action, m_spawnPoint),
                                                             createLabel(action));
        CreateEntityCommand* created = command.get();
        undo.execute(std::move(command));
        m_lastCreated = created->createdEntity();
        if (!m_lastCreated.valid() || !registry.alive(m_lastCreated)) {
            return false;
        }
        selectOnly(state, m_lastCreated);
        state.sceneModified = true;
        return true;
    }

    std::vector<ecs::EntityID> targets = state.selectedEntities;
    if (targets.empty() && state.primarySelection.valid()) {
        targets.push_back(state.primarySelection);
    }
    targets.erase(std::remove_if(targets.begin(), targets.end(),
                                 [&](ecs::EntityID id) { return !registry.alive(id); }),
                  targets.end());
    if (targets.empty()) {
        return false;
    }

    if (action == ContextMenuAction::Duplicate) {
        undo.beginMacro("Duplicate");
        std::vector<ecs::EntityID> copies;
        for (ecs::EntityID source : targets) {
            auto command = std::make_unique<CreateEntityCommand>(
                registry, captureEntityComponents(registry, source), "Duplicate entity");
            CreateEntityCommand* created = command.get();
            undo.execute(std::move(command));
            if (created->createdEntity().valid()) {
                copies.push_back(created->createdEntity());
            }
        }
        undo.endMacro();
        if (copies.empty()) {
            return false;
        }
        m_lastCreated = copies.back();
        state.selectedEntities = copies;
        state.primarySelection = copies.back();
        state.sceneModified = true;
        return true;
    }

    // Delete: one undo step for the whole selection.
    undo.beginMacro(targets.size() == 1u ? "Delete entity" : "Delete " + std::to_string(targets.size()) + " entities");
    for (ecs::EntityID id : targets) {
        undo.execute(std::make_unique<DeleteEntityCommand>(registry, id));
    }
    undo.endMacro();
    selectOnly(state, ecs::EntityID::null());
    state.sceneModified = true;
    return true;
}

} // namespace fuse::editor
