#pragma once

#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/animation/skinning.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <vector>

namespace fuse::animation {

class SkeletonAsset : public fuse::Object {
public:
    Skeleton skeleton;

    const char* typeName() const override { return "SkeletonAsset"; }
};

struct Animator {
    static constexpr const char* component_name = "Animator";

    Handle<SkeletonAsset> skeleton;
    std::unique_ptr<AnimStateMachine> state_machine;
    Pose current_pose;

    f32 speed = 0.f;
    f32 direction = 0.f;
    bool is_grounded = true;
    bool is_attacking = false;

    f32 playback_rate = 1.f;
    bool paused = false;

    u32 tick_count = 0;

    /// Bone buffer contents rebuilt every tick: world * inverse_bind per bone (see
    /// `compute_skinning_palette`). Storage is reused across ticks; GPU upload is the renderer's job.
    std::vector<mat4> bone_palette;

    void tick(const Skeleton& skel, const frame::FrameCtx& ctx = {});
};

} // namespace fuse::animation
