#include <fuse/animation/blend_tree.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::animation {

namespace {

void blend_poses(const Pose& a, const Pose& b, f32 weight, Pose& out) {
    const f32 clamped = std::clamp(weight, 0.f, 1.f);
    out.bone_count = std::max(a.bone_count, b.bone_count);
    const u32 count =
        static_cast<u32>(std::max(a.bone_world_transforms.size(), b.bone_world_transforms.size()));
    out.bone_world_transforms.resize(count, mat4::identity());

    for (u32 i = 0; i < count; ++i) {
        const mat4 from = (i < a.bone_world_transforms.size()) ? a.bone_world_transforms[i]
                                                               : mat4::identity();
        const mat4 to = (i < b.bone_world_transforms.size()) ? b.bone_world_transforms[i]
                                                             : mat4::identity();
        const vec3 fromPos = mat4_translation(from);
        const vec3 toPos = mat4_translation(to);
        const vec3 blended = lerp(fromPos, toPos, clamped);

        out.bone_world_transforms[i] = from;
        out.bone_world_transforms[i].data[12] = blended.x;
        out.bone_world_transforms[i].data[13] = blended.y;
        out.bone_world_transforms[i].data[14] = blended.z;
    }
}

PoseSoA pose_to_soa(const Pose& pose, const Skeleton& skel) {
    PoseSoA soa = PoseSoA::from_bind_pose(skel);
    for (u32 i = 0; i < pose.bone_world_transforms.size() && i < soa.bone_count; ++i) {
        decompose_trs(pose.bone_world_transforms[i],
                      soa.local_positions[i],
                      soa.local_rotations[i],
                      soa.local_scales[i]);
    }
    soa.compute_world_transforms(skel);
    return soa;
}

void find_blend_space_1d_bracket(const BlendSpace1D& space,
                                 f32 value,
                                 u32& out_lower,
                                 u32& out_upper,
                                 f32& out_alpha) {
    if (space.entries.empty()) {
        out_lower = 0;
        out_upper = 0;
        out_alpha = 0.f;
        return;
    }

    out_lower = 0;
    out_upper = 0;
    for (u32 i = 0; i < space.entries.size(); ++i) {
        if (space.entries[i].param_value <= value &&
            space.entries[i].param_value >= space.entries[out_lower].param_value) {
            out_lower = i;
        }
    }

    out_upper = out_lower;
    for (u32 i = 0; i < space.entries.size(); ++i) {
        if (space.entries[i].param_value >= value) {
            out_upper = i;
            break;
        }
    }

    if (space.entries[out_upper].param_value < value) {
        out_upper = out_lower;
    }

    if (out_lower == out_upper) {
        out_alpha = 0.f;
        return;
    }

    const f32 lowerValue = space.entries[out_lower].param_value;
    const f32 upperValue = space.entries[out_upper].param_value;
    if (std::fabs(upperValue - lowerValue) < 1e-5f) {
        out_alpha = 0.f;
        return;
    }

    out_alpha = std::clamp((value - lowerValue) / (upperValue - lowerValue), 0.f, 1.f);
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

void BlendSpace1D::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (entries.empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    const f32 value = param ? *param : 0.f;
    u32 lower = 0;
    u32 upper = 0;
    f32 alpha = 0.f;
    find_blend_space_1d_bracket(*this, value, lower, upper, alpha);

    Pose poseA = Pose::make_bind_pose(skel);
    Pose poseB = Pose::make_bind_pose(skel);
    if (entries[lower].clip) {
        entries[lower].clip->evaluate(dt, skel, poseA);
    }
    if (entries[upper].clip && upper != lower) {
        entries[upper].clip->evaluate(dt, skel, poseB);
    } else {
        poseB = poseA;
    }

    blend_poses(poseA, poseB, alpha, out);
}

void BlendSpace2D::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (entries.empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    const vec2 value = param ? *param : vec2{};
    PoseSoA result = PoseSoA::from_bind_pose(skel);
    f32 totalWeight = 0.f;
    bool hasPose = false;

    for (const Entry& entry : entries) {
        if (!entry.clip) {
            continue;
        }

        const f32 dx = value.x - entry.param.x;
        const f32 dy = value.y - entry.param.y;
        const f32 distanceSq = dx * dx + dy * dy;
        const f32 weight = 1.f / (distanceSq + 1e-4f);

        Pose entryPose = Pose::make_bind_pose(skel);
        entry.clip->evaluate(dt, skel, entryPose);
        PoseSoA entrySoa = pose_to_soa(entryPose, skel);

        if (!hasPose) {
            result = entrySoa;
            totalWeight = weight;
            hasPose = true;
            continue;
        }

        PoseSoA blended = PoseSoA::allocate(result.bone_count);
        blend_pose_soa(result, entrySoa, weight / (totalWeight + weight), blended);
        result = blended;
        totalWeight += weight;
    }

    if (!hasPose) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    result.compute_world_transforms(skel);
    out = result.to_pose();
}

void LayeredBlendNode::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    Pose basePose = Pose::make_bind_pose(skel);
    Pose layerPose = Pose::make_bind_pose(skel);

    if (base) {
        base->evaluate(dt, skel, basePose);
    }
    if (layer) {
        layer->evaluate(dt, skel, layerPose);
    }

    PoseSoA baseSoa = pose_to_soa(basePose, skel);
    PoseSoA layerSoa = pose_to_soa(layerPose, skel);
    PoseSoA result = baseSoa;

    const f32 weight = std::clamp(layer_weight, 0.f, 1.f);
    for (u32 boneIndex : masked_bones) {
        if (boneIndex >= result.bone_count) {
            continue;
        }
        result.local_positions[boneIndex] =
            lerp(baseSoa.local_positions[boneIndex], layerSoa.local_positions[boneIndex], weight);
        result.local_rotations[boneIndex] =
            lerp(baseSoa.local_rotations[boneIndex], layerSoa.local_rotations[boneIndex], weight);
        result.local_scales[boneIndex] =
            lerp(baseSoa.local_scales[boneIndex], layerSoa.local_scales[boneIndex], weight);
    }

    result.compute_world_transforms(skel);
    out = result.to_pose();
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
