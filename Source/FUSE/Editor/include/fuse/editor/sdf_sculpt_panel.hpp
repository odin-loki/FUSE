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

    /// Applies one brush sample to the ECS scene as one undoable step (plus its X=0 mirror when
    /// symmetry is on). Brush ops map onto the SDF CSG op on `ecs::SDFObject`
    /// (`fuse/ecs/sdf_csg.hpp`), each new primitive taking the next `csg_order`:
    ///  - Add: spawn a Union primitive (brush shape, radius in `params.x`, α, material) at `hitPoint`.
    ///  - Subtract: spawn a Subtract primitive that carves its volume out of everything before it.
    ///  - Smooth: spawn a SmoothUnion primitive with blend radius `blendAlpha * radius`.
    ///  - Roughen: raise `roughness` by `strength * kRoughenStep * radius` (capped at
    ///    `kMaxRoughness * radius`) on every object whose surface lies within the brush radius.
    ///  - Paint: set `material_id` to the brush material on every object whose surface lies within
    ///    the brush radius.
    /// Roughen/Paint touching nothing return false (no undo step). Stroke spacing applies as above.
    static constexpr f32 kRoughenStep = 0.05f;
    static constexpr f32 kMaxRoughness = 0.25f;
    bool applyBrushStroke(const ecs::vec3& hitPoint, const ecs::vec3& hitNormal, EditorScene& scene,
                          UndoStack& undo);

private:
    bool shouldEmitStroke(const ecs::vec3& hitPoint) const;
    ecs::vec3 mirrorHitPoint(const ecs::vec3& hitPoint) const;
    bool applySpawnBrush(const ecs::vec3& hitPoint, bool mirror, EditorScene& scene, UndoStack& undo);
    bool applyEditBrush(const ecs::vec3& hitPoint, bool mirror, EditorScene& scene, UndoStack& undo);

    BrushState m_brush{};
    bool m_sculptActive = false;
    bool m_strokeOpen = false;
    ecs::vec3 m_lastStrokePos{};
    f32 m_strokeSpacing = 0.5f;
    u32 m_strokeCount = 0;
};

} // namespace fuse::editor
