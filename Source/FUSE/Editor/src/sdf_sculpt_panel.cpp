#include <fuse/editor/sdf_sculpt_panel.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <tuple>

namespace fuse::editor {

void SdfSculptPanel::sync(const EditorState& state, EditorScene& scene) {
    m_sculptActive = state.primarySelection.valid() && scene.registry().alive(state.primarySelection) &&
                     scene.registry().has<ecs::SDFObject>(state.primarySelection);
}

void SdfSculptPanel::setBrushRadius(f32 radius) {
    m_brush.radius = radius;
}

void SdfSculptPanel::setBrushOperation(BrushOp op) {
    m_brush.op = op;
}

void SdfSculptPanel::setBlendAlpha(f32 alpha) {
    m_brush.blendAlpha = alpha;
}

bool SdfSculptPanel::shouldEmitStroke(const ecs::vec3& hitPoint) const {
    if (!m_strokeOpen) {
        return true;
    }

    const f32 dx = hitPoint.x - m_lastStrokePos.x;
    const f32 dy = hitPoint.y - m_lastStrokePos.y;
    const f32 dz = hitPoint.z - m_lastStrokePos.z;
    const f32 distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return distance >= m_strokeSpacing;
}

ecs::vec3 SdfSculptPanel::mirrorHitPoint(const ecs::vec3& hitPoint) const {
    ecs::vec3 mirrored = hitPoint;
    mirrored.x = -mirrored.x;
    return mirrored;
}

bool SdfSculptPanel::handleBrushStroke(const ecs::vec3& hitPoint, const ecs::vec3& hitNormal,
                                       EditorScene& scene, CommandStack& cmds) {
    (void)hitNormal;
    (void)scene;

    if (!m_sculptActive || !shouldEmitStroke(hitPoint)) {
        return false;
    }

    m_lastStrokePos = hitPoint;
    m_strokeOpen = true;
    ++m_strokeCount;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "sdf_sculpt.stroke";
    command.propertyValue = std::to_string(hitPoint.x) + "," + std::to_string(hitPoint.y) + "," +
                          std::to_string(hitPoint.z);
    cmds.execute(std::move(command));

    if (m_brush.symmetryX) {
        const ecs::vec3 mirrored = mirrorHitPoint(hitPoint);
        EditorCommand mirroredCommand;
        mirroredCommand.kind = CommandKind::SetProperty;
        mirroredCommand.propertyName = "sdf_sculpt.stroke.mirror";
        mirroredCommand.propertyValue = std::to_string(mirrored.x) + "," +
                                        std::to_string(mirrored.y) + "," +
                                        std::to_string(mirrored.z);
        cmds.execute(std::move(mirroredCommand));
        ++m_strokeCount;
    }

    return true;
}

} // namespace fuse::editor

namespace fuse::editor {

bool SdfSculptPanel::applyBrushStroke(const ecs::vec3& hitPoint, const ecs::vec3& hitNormal,
                                      EditorScene& scene, UndoStack& undo) {
    (void)hitNormal;

    if (!m_sculptActive || m_brush.op != BrushOp::Add || !shouldEmitStroke(hitPoint)) {
        return false;
    }

    m_lastStrokePos = hitPoint;
    m_strokeOpen = true;

    const auto makePrimitive = [this](const ecs::vec3& position) {
        EntityComponentSet components{};
        ecs::Transform transform{};
        transform.position = {position.x, position.y, position.z, 1.f};
        std::get<std::optional<ecs::Transform>>(components) = transform;

        ecs::SDFObject sdf{};
        sdf.type = m_brush.shape;
        sdf.params = {m_brush.radius, 0.f, 0.f, 0.f};
        sdf.material_id = m_brush.materialId;
        sdf.blend_alpha = m_brush.blendAlpha;
        std::get<std::optional<ecs::SDFObject>>(components) = sdf;
        return components;
    };

    constexpr f32 kMirrorPlaneEpsilon = 1e-6f;
    const bool mirror = m_brush.symmetryX && std::fabs(hitPoint.x) > kMirrorPlaneEpsilon;

    undo.beginMacro("Sculpt stroke");
    undo.execute(std::make_unique<CreateEntityCommand>(scene.registry(), makePrimitive(hitPoint),
                                                       "Add SDF primitive"));
    ++m_strokeCount;
    if (mirror) {
        undo.execute(std::make_unique<CreateEntityCommand>(
            scene.registry(), makePrimitive(mirrorHitPoint(hitPoint)), "Add SDF primitive (mirror)"));
        ++m_strokeCount;
    }
    undo.endMacro();
    return true;
}

} // namespace fuse::editor
