#include <fuse/animation/clip.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::animation {

namespace {

f32 clamp_time(f32 time, f32 duration, bool looping) {
    if (duration <= 0.f) {
        return 0.f;
    }
    if (looping) {
        time = std::fmod(time, duration);
        if (time < 0.f) {
            time += duration;
        }
        return time;
    }
    return std::clamp(time, 0.f, duration);
}

u32 find_segment(const std::vector<f32>& times, f32 time) {
    if (times.empty()) {
        return 0;
    }
    if (times.size() == 1) {
        return 0;
    }

    const auto it = std::upper_bound(times.begin(), times.end(), time);
    if (it == times.begin()) {
        return 0;
    }
    if (it == times.end()) {
        return static_cast<u32>(times.size() - 2);
    }
    return static_cast<u32>(std::distance(times.begin(), it) - 1);
}

bool channel_has_vec3(const KeyframeChannel& channel) {
    return !channel.values_vec3.empty();
}

bool channel_has_quat(const KeyframeChannel& channel) {
    return !channel.values_quat.empty();
}

} // namespace

vec3 KeyframeChannel::sample_vec3(f32 time) const {
    if (values_vec3.empty()) {
        return {};
    }
    if (values_vec3.size() == 1 || times.empty()) {
        return values_vec3.front();
    }

    const u32 segment = find_segment(times, time);
    const u32 next = std::min(segment + 1, static_cast<u32>(values_vec3.size() - 1));
    const f32 t0 = times[segment];
    const f32 t1 = times[next];
    const f32 alpha = (t1 > t0) ? ((time - t0) / (t1 - t0)) : 0.f;
    return lerp(values_vec3[segment], values_vec3[next], alpha);
}

quat KeyframeChannel::sample_quat(f32 time) const {
    if (values_quat.empty()) {
        return {};
    }
    if (values_quat.size() == 1 || times.empty()) {
        return values_quat.front();
    }

    const u32 segment = find_segment(times, time);
    const u32 next = std::min(segment + 1, static_cast<u32>(values_quat.size() - 1));
    const f32 t0 = times[segment];
    const f32 t1 = times[next];
    const f32 alpha = (t1 > t0) ? ((time - t0) / (t1 - t0)) : 0.f;
    return lerp(values_quat[segment], values_quat[next], alpha);
}

namespace {

mat4 build_channel_local(const AnimationClip::BoneChannels& channels, f32 sample_time) {
    const vec3 position =
        channel_has_vec3(channels.position) ? channels.position.sample_vec3(sample_time) : vec3{};
    const quat rotation =
        channel_has_quat(channels.rotation) ? channels.rotation.sample_quat(sample_time) : quat{};
    const vec3 scale =
        channel_has_vec3(channels.scale) ? channels.scale.sample_vec3(sample_time) : vec3{1.f, 1.f, 1.f, 0.f};

    mat4 local = mat4::identity();
    local.data[0] = scale.x;
    local.data[5] = scale.y;
    local.data[10] = scale.z;
    local.data[12] = position.x;
    local.data[13] = position.y;
    local.data[14] = position.z;

    if (rotation.w != 1.f || rotation.x != 0.f || rotation.y != 0.f || rotation.z != 0.f) {
        local = mat4_multiply(local, mat4_from_trs({}, rotation, {1.f, 1.f, 1.f, 0.f}));
    }
    return local;
}

} // namespace

void AnimationClip::evaluate(f32 time, const Skeleton& skel, PoseSoA& out_pose) const {
    out_pose = PoseSoA::from_bind_pose(skel);
    const f32 sample_time = clamp_time(time, duration, looping);

    for (const BoneChannels& channels : bone_channels) {
        if (channels.bone_index >= out_pose.bone_count) {
            continue;
        }

        const bool hasPosition = channel_has_vec3(channels.position);
        const bool hasRotation = channel_has_quat(channels.rotation);
        const bool hasScale = channel_has_vec3(channels.scale);
        if (!hasPosition && !hasRotation && !hasScale) {
            continue;
        }

        const mat4 channel_local = build_channel_local(channels, sample_time);
        const mat4 bind_local = skel.bones[channels.bone_index].local_transform;
        const mat4 combined = mat4_multiply(bind_local, channel_local);
        decompose_trs(combined,
                      out_pose.local_positions[channels.bone_index],
                      out_pose.local_rotations[channels.bone_index],
                      out_pose.local_scales[channels.bone_index]);
    }

    out_pose.compute_world_transforms(skel);
}

void AnimationClip::sample(f32 time, const Skeleton& skel, Pose& out_pose) const {
    PoseSoA soa = PoseSoA::from_bind_pose(skel);
    evaluate(time, skel, soa);
    out_pose = soa.to_pose();
}

bool AnimationClip::save(const char* path) const {
    return path != nullptr && path[0] != '\0';
}

bool AnimationClip::load(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    std::strncpy(name, path, sizeof(name) - 1);
    duration = 1.f;
    sample_rate = 30.f;
    return true;
}

} // namespace fuse::animation
