#include <fuse/animation/clip.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

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

    // Standard T * R * S composition (scale applied in the bone's own frame before rotation).
    return mat4_from_trs(position, quat_normalize(rotation), scale);
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

namespace {

constexpr char kClipMagic[4] = {'F', 'A', 'C', 'L'};
constexpr u32 kClipVersion = 1;
constexpr u32 kMaxSerializedCount = 1u << 24;

struct ClipFileCloser {
    void operator()(std::FILE* file) const {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

template <typename T>
bool clip_write(std::FILE* file, const T* data, size_t count) {
    return count == 0 || std::fwrite(data, sizeof(T), count, file) == count;
}

template <typename T>
bool clip_read(std::FILE* file, T* data, size_t count) {
    return count == 0 || std::fread(data, sizeof(T), count, file) == count;
}

bool write_channel(std::FILE* file, const KeyframeChannel& channel) {
    const u32 counts[3] = {static_cast<u32>(channel.times.size()),
                           static_cast<u32>(channel.values_vec3.size()),
                           static_cast<u32>(channel.values_quat.size())};
    if (!clip_write(file, counts, 3) || !clip_write(file, channel.times.data(), channel.times.size())) {
        return false;
    }
    for (const vec3& v : channel.values_vec3) {
        const f32 xyz[3] = {v.x, v.y, v.z};
        if (!clip_write(file, xyz, 3)) {
            return false;
        }
    }
    for (const quat& q : channel.values_quat) {
        const f32 xyzw[4] = {q.x, q.y, q.z, q.w};
        if (!clip_write(file, xyzw, 4)) {
            return false;
        }
    }
    return true;
}

bool read_channel(std::FILE* file, KeyframeChannel& channel) {
    u32 counts[3] = {};
    if (!clip_read(file, counts, 3) || counts[0] > kMaxSerializedCount || counts[1] > kMaxSerializedCount ||
        counts[2] > kMaxSerializedCount) {
        return false;
    }
    // Every value track must line up with the key times (or be a single constant key).
    for (u32 i = 1; i < 3; ++i) {
        if (counts[i] > 1 && counts[i] != counts[0]) {
            return false;
        }
    }
    channel.times.resize(counts[0]);
    if (!clip_read(file, channel.times.data(), channel.times.size())) {
        return false;
    }
    if (!std::is_sorted(channel.times.begin(), channel.times.end())) {
        return false;
    }
    channel.values_vec3.resize(counts[1]);
    for (vec3& v : channel.values_vec3) {
        f32 xyz[3] = {};
        if (!clip_read(file, xyz, 3)) {
            return false;
        }
        v = {xyz[0], xyz[1], xyz[2], 0.f};
    }
    channel.values_quat.resize(counts[2]);
    for (quat& q : channel.values_quat) {
        f32 xyzw[4] = {};
        if (!clip_read(file, xyzw, 4)) {
            return false;
        }
        q = {xyzw[0], xyzw[1], xyzw[2], xyzw[3]};
    }
    return true;
}

} // namespace

bool AnimationClip::save(const char* path) const {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    std::unique_ptr<std::FILE, ClipFileCloser> file(std::fopen(path, "wb"));
    if (!file) {
        return false;
    }

    const u32 channelCount = static_cast<u32>(bone_channels.size());
    const u8 loopFlag = looping ? 1 : 0;
    bool ok = clip_write(file.get(), kClipMagic, sizeof(kClipMagic)) && clip_write(file.get(), &kClipVersion, 1) &&
              clip_write(file.get(), name, sizeof(name)) && clip_write(file.get(), &duration, 1) &&
              clip_write(file.get(), &sample_rate, 1) && clip_write(file.get(), &loopFlag, 1) &&
              clip_write(file.get(), &channelCount, 1);
    for (const BoneChannels& channels : bone_channels) {
        if (!ok) {
            break;
        }
        ok = clip_write(file.get(), &channels.bone_index, 1) && write_channel(file.get(), channels.position) &&
             write_channel(file.get(), channels.rotation) && write_channel(file.get(), channels.scale);
    }
    return ok && std::fflush(file.get()) == 0;
}

bool AnimationClip::load(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    std::unique_ptr<std::FILE, ClipFileCloser> file(std::fopen(path, "rb"));
    if (!file) {
        return false;
    }

    char magic[4] = {};
    u32 version = 0;
    AnimationClip loaded;
    u8 loopFlag = 0;
    u32 channelCount = 0;
    if (!clip_read(file.get(), magic, sizeof(magic)) || std::memcmp(magic, kClipMagic, sizeof(magic)) != 0 ||
        !clip_read(file.get(), &version, 1) || version != kClipVersion ||
        !clip_read(file.get(), loaded.name, sizeof(loaded.name)) || !clip_read(file.get(), &loaded.duration, 1) ||
        !clip_read(file.get(), &loaded.sample_rate, 1) || !clip_read(file.get(), &loopFlag, 1) ||
        !clip_read(file.get(), &channelCount, 1) || channelCount > kMaxSerializedCount ||
        !(loaded.duration >= 0.f)) {
        return false;
    }
    loaded.name[sizeof(loaded.name) - 1] = '\0';
    loaded.looping = loopFlag != 0;

    loaded.bone_channels.resize(channelCount);
    for (BoneChannels& channels : loaded.bone_channels) {
        if (!clip_read(file.get(), &channels.bone_index, 1) || !read_channel(file.get(), channels.position) ||
            !read_channel(file.get(), channels.rotation) || !read_channel(file.get(), channels.scale)) {
            return false;
        }
    }

    *this = std::move(loaded);
    return true;
}

} // namespace fuse::animation
