#include <fuse/editor/sdf_sculpt_panel.hpp>

#include <fuse/ecs/sdf_csg.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

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

    if (!m_sculptActive || !shouldEmitStroke(hitPoint)) {
        return false;
    }

    constexpr f32 kMirrorPlaneEpsilon = 1e-6f;
    const bool mirror = m_brush.symmetryX && std::fabs(hitPoint.x) > kMirrorPlaneEpsilon;

    const bool applied = (m_brush.op == BrushOp::Roughen || m_brush.op == BrushOp::Paint)
                             ? applyEditBrush(hitPoint, mirror, scene, undo)
                             : applySpawnBrush(hitPoint, mirror, scene, undo);
    if (applied) {
        m_lastStrokePos = hitPoint;
        m_strokeOpen = true;
    }
    return applied;
}

bool SdfSculptPanel::applySpawnBrush(const ecs::vec3& hitPoint, bool mirror, EditorScene& scene,
                                     UndoStack& undo) {
    ecs::SdfCsgScene csg;
    csg.build(scene.registry());
    u32 order = csg.next_csg_order();

    ecs::SDFCsgOp op = ecs::SDFCsgOp::Union;
    f32 blendRadius = 0.f;
    const char* label = "Add SDF primitive";
    if (m_brush.op == BrushOp::Subtract) {
        op = ecs::SDFCsgOp::Subtract;
        label = "Subtract SDF primitive";
    } else if (m_brush.op == BrushOp::Smooth) {
        op = ecs::SDFCsgOp::SmoothUnion;
        blendRadius = m_brush.blendAlpha * m_brush.radius;
        label = "Smooth SDF primitive";
    }

    const auto makePrimitive = [&](const ecs::vec3& position) {
        EntityComponentSet components{};
        ecs::Transform transform{};
        transform.position = {position.x, position.y, position.z, 1.f};
        std::get<std::optional<ecs::Transform>>(components) = transform;

        ecs::SDFObject sdf{};
        sdf.type = m_brush.shape;
        sdf.op = op;
        sdf.params = {m_brush.radius, 0.f, 0.f, 0.f};
        sdf.material_id = m_brush.materialId;
        sdf.blend_alpha = m_brush.blendAlpha;
        sdf.blend_radius = blendRadius;
        sdf.csg_order = order++;
        std::get<std::optional<ecs::SDFObject>>(components) = sdf;
        return components;
    };

    undo.beginMacro("Sculpt stroke");
    undo.execute(std::make_unique<CreateEntityCommand>(scene.registry(), makePrimitive(hitPoint), label));
    ++m_strokeCount;
    if (mirror) {
        undo.execute(std::make_unique<CreateEntityCommand>(
            scene.registry(), makePrimitive(mirrorHitPoint(hitPoint)), std::string(label) + " (mirror)"));
        ++m_strokeCount;
    }
    undo.endMacro();
    return true;
}

bool SdfSculptPanel::applyEditBrush(const ecs::vec3& hitPoint, bool mirror, EditorScene& scene,
                                    UndoStack& undo) {
    ecs::Registry& registry = scene.registry();
    const ecs::vec3 samples[2] = {hitPoint, mirrorHitPoint(hitPoint)};
    const usize sampleCount = mirror ? 2u : 1u;

    // One edit per touched entity, even when the stroke and its mirror both reach it.
    std::vector<std::pair<ecs::EntityID, ecs::SDFObject>> touched;
    registry.each<ecs::SDFObject, ecs::Transform>(
        [&](ecs::EntityID id, ecs::SDFObject& sdf, ecs::Transform& transform) {
            if (!sdf.visible) {
                return;
            }
            for (usize i = 0; i < sampleCount; ++i) {
                if (std::fabs(ecs::sdf_object_distance(sdf, transform, samples[i])) <= m_brush.radius) {
                    touched.emplace_back(id, sdf);
                    return;
                }
            }
        });

    const bool paint = m_brush.op == BrushOp::Paint;
    std::vector<std::unique_ptr<UndoCommand>> edits;
    for (const auto& [id, before] : touched) {
        ecs::SDFObject after = before;
        if (paint) {
            after.material_id = m_brush.materialId;
        } else {
            const f32 cap = kMaxRoughness * m_brush.radius;
            after.roughness =
                std::min(cap, before.roughness + m_brush.strength * kRoughenStep * m_brush.radius);
        }
        if (after.material_id != before.material_id || after.roughness != before.roughness) {
            edits.push_back(std::make_unique<SdfObjectEditCommand>(
                registry, id, before, after, paint ? "Paint SDF object" : "Roughen SDF object"));
        }
    }
    if (edits.empty()) {
        return false;
    }

    undo.beginMacro(paint ? "Paint stroke" : "Roughen stroke");
    for (auto& edit : edits) {
        undo.execute(std::move(edit));
    }
    undo.endMacro();
    m_strokeCount += static_cast<u32>(sampleCount);
    return true;
}

} // namespace fuse::editor
