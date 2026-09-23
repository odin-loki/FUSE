#pragma once

#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::editor {

/// Headless SDF sculpt panel stub (B6.8).
class SdfSculptPanel {
public:
    enum class BrushOp : u8 { Add, Subtract, Smooth, Roughen, Paint };

    struct BrushState {
        ecs::SDFPrimitive shape = ecs::SDFPrimitive::Sphere;
        BrushOp op = BrushOp::Add;
        f32 radius = 1.f;
        f32 strength = 1.f;
        f32 blendAlpha = ecs::kSdfDefaultBlendAlpha;
        u32 materialId = 0;
        bool symmetryX = false;
    };

    void sync(const EditorState& state, EditorScene& scene);

    [[nodiscard]] const BrushState& brush() const { return m_brush; }
    [[nodiscard]] bool sculptActive() const { return m_sculptActive; }
    [[nodiscard]] u32 strokeCount() const { return m_strokeCount; }

    void setBrushRadius(f32 radius);
    void setBrushOperation(BrushOp op);
    void setBlendAlpha(f32 alpha);
    void setBrushShape(ecs::SDFPrimitive shape) { m_brush.shape = shape; }
    void setMaterialId(u32 materialId) { m_brush.materialId = materialId; }
    void setSymmetryX(bool enabled) { m_brush.symmetryX = enabled; }
    /// Minimum world distance between emitted stroke samples within one drag.
    void setStrokeSpacing(f32 spacing) { m_strokeSpacing = spacing > 0.f ? spacing : 0.f; }
    [[nodiscard]] f32 strokeSpacing() const { return m_strokeSpacing; }
    /// Ends the current drag: the next sample always emits, wherever it lands.
    void endStroke() { m_strokeOpen = false; }

    bool handleBrushStroke(const ecs::vec3& hitPoint, const ecs::vec3& hitNormal, EditorScene& scene,
                           CommandStack& cmds);

    /// Applies an Add-brush sample to the ECS scene: spawns an `SDFObject` primitive (brush shape,
    /// radius in `params.x`, blend α, material) with a Transform at `hitPoint`, plus its X=0 mirror
    /// when symmetry is on, as one undoable step. Other brush ops need an SDF CSG op on
    /// `ecs::SDFObject` (not modelled yet) and return false. Stroke spacing applies as above.
    bool applyBrushStroke(const ecs::vec3& hitPoint, const ecs::vec3& hitNormal, EditorScene& scene,
                          UndoStack& undo);

private:
    bool shouldEmitStroke(const ecs::vec3& hitPoint) const;
    ecs::vec3 mirrorHitPoint(const ecs::vec3& hitPoint) const;

    BrushState m_brush{};
    bool m_sculptActive = false;
    bool m_strokeOpen = false;
    ecs::vec3 m_lastStrokePos{};
    f32 m_strokeSpacing = 0.5f;
    u32 m_strokeCount = 0;
};

} // namespace fuse::editor
