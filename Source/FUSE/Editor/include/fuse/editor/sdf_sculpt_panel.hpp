#pragma once

#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
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

    bool handleBrushStroke(const ecs::vec3& hitPoint, const ecs::vec3& hitNormal, EditorScene& scene,
                           CommandStack& cmds);

private:
    bool shouldEmitStroke(const ecs::vec3& hitPoint) const;
    ecs::vec3 mirrorHitPoint(const ecs::vec3& hitPoint) const;

    BrushState m_brush{};
    bool m_sculptActive = false;
    ecs::vec3 m_lastStrokePos{};
    f32 m_strokeSpacing = 0.5f;
    u32 m_strokeCount = 0;
};

} // namespace fuse::editor
