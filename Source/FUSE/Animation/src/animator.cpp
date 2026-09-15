#include <fuse/animation/animator.hpp>

namespace fuse::animation {

void Animator::tick(const Skeleton& skel, const frame::FrameCtx& ctx) {
    if (paused) {
        return;
    }

    const f32 dt = (ctx.dt > 0.f) ? ctx.dt * playback_rate : 0.f;
    if (state_machine) {
        state_machine->evaluate(dt, skel, current_pose);
    } else {
        current_pose = Pose::make_bind_pose(skel);
    }

    ++tick_count;
}

} // namespace fuse::animation
