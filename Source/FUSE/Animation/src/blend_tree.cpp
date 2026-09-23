#include <fuse/animation/blend_tree.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::animation {

namespace {

void blend_poses_soa(const PoseSoA& a, const PoseSoA& b, f32 weight, PoseSoA& out) {
    blend_pose_soa(a, b, weight, out);
}

PoseSoA pose_to_soa(const Pose& pose, const Skeleton& skel) {
    // Pose stores world matrices; recover local TRS as inverse(parent_world) * world.
    PoseSoA soa = PoseSoA::from_bind_pose(skel);
    const u32 count = std::min(static_cast<u32>(pose.bone_world_transforms.size()), soa.bone_count);
    for (u32 i = 0; i < count; ++i) {
        const s32 parent = skel.bones[i].parent_index;
        mat4 local = pose.bone_world_transforms[i];
        if (parent >= 0 && static_cast<u32>(parent) < count) {
            local = mat4_multiply(mat4_inverse_affine(pose.bone_world_transforms[static_cast<u32>(parent)]), local);
        }
        decompose_trs(local, soa.local_positions[i], soa.local_rotations[i], soa.local_scales[i]);
    }
    soa.compute_world_transforms(skel);
    return soa;
}

/// AoS entry points share the SoA evaluation so blending happens on local TRS (slerp rotations,
/// lerp translation/scale) followed by forward kinematics.
template <typename Node>
void evaluate_via_soa(Node& node, f32 dt, const Skeleton& skel, Pose& out) {
    // Per-thread scratch keeps the per-frame AoS entry point heap-free once it has grown to the
    // skeleton; a nested call on the same thread (not expected) falls back to a local pose.
    thread_local PoseSoA t_scratch;
    thread_local bool t_busy = false;
    if (t_busy) {
        PoseSoA soa = PoseSoA::from_bind_pose(skel);
        node.evaluate_soa(dt, skel, soa);
        soa.to_pose(out);
        return;
    }
    t_busy = true;
    t_scratch.assign_bind_pose(skel);
    node.evaluate_soa(dt, skel, t_scratch);
    t_scratch.to_pose(out);
    t_busy = false;
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
    if (is_empty()) {
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
    if (is_empty()) {
        out.assign_bind_pose(skel);
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
    ensure_pose_soa_bind_fallback(out, skel);
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
    evaluate_via_soa(*this, dt, skel, out);
}

void BlendNode2::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (is_empty()) {
        out.assign_bind_pose(skel);
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
    evaluate_via_soa(*this, dt, skel, out);
}

void BlendSpace1D::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (is_empty()) {
        out.assign_bind_pose(skel);
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
    if (is_empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    PoseSoA soa = PoseSoA::from_bind_pose(skel);
    evaluate_soa(dt, skel, soa);
    out = soa.to_pose();
}

void BlendSpace2D::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (is_empty()) {
        out.assign_bind_pose(skel);
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
    if (is_empty()) {
        out.assign_bind_pose(skel);
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
    ensure_pose_soa_bind_fallback(out, skel);
}

void LayeredBlendNode::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    if (is_empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

    PoseSoA soa = PoseSoA::from_bind_pose(skel);
    evaluate_soa(dt, skel, soa);
    out = soa.to_pose();
}

void AdditiveBlendNode::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (is_empty()) {
        out.assign_bind_pose(skel);
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
    if (is_empty()) {
        out = Pose::make_bind_pose(skel);
        return;
    }

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
    // blend_time is a running sum of per-frame dt, which drifts a few ulps below the exact elapsed
    // time (e.g. 60 x (1/60.f) < 1.f); without slack the crossfade would overrun by a whole frame.
    constexpr f32 kCompletionSlack = 1e-4f;
    const f32 alpha = blend_time / blend_duration;
    if (alpha >= 1.f - kCompletionSlack) {
        return 1.f;
    }
    return std::clamp(alpha, 0.f, 1.f);
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

f32 AnimStateMachine::remaining_crossfade_time() const {
    if (!is_transitioning || blend_duration <= 0.f) {
        return 0.f;
    }

    return std::max(0.f, blend_duration - blend_time);
}

s32 AnimStateMachine::find_first_passing_outgoing_transition(u32 from_state) const {
    for (u32 i = 0; i < transitions.size(); ++i) {
        const Transition& transition = transitions[i];
        if (transition.from != from_state || transition.to == from_state) {
            continue;
        }
        if (transition.to >= states.size()) {
            continue;
        }
        if (!transition.condition || transition.condition()) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

bool AnimStateMachine::outgoing_transition_condition_passes(u32 from_state, u32 edge_index) const {
    u32 seen = 0;
    for (const Transition& transition : transitions) {
        if (transition.from != from_state) {
            continue;
        }
        if (seen == edge_index) {
            return !transition.condition || transition.condition();
        }
        ++seen;
    }
    return false;
}

f32 AnimStateMachine::incoming_transition_blend_duration_at(u32 to_state, u32 edge_index) const {
    u32 seen = 0;
    for (const Transition& transition : transitions) {
        if (transition.to != to_state) {
            continue;
        }
        if (seen == edge_index) {
            return transition.blend_duration;
        }
        ++seen;
    }
    return -1.f;
}

f32 AnimStateMachine::elapsed_crossfade_time() const {
    if (!is_transitioning) {
        return 0.f;
    }

    return std::max(0.f, blend_time);
}

bool AnimStateMachine::has_passing_outgoing_transition(u32 from_state) const {
    return find_first_passing_outgoing_transition(from_state) >= 0;
}

bool AnimStateMachine::incoming_transition_condition_passes(u32 to_state, u32 edge_index) const {
    u32 seen = 0;
    for (const Transition& transition : transitions) {
        if (transition.to != to_state) {
            continue;
        }
        if (seen == edge_index) {
            return !transition.condition || transition.condition();
        }
        ++seen;
    }
    return false;
}

bool AnimStateMachine::is_valid_transition_index(u32 transition_index) const {
    return transition_index < transitions.size();
}

s32 AnimStateMachine::transition_from_at(u32 transition_index) const {
    if (!is_valid_transition_index(transition_index)) {
        return -1;
    }
    return static_cast<s32>(transitions[transition_index].from);
}

s32 AnimStateMachine::transition_to_at(u32 transition_index) const {
    if (!is_valid_transition_index(transition_index)) {
        return -1;
    }
    return static_cast<s32>(transitions[transition_index].to);
}

bool AnimStateMachine::is_valid_pending_state() const {
    if (!is_transitioning) {
        return true;
    }

    return pending_state < states.size();
}

f32 AnimStateMachine::transition_blend_duration_at(u32 transition_index) const {
    if (!is_valid_transition_index(transition_index)) {
        return -1.f;
    }

    return transitions[transition_index].blend_duration;
}

bool AnimStateMachine::transition_condition_passes_at(u32 transition_index) const {
    if (!is_valid_transition_index(transition_index)) {
        return false;
    }

    const Transition& transition = transitions[transition_index];
    return !transition.condition || transition.condition();
}

void AnimStateMachine::evaluate_soa(f32 dt, const Skeleton& skel, PoseSoA& out) {
    if (is_empty()) {
        out.assign_bind_pose(skel);
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
        if (!is_valid_pending_state()) {
            is_transitioning = false;
            out.assign_bind_pose(skel);
            return;
        }

        blend_time += dt;
        PoseSoA targetSoa = PoseSoA::from_bind_pose(skel);
        if (pending_state < states.size() && states[pending_state].node) {
            states[pending_state].node->evaluate_soa(dt, skel, targetSoa);
            ensure_pose_soa_bind_fallback(targetSoa, skel);
        }

        ensure_pose_soa_bind_fallback(blend_from_pose_soa, skel);
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
        out.assign_bind_pose(skel);
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

        ensure_pose_soa_bind_fallback(out, skel);
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

        ensure_pose_soa_bind_fallback(blend_from_pose_soa, skel);
        blend_poses_soa(blend_from_pose_soa, targetSoa, alpha, out);
        ensure_pose_soa_bind_fallback(out, skel);
        out.compute_world_transforms(skel);
        return;
    }
}

void AnimStateMachine::evaluate(f32 dt, const Skeleton& skel, Pose& out) {
    evaluate_via_soa(*this, dt, skel, out);
    if (is_transitioning) {
        blend_from_pose = blend_from_pose_soa.to_pose();
    }
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
