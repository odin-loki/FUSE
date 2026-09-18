#include <fuse/animation/blend_tree.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::animation {

namespace {

bool skeleton_has_bones(const Skeleton& skel) {
    return skel.bone_count > 0 && !skel.bones.empty();
}

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

void blend_poses_soa(const PoseSoA& a, const PoseSoA& b, f32 weight, PoseSoA& out) {
    blend_pose_soa(a, b, weight, out);
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

void BlendNode::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (!skeleton_has_bones(skel)) {
        out.clear();
        return;
    }

    Pose pose = Pose::make_bind_pose(skel);
    evaluate(dt, skel, pose);
    out = pose_to_soa(pose, skel);
    ensure_pose_soa_bind_fallback(out, skel);
}

BlendSpace1DSample sample_blend_space_1d(const BlendSpace1D& space, f32 value) {
    BlendSpace1DSample sample{};
    find_blend_space_1d_bracket(space, value, sample.lower_index, sample.upper_index, sample.alpha);
    return sample;
}

BlendSpace2DSample sample_blend_space_2d(const BlendSpace2D& space, vec2 value) {
    BlendSpace2DSample sample{};
    sample.weights.resize(space.entries.size(), 0.f);

    if (space.entries.empty()) {
        return sample;
    }

    f32 totalWeight = 0.f;
    for (u32 i = 0; i < space.entries.size(); ++i) {
        const f32 dx = value.x - space.entries[i].param.x;
        const f32 dy = value.y - space.entries[i].param.y;
        const f32 distanceSq = dx * dx + dy * dy;
        const f32 weight = 1.f / (distanceSq + 1e-4f);
        sample.weights[i] = weight;
        totalWeight += weight;
    }

    if (totalWeight > 0.f) {
        for (f32& weight : sample.weights) {
            weight /= totalWeight;
        }
    }

    return sample;
}

bool ClipNode::is_empty() const {
    return clip == nullptr;
}

void ClipNode::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (!skeleton_has_bones(skel) || clip == nullptr) {
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

void ClipNode::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (!skeleton_has_bones(skel)) {
        out.clear();
        return;
    }

    if (clip == nullptr) {
        out = PoseSoA::from_bind_pose(skel);
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

    clip->evaluate(time, skel, out);
}

bool BlendNode2::is_empty() const {
    return !a && !b;
}

bool BlendSpace1D::is_empty() const {
    return entries.empty();
}

bool BlendSpace2D::is_empty() const {
    return entries.empty();
}

bool LayeredBlendNode::is_empty() const {
    return !base && !layer;
}

bool AdditiveBlendNode::is_empty() const {
    return !base && !layer;
}

bool AnimStateMachine::is_empty() const {
    return states.empty();
}

void BlendNode2::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (!skeleton_has_bones(skel)) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    if (is_empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

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

void BlendNode2::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (!skeleton_has_bones(skel)) {
        out.clear();
        return;
    }

    if (is_empty()) {
        out = PoseSoA::from_bind_pose(skel);
        return;
    }

    PoseSoA poseA = PoseSoA::from_bind_pose(skel);
    PoseSoA poseB = PoseSoA::from_bind_pose(skel);

    if (a) {
        a->evaluate_soa(dt, skel, poseA);
        ensure_pose_soa_bind_fallback(poseA, skel);
    }
    if (b) {
        b->evaluate_soa(dt, skel, poseB);
        ensure_pose_soa_bind_fallback(poseB, skel);
    }

    const f32 weight = blend_param ? std::clamp(*blend_param, 0.f, 1.f) : 0.f;
    blend_poses_soa(poseA, poseB, weight, out);
    ensure_pose_soa_bind_fallback(out, skel);
    out.compute_world_transforms(skel);
}

void BlendSpace1D::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (!skeleton_has_bones(skel)) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    if (is_empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    const f32 value = param ? *param : 0.f;
    const BlendSpace1DSample sample = sample_blend_space_1d(*this, value);

    Pose poseA = Pose::make_bind_pose(skel);
    Pose poseB = Pose::make_bind_pose(skel);
    if (entries[sample.lower_index].clip) {
        entries[sample.lower_index].clip->evaluate(dt, skel, poseA);
    }
    if (entries[sample.upper_index].clip && sample.upper_index != sample.lower_index) {
        entries[sample.upper_index].clip->evaluate(dt, skel, poseB);
    } else {
        poseB = poseA;
    }

    blend_poses(poseA, poseB, sample.alpha, out);
}

void BlendSpace1D::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (!skeleton_has_bones(skel)) {
        out.clear();
        return;
    }

    if (is_empty()) {
        out = PoseSoA::from_bind_pose(skel);
        return;
    }

    const f32 value = param ? *param : 0.f;
    const BlendSpace1DSample sample = sample_blend_space_1d(*this, value);

    PoseSoA poseA = PoseSoA::from_bind_pose(skel);
    PoseSoA poseB = PoseSoA::from_bind_pose(skel);
    if (entries[sample.lower_index].clip) {
        entries[sample.lower_index].clip->evaluate_soa(dt, skel, poseA);
        ensure_pose_soa_bind_fallback(poseA, skel);
    }
    if (entries[sample.upper_index].clip && sample.upper_index != sample.lower_index) {
        entries[sample.upper_index].clip->evaluate_soa(dt, skel, poseB);
        ensure_pose_soa_bind_fallback(poseB, skel);
    } else {
        poseB = poseA;
    }

    blend_poses_soa(poseA, poseB, sample.alpha, out);
    ensure_pose_soa_bind_fallback(out, skel);
    out.compute_world_transforms(skel);
}

void BlendSpace2D::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    PoseSoA soa = PoseSoA::from_bind_pose(skel);
    evaluate_soa(dt, skel, soa);
    out = soa.to_pose();
}

void BlendSpace2D::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (!skeleton_has_bones(skel)) {
        out.clear();
        return;
    }

    if (is_empty()) {
        out = PoseSoA::from_bind_pose(skel);
        return;
    }

    const vec2 value = param ? *param : vec2{};
    const BlendSpace2DSample sample = sample_blend_space_2d(*this, value);
    PoseSoA result = PoseSoA::from_bind_pose(skel);
    f32 accumulatedWeight = 0.f;

    for (u32 i = 0; i < entries.size(); ++i) {
        if (!entries[i].clip || sample.weights[i] <= 0.f) {
            continue;
        }

        PoseSoA entrySoa = PoseSoA::from_bind_pose(skel);
        entries[i].clip->evaluate_soa(dt, skel, entrySoa);
        ensure_pose_soa_bind_fallback(entrySoa, skel);
        accumulate_weighted_pose_soa(result, accumulatedWeight, entrySoa, sample.weights[i]);
    }

    finalize_weighted_pose_soa(result, accumulatedWeight, skel, out);
    ensure_pose_soa_bind_fallback(out, skel);
}

void LayeredBlendNode::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (!skeleton_has_bones(skel)) {
        out.clear();
        return;
    }

    if (is_empty()) {
        out = PoseSoA::from_bind_pose(skel);
        return;
    }

    PoseSoA baseSoa = PoseSoA::from_bind_pose(skel);
    PoseSoA layerSoa = PoseSoA::from_bind_pose(skel);

    if (base) {
        base->evaluate_soa(dt, skel, baseSoa);
        ensure_pose_soa_bind_fallback(baseSoa, skel);
    }
    if (layer) {
        layer->evaluate_soa(dt, skel, layerSoa);
        ensure_pose_soa_bind_fallback(layerSoa, skel);
    }

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
    out = result;
}

void LayeredBlendNode::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    PoseSoA soa = PoseSoA::from_bind_pose(skel);
    evaluate_soa(dt, skel, soa);
    out = soa.to_pose();
}

void AdditiveBlendNode::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (!skeleton_has_bones(skel)) {
        out.clear();
        return;
    }

    if (is_empty()) {
        out = PoseSoA::from_bind_pose(skel);
        return;
    }

    PoseSoA baseSoa = PoseSoA::from_bind_pose(skel);
    PoseSoA layerSoa = PoseSoA::from_bind_pose(skel);

    if (base) {
        base->evaluate_soa(dt, skel, baseSoa);
        ensure_pose_soa_bind_fallback(baseSoa, skel);
    }
    if (layer) {
        layer->evaluate_soa(dt, skel, layerSoa);
        ensure_pose_soa_bind_fallback(layerSoa, skel);
    }

    const PoseSoA bindSoa = PoseSoA::from_bind_pose(skel);
    add_pose_soa(baseSoa, layerSoa, bindSoa, layer_weight, masked_bones, out);
    ensure_pose_soa_bind_fallback(out, skel);
    out.compute_world_transforms(skel);
}

void AdditiveBlendNode::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    PoseSoA soa = PoseSoA::from_bind_pose(skel);
    evaluate_soa(dt, skel, soa);
    out = soa.to_pose();
}

f32 AnimStateMachine::crossfade_alpha() const {
    if (!is_transitioning) {
        return 0.f;
    }
    if (blend_duration <= 0.f) {
        return 1.f;
    }
    return std::clamp(blend_time / blend_duration, 0.f, 1.f);
}

void AnimStateMachine::reset() {
    active_state = 0;
    pending_state = 0;
    is_transitioning = false;
    blend_time = 0.f;
    blend_duration = 0.2f;
    has_entered_initial = false;
}

s32 AnimStateMachine::find_state_index(const char* name) const {
    if (name == nullptr) {
        return -1;
    }

    for (u32 i = 0; i < states.size(); ++i) {
        if (states[i].name == name) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

u32 AnimStateMachine::outgoing_transition_count(u32 from_state) const {
    u32 count = 0;
    for (const Transition& transition : transitions) {
        if (transition.from == from_state) {
            ++count;
        }
    }
    return count;
}

bool AnimStateMachine::has_transition(u32 from_state, u32 to_state) const {
    for (const Transition& transition : transitions) {
        if (transition.from == from_state && transition.to == to_state) {
            return true;
        }
    }
    return false;
}

u32 AnimStateMachine::incoming_transition_count(u32 to_state) const {
    u32 count = 0;
    for (const Transition& transition : transitions) {
        if (transition.to == to_state) {
            ++count;
        }
    }
    return count;
}

f32 AnimStateMachine::transition_blend_duration(u32 from_state, u32 to_state) const {
    for (const Transition& transition : transitions) {
        if (transition.from == from_state && transition.to == to_state) {
            return transition.blend_duration;
        }
    }
    return -1.f;
}

const char* AnimStateMachine::state_name(u32 state_index) const {
    if (state_index >= states.size()) {
        return "";
    }
    return states[state_index].name.c_str();
}

const char* AnimStateMachine::active_state_name() const {
    return state_name(active_state);
}

const char* AnimStateMachine::pending_state_name() const {
    if (!is_transitioning) {
        return "";
    }
    return state_name(pending_state);
}

u32 AnimStateMachine::state_count() const {
    return static_cast<u32>(states.size());
}

bool AnimStateMachine::is_valid_state(u32 state_index) const {
    return state_index < states.size();
}

u32 AnimStateMachine::transition_count() const {
    return static_cast<u32>(transitions.size());
}

bool AnimStateMachine::has_pending_transition() const {
    return is_transitioning;
}

s32 AnimStateMachine::outgoing_transition_to(u32 from_state, u32 edge_index) const {
    u32 seen = 0;
    for (const Transition& transition : transitions) {
        if (transition.from != from_state) {
            continue;
        }
        if (seen == edge_index) {
            return static_cast<s32>(transition.to);
        }
        ++seen;
    }
    return -1;
}

s32 AnimStateMachine::incoming_transition_from(u32 to_state, u32 edge_index) const {
    u32 seen = 0;
    for (const Transition& transition : transitions) {
        if (transition.to != to_state) {
            continue;
        }
        if (seen == edge_index) {
            return static_cast<s32>(transition.from);
        }
        ++seen;
    }
    return -1;
}

f32 AnimStateMachine::outgoing_transition_blend_duration_at(u32 from_state, u32 edge_index) const {
    u32 seen = 0;
    for (const Transition& transition : transitions) {
        if (transition.from != from_state) {
            continue;
        }
        if (seen == edge_index) {
            return transition.blend_duration;
        }
        ++seen;
    }
    return -1.f;
}

bool AnimStateMachine::has_state_node(u32 state_index) const {
    if (!is_valid_state(state_index)) {
        return false;
    }
    return states[state_index].node != nullptr;
}

s32 AnimStateMachine::find_transition_index(u32 from_state, u32 to_state) const {
    for (u32 i = 0; i < transitions.size(); ++i) {
        if (transitions[i].from == from_state && transitions[i].to == to_state) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

bool AnimStateMachine::transition_condition_passes(u32 from_state, u32 to_state) const {
    const s32 index = find_transition_index(from_state, to_state);
    if (index < 0) {
        return false;
    }

    const Transition& transition = transitions[static_cast<u32>(index)];
    return !transition.condition || transition.condition();
}

bool AnimStateMachine::can_transition(const char* from, const char* to) const {
    const s32 fromIndex = find_state_index(from);
    const s32 toIndex = find_state_index(to);
    if (fromIndex < 0 || toIndex < 0) {
        return false;
    }
    return transition_condition_passes(static_cast<u32>(fromIndex), static_cast<u32>(toIndex));
}

s32 AnimStateMachine::find_named_transition_index(const char* from, const char* to) const {
    const s32 fromIndex = find_state_index(from);
    const s32 toIndex = find_state_index(to);
    if (fromIndex < 0 || toIndex < 0) {
        return -1;
    }
    return find_transition_index(static_cast<u32>(fromIndex), static_cast<u32>(toIndex));
}

bool AnimStateMachine::outgoing_transition_condition_passes(u32 from_state, u32 edge_index) const {
    const s32 toState = outgoing_transition_to(from_state, edge_index);
    if (toState < 0) {
        return false;
    }
    return transition_condition_passes(from_state, static_cast<u32>(toState));
}

s32 AnimStateMachine::find_first_passing_outgoing_transition(u32 from_state) const {
    for (u32 i = 0; i < transitions.size(); ++i) {
        const Transition& transition = transitions[i];
        if (transition.from != from_state || transition.to == from_state) {
            continue;
        }
        if (!transition.condition || transition.condition()) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

bool AnimStateMachine::has_passing_outgoing_transition(u32 from_state) const {
    return find_first_passing_outgoing_transition(from_state) >= 0;
}

void AnimStateMachine::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (!skeleton_has_bones(skel)) {
        out.clear();
        return;
    }

    if (is_empty()) {
        out = PoseSoA::from_bind_pose(skel);
        return;
    }

    if (active_state >= states.size()) {
        active_state = 0;
    }

    if (!has_entered_initial) {
        has_entered_initial = true;
        if (states[active_state].on_enter) {
            states[active_state].on_enter();
        }
    }

    if (is_transitioning) {
        blend_time += dt;
        PoseSoA targetSoa = PoseSoA::from_bind_pose(skel);
        if (pending_state < states.size() && states[pending_state].node) {
            states[pending_state].node->evaluate_soa(dt, skel, targetSoa);
            ensure_pose_soa_bind_fallback(targetSoa, skel);
        }

        const f32 alpha = crossfade_alpha();
        blend_poses_soa(blend_from_pose_soa, targetSoa, alpha, out);
        ensure_pose_soa_bind_fallback(out, skel);
        out.compute_world_transforms(skel);

        if (alpha >= 1.f) {
            active_state = pending_state;
            is_transitioning = false;
            blend_time = 0.f;
            if (active_state < states.size() && states[active_state].on_enter) {
                states[active_state].on_enter();
            }
        }
        return;
    }

    if (states[active_state].node) {
        states[active_state].node->evaluate_soa(dt, skel, out);
        ensure_pose_soa_bind_fallback(out, skel);
    } else {
        out = PoseSoA::from_bind_pose(skel);
    }

    for (const Transition& transition : transitions) {
        if (transition.from != active_state || transition.to == active_state) {
            continue;
        }
        if (!transition.condition || !transition.condition()) {
            continue;
        }
        if (transition.to >= states.size()) {
            break;
        }

        if (active_state < states.size() && states[active_state].on_exit) {
            states[active_state].on_exit();
        }

        blend_from_pose_soa = out;
        pending_state = transition.to;
        blend_duration = transition.blend_duration;
        blend_time = blend_duration <= 0.f ? blend_duration : dt;
        is_transitioning = true;

        PoseSoA targetSoa = PoseSoA::from_bind_pose(skel);
        if (states[pending_state].node) {
            states[pending_state].node->evaluate_soa(dt, skel, targetSoa);
            ensure_pose_soa_bind_fallback(targetSoa, skel);
        }

        const f32 alpha = crossfade_alpha();
        if (alpha >= 1.f) {
            active_state = pending_state;
            is_transitioning = false;
            blend_time = 0.f;
            out = targetSoa;
            ensure_pose_soa_bind_fallback(out, skel);
            out.compute_world_transforms(skel);
            if (states[active_state].on_enter) {
                states[active_state].on_enter();
            }
            return;
        }

        blend_poses_soa(blend_from_pose_soa, targetSoa, alpha, out);
        ensure_pose_soa_bind_fallback(out, skel);
        out.compute_world_transforms(skel);
        return;
    }
}

void AnimStateMachine::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (!skeleton_has_bones(skel)) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    if (is_empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    if (active_state >= states.size()) {
        active_state = 0;
    }

    if (!has_entered_initial) {
        has_entered_initial = true;
        if (states[active_state].on_enter) {
            states[active_state].on_enter();
        }
    }

    if (is_transitioning) {
        blend_time += dt;
        Pose targetPose = Pose::make_bind_pose(skel);
        if (pending_state < states.size() && states[pending_state].node) {
            states[pending_state].node->evaluate(dt, skel, targetPose);
        }

        const f32 alpha = crossfade_alpha();
        blend_poses(blend_from_pose, targetPose, alpha, out);

        if (alpha >= 1.f) {
            active_state = pending_state;
            is_transitioning = false;
            blend_time = 0.f;
            blend_from_pose = out;
            if (active_state < states.size() && states[active_state].on_enter) {
                states[active_state].on_enter();
            }
        }
        return;
    }

    Pose activePose = Pose::make_bind_pose(skel);
    if (states[active_state].node) {
        states[active_state].node->evaluate(dt, skel, activePose);
    }

    for (const Transition& transition : transitions) {
        if (transition.from != active_state || transition.to == active_state) {
            continue;
        }
        if (!transition.condition || !transition.condition()) {
            continue;
        }
        if (transition.to >= states.size()) {
            break;
        }

        if (active_state < states.size() && states[active_state].on_exit) {
            states[active_state].on_exit();
        }

        blend_from_pose = activePose;
        pending_state = transition.to;
        blend_duration = transition.blend_duration;
        blend_time = blend_duration <= 0.f ? blend_duration : dt;
        is_transitioning = true;

        Pose targetPose = Pose::make_bind_pose(skel);
        if (states[pending_state].node) {
            states[pending_state].node->evaluate(dt, skel, targetPose);
        }

        const f32 alpha = crossfade_alpha();
        if (alpha >= 1.f) {
            active_state = pending_state;
            is_transitioning = false;
            blend_time = 0.f;
            out = targetPose;
            if (states[active_state].on_enter) {
                states[active_state].on_enter();
            }
            return;
        }

        blend_poses(blend_from_pose, targetPose, alpha, out);
        return;
    }

    out = activePose;
}

void AnimStateMachine::add_state(std::string name, std::unique_ptr<BlendNode> node) {
    states.push_back(State{std::move(name), std::move(node), {}, {}});
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

    if (fromIndex < 0 || toIndex < 0 || fromIndex == toIndex) {
        return;
    }

    transitions.push_back(
        Transition{static_cast<u32>(fromIndex), static_cast<u32>(toIndex), duration, std::move(condition)});
}

} // namespace fuse::animation
