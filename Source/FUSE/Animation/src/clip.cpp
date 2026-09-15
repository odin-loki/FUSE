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

void AnimationClip::sample(f32 time, const Skeleton& skel, Pose& out_pose) const {
    out_pose = Pose::make_bind_pose(skel);
    const f32 sample_time = clamp_time(time, duration, looping);

    for (const BoneChannels& channels : bone_channels) {
        if (channels.bone_index >= out_pose.bone_world_transforms.size()) {
            continue;
        }

        const vec3 position = channels.position.sample_vec3(sample_time);
        const quat rotation = channels.rotation.sample_quat(sample_time);
        const vec3 scale = channels.scale.sample_vec3(sample_time);

        mat4 local = mat4::identity();
        local.data[0] = scale.x;
        local.data[5] = scale.y;
        local.data[10] = scale.z;
        local.data[12] = position.x;
        local.data[13] = position.y;
        local.data[14] = position.z;

        if (rotation.w != 1.f || rotation.x != 0.f || rotation.y != 0.f || rotation.z != 0.f) {
            const f32 xx = rotation.x * rotation.x;
            const f32 yy = rotation.y * rotation.y;
            const f32 zz = rotation.z * rotation.z;
            const f32 xy = rotation.x * rotation.y;
            const f32 xz = rotation.x * rotation.z;
            const f32 yz = rotation.y * rotation.z;
            const f32 wx = rotation.w * rotation.x;
            const f32 wy = rotation.w * rotation.y;
            const f32 wz = rotation.w * rotation.z;

            mat4 rot = mat4::identity();
            rot.data[0] = 1.f - 2.f * (yy + zz);
            rot.data[1] = 2.f * (xy + wz);
            rot.data[2] = 2.f * (xz - wy);
            rot.data[4] = 2.f * (xy - wz);
            rot.data[5] = 1.f - 2.f * (xx + zz);
            rot.data[6] = 2.f * (yz + wx);
            rot.data[8] = 2.f * (xz + wy);
            rot.data[9] = 2.f * (yz - wx);
            rot.data[10] = 1.f - 2.f * (xx + yy);
            local = mat4_multiply(local, rot);
        }

        out_pose.bone_world_transforms[channels.bone_index] =
            skel.compute_world_transform(channels.bone_index);
        out_pose.bone_world_transforms[channels.bone_index] =
            mat4_multiply(out_pose.bone_world_transforms[channels.bone_index], local);
    }
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
