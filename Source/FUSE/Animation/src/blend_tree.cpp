#include <fuse/animation/blend_tree.hpp>

#include <cmath>

namespace fuse::animation {

namespace {

void blend_poses(const Pose& a, const Pose& b, f32 weight, Pose& out) {
    const f32 clamped = std::clamp(weight, 0.f, 1.f);
    out.bone_count = std::max(a.bone_count, b.bone_count);
    const u32 count = static_cast<u32>(std::max(a.bone_world_transforms.size(), b.bone_world_transforms.size()));
    out.bone_world_transforms.resize(count, mat4::identity());

    for (u32 i = 0; i < count; ++i) {
        const mat4 from = (i < a.bone_world_transforms.size()) ? a.bone_world_transforms[i] : mat4::identity();
        const mat4 to = (i < b.bone_world_transforms.size()) ? b.bone_world_transforms[i] : mat4::identity();
        const vec3 fromPos = mat4_translation(from);
        const vec3 toPos = mat4_translation(to);
        const vec3 blended = lerp(fromPos, toPos, clamped);

        out.bone_world_transforms[i] = from;
        out.bone_world_transforms[i].data[12] = blended.x;
        out.bone_world_transforms[i].data[13] = blended.y;
        out.bone_world_transforms[i].data[14] = blended.z;
    }
}

} // namespace

void ClipNode::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (clip == nullptr) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    time += dt * play_rate;
    if (looping && clip->duration > 0.f) {
        time = std::fmod(time, clip->duration);
        if (time < 0.f) {
            time += clip->duration;
        }
    } else {
        time = std::min(time, clip->duration);
    }

    clip->sample(time, skel, out);
}

void BlendNode2::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    Pose poseA = Pose::make_bind_pose(skel);
    Pose poseB = Pose::make_bind_pose(skel);

    if (a) {
        a->evaluate(dt, skel, poseA);
    }
    if (b) {
        b->evaluate(dt, skel, poseB);
    }

    const f32 weight = blend_param ? std::clamp(*blend_param, 0.f, 1.f) : 0.f;
    blend_poses(poseA, poseB, weight, out);
}

void AnimStateMachine::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (states.empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    if (active_state >= states.size()) {
        active_state = 0;
    }

    Pose activePose = Pose::make_bind_pose(skel);
    if (states[active_state].node) {
        states[active_state].node->evaluate(dt, skel, activePose);
    }

    for (const Transition& transition : transitions) {
        if (transition.from != active_state || !transition.condition || !transition.condition()) {
            continue;
        }
        if (transition.to >= states.size()) {
            break;
        }

        if (blend_time <= 0.f) {
            blend_from_pose = activePose;
            blend_duration = transition.blend_duration;
        }

        blend_time += dt;
        Pose targetPose = Pose::make_bind_pose(skel);
        if (states[transition.to].node) {
            states[transition.to].node->evaluate(dt, skel, targetPose);
        }

        const f32 alpha = (blend_duration > 0.f) ? std::min(blend_time / blend_duration, 1.f) : 1.f;
        blend_poses(blend_from_pose, targetPose, alpha, out);

        if (alpha >= 1.f) {
            active_state = transition.to;
            blend_time = 0.f;
        }
        return;
    }

    out = activePose;
}

void AnimStateMachine::add_state(std::string name, std::unique_ptr<BlendNode> node) {
    states.push_back(State{std::move(name), std::move(node)});
}

void AnimStateMachine::add_transition(const char* from,
                                      const char* to,
                                      f32 duration,
                                      std::function<bool()> condition) {
    s32 fromIndex = -1;
    s32 toIndex = -1;
    for (u32 i = 0; i < states.size(); ++i) {
        if (from != nullptr && states[i].name == from) {
            fromIndex = static_cast<s32>(i);
        }
        if (to != nullptr && states[i].name == to) {
            toIndex = static_cast<s32>(i);
        }
    }

    if (fromIndex < 0 || toIndex < 0) {
        return;
    }

    transitions.push_back(
        Transition{static_cast<u32>(fromIndex), static_cast<u32>(toIndex), duration, std::move(condition)});
}

} // namespace fuse::animation
