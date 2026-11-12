#include <fuse/cook/anim_cook.hpp>

#include <fuse/animation/math.hpp>
#include <fuse/cook/dts_import.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

#if defined(FUSE_HAS_ASSIMP)
#include <assimp/Importer.hpp>
#include <assimp/anim.h>
#include <assimp/scene.h>
#endif

namespace fuse::cook {

namespace {

namespace an = fuse::animation;

bool fail(std::string* error, CookFailure* failure, CookFailure cls, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    if (failure != nullptr) {
        *failure = cls;
    }
    return false;
}

bool has_ext(const std::string& path, const char* ext) {
    std::string e = std::filesystem::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ext;
}

/// Asset plan 4a: `.dts` / `.dsq` sequences through the clean-room reader (dts_import.hpp).
bool import_torque_anim(const std::string& input_path, const AnimCookOptions& options, an::CookedAnim& out,
                        std::string* error, CookFailure* failure) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(input_path, ec)) {
        return fail(error, failure, CookFailure::InvalidArgument, "source not found: " + input_path);
    }
    const bool dsq = has_ext(input_path, ".dsq");
    DtsShape shape;
    DsqFile sequences;
    std::string why;
    if (dsq ? !read_dsq_file(input_path, sequences, &why) : !read_dts_file(input_path, shape, &why)) {
        return fail(error, failure, CookFailure::MalformedSource, std::string(dsq ? "dsq" : "dts") + " read failed: " + why);
    }
    DtsShape skeleton;
    if (dsq && !options.skeleton_path.empty() && !read_dts_file(options.skeleton_path, skeleton, &why)) {
        return fail(error, failure, CookFailure::InvalidArgument, "skeleton shape unreadable: " + why);
    }
    const std::vector<DtsSequence>& list = dsq ? sequences.sequences : shape.sequences;
    if (list.empty()) {
        return fail(error, failure, CookFailure::MalformedSource, "source has no animation sequence");
    }
    u32 index = options.clip_index;
    if (!options.clip_name.empty()) {
        index = static_cast<u32>(list.size());
        for (u32 i = 0; i < list.size(); ++i) {
            if (list[i].name == options.clip_name) {
                index = i;
                break;
            }
        }
        if (index == list.size()) {
            return fail(error, failure, CookFailure::MalformedSource, "no sequence named '" + options.clip_name + "'");
        }
    } else if (index >= list.size()) {
        return fail(error, failure, CookFailure::MalformedSource,
                    "clip index " + std::to_string(index) + " out of range (" + std::to_string(list.size()) + " sequences)");
    }
    DtsAnimOptions animOptions;
    animOptions.sample_rate = options.sample_rate;
    animOptions.root_motion = options.root_motion;
    const bool ok = dsq ? dsq_sequence_to_cooked_anim(sequences, index, options.skeleton_path.empty() ? nullptr : &skeleton,
                                                      animOptions, out, &why)
                        : dts_sequence_to_cooked_anim(shape, index, animOptions, out, &why);
    if (!ok) {
        return fail(error, failure, CookFailure::MalformedSource, "sequence conversion failed: " + why);
    }
    for (const AnimCookEvent& event : options.events) {
        if (!std::isfinite(event.time) || event.time < 0.f || event.time > out.duration || event.name.empty()) {
            return fail(error, failure, CookFailure::InvalidArgument, "event '" + event.name + "' outside the clip");
        }
        out.events.push_back(an::CookedAnimEvent{event.name, event.time});
    }
    std::sort(out.events.begin(), out.events.end(), [](const an::CookedAnimEvent& a, const an::CookedAnimEvent& b) {
        return a.time != b.time ? a.time < b.time : a.name < b.name;
    });
    return true;
}

#if defined(FUSE_HAS_ASSIMP)

/// Index of the segment [i, i + 1] containing `t` (keys sorted); clamps.
template <typename Key>
void bracket(const Key* keys, u32 count, double t, u32& i0, u32& i1, double& alpha) {
    if (count <= 1u || t <= keys[0].mTime) {
        i0 = i1 = 0;
        alpha = 0.0;
        return;
    }
    if (t >= keys[count - 1u].mTime) {
        i0 = i1 = count - 1u;
        alpha = 0.0;
        return;
    }
    u32 hi = 1;
    while (hi < count && keys[hi].mTime <= t) {
        ++hi;
    }
    i0 = hi - 1u;
    i1 = hi;
    const double span = keys[i1].mTime - keys[i0].mTime;
    alpha = span > 0.0 ? (t - keys[i0].mTime) / span : 0.0;
    if (keys[i0].mInterpolation == aiAnimInterpolation_Step) {
        alpha = 0.0;
    }
}

an::vec3 sample_vec(const aiVectorKey* keys, u32 count, double t) {
    u32 i0 = 0, i1 = 0;
    double a = 0.0;
    bracket(keys, count, t, i0, i1, a);
    const aiVector3D& p = keys[i0].mValue;
    const aiVector3D& q = keys[i1].mValue;
    return {static_cast<f32>(p.x + (q.x - p.x) * a), static_cast<f32>(p.y + (q.y - p.y) * a),
            static_cast<f32>(p.z + (q.z - p.z) * a), 0.f};
}

an::quat sample_quat(const aiQuatKey* keys, u32 count, double t) {
    u32 i0 = 0, i1 = 0;
    double a = 0.0;
    bracket(keys, count, t, i0, i1, a);
    const aiQuaternion& p = keys[i0].mValue;
    const aiQuaternion& q = keys[i1].mValue;
    const an::quat qa = an::quat_normalize({static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z),
                                            static_cast<f32>(p.w)});
    if (i0 == i1 || a == 0.0) {
        return qa;
    }
    const an::quat qb = an::quat_normalize({static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z),
                                            static_cast<f32>(q.w)});
    return an::quat_normalize(an::lerp(qa, qb, static_cast<f32>(a)));
}

template <typename Key>
bool keys_ok(const Key* keys, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (!std::isfinite(keys[i].mTime) || (i > 0 && keys[i].mTime < keys[i - 1].mTime)) {
            return false;
        }
    }
    return true;
}

bool vector_keys_finite(const aiVectorKey* keys, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const aiVector3D& v = keys[i].mValue;
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
    }
    return true;
}

bool quat_keys_finite(const aiQuatKey* keys, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        const aiQuaternion& q = keys[i].mValue;
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) ||
            (q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) < 1e-12f) {
            return false;
        }
    }
    return true;
}

void collect_order(const aiNode* node, s32 parentJoint, const std::map<std::string, u32>& channelOf,
                   std::vector<std::pair<u32, s32>>& order) {
    if (node == nullptr) {
        return;
    }
    s32 here = parentJoint;
    const auto it = channelOf.find(node->mName.C_Str());
    if (it != channelOf.end()) {
        bool seen = false;
        for (const auto& entry : order) {
            if (entry.first == it->second) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            order.emplace_back(it->second, parentJoint);
            here = static_cast<s32>(order.size() - 1u);
        }
    }
    for (u32 c = 0; c < node->mNumChildren; ++c) {
        collect_order(node->mChildren[c], here, channelOf, order);
    }
}

f32 yaw_of(const an::quat& q) {
    const an::vec3 f = an::quat_rotate(q, {0.f, 0.f, 1.f, 0.f});
    return static_cast<f32>(std::atan2(static_cast<double>(f.x), static_cast<double>(f.z)));
}

#endif

} // namespace

u32 count_anim_clips(const std::string& input_path) {
    if (has_ext(input_path, ".dts")) {
        DtsShape shape;
        return read_dts_file(input_path, shape) ? static_cast<u32>(shape.sequences.size()) : 0u;
    }
    if (has_ext(input_path, ".dsq")) {
        DsqFile dsq;
        return read_dsq_file(input_path, dsq) ? static_cast<u32>(dsq.sequences.size()) : 0u;
    }
#if defined(FUSE_HAS_ASSIMP)
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(input_path, 0u);
    return scene != nullptr ? scene->mNumAnimations : 0u;
#else
    (void)input_path;
    return 0u;
#endif
}

bool import_anim_file(const std::string& input_path, const AnimCookOptions& options, an::CookedAnim& out,
                      std::string* error, CookFailure* failure) {
    out = an::CookedAnim{};
    if (input_path.empty()) {
        return fail(error, failure, CookFailure::InvalidArgument, "empty input path");
    }
    if (!(options.sample_rate > 0.f) || options.sample_rate > 1000.f) {
        return fail(error, failure, CookFailure::InvalidArgument, "sample_rate must be in (0, 1000] Hz");
    }
    if (has_ext(input_path, ".dts") || has_ext(input_path, ".dsq")) {
        return import_torque_anim(input_path, options, out, error, failure);
    }
#if !defined(FUSE_HAS_ASSIMP)
    return fail(error, failure, CookFailure::ImporterUnavailable, "anim cook needs assimp (not linked in this build)");
#else
    std::error_code ec;
    if (!std::filesystem::is_regular_file(input_path, ec)) {
        return fail(error, failure, CookFailure::InvalidArgument, "source not found: " + input_path);
    }
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(input_path, 0u);
    if (scene == nullptr) {
        return fail(error, failure, CookFailure::MalformedSource,
                    std::string("assimp could not read the source: ") + importer.GetErrorString());
    }
    if (scene->mNumAnimations == 0u) {
        return fail(error, failure, CookFailure::MalformedSource, "source has no animation clip");
    }
    const aiAnimation* clip = nullptr;
    if (!options.clip_name.empty()) {
        for (u32 i = 0; i < scene->mNumAnimations; ++i) {
            if (options.clip_name == scene->mAnimations[i]->mName.C_Str()) {
                clip = scene->mAnimations[i];
                break;
            }
        }
        if (clip == nullptr) {
            return fail(error, failure, CookFailure::MalformedSource, "no clip named '" + options.clip_name + "'");
        }
    } else {
        if (options.clip_index >= scene->mNumAnimations) {
            return fail(error, failure, CookFailure::MalformedSource,
                        "clip index " + std::to_string(options.clip_index) + " out of range (" +
                            std::to_string(scene->mNumAnimations) + " clips)");
        }
        clip = scene->mAnimations[options.clip_index];
    }
    if (clip->mNumChannels == 0u) {
        return fail(error, failure, CookFailure::MalformedSource, "clip has no node channels");
    }
    const double tps = clip->mTicksPerSecond > 0.0 ? clip->mTicksPerSecond : 25.0;
    const double durationSec = clip->mDuration / tps;
    if (!std::isfinite(durationSec) || durationSec < 0.0) {
        return fail(error, failure, CookFailure::MalformedSource, "clip duration is negative or not finite");
    }

    std::map<std::string, u32> channelOf;
    for (u32 c = 0; c < clip->mNumChannels; ++c) {
        const aiNodeAnim* ch = clip->mChannels[c];
        const std::string name = ch->mNodeName.C_Str();
        if (name.empty() || !channelOf.emplace(name, c).second) {
            return fail(error, failure, CookFailure::MalformedSource, "empty or duplicate channel node '" + name + "'");
        }
        if (!keys_ok(ch->mPositionKeys, ch->mNumPositionKeys) || !keys_ok(ch->mRotationKeys, ch->mNumRotationKeys) ||
            !keys_ok(ch->mScalingKeys, ch->mNumScalingKeys) ||
            !vector_keys_finite(ch->mPositionKeys, ch->mNumPositionKeys) ||
            !vector_keys_finite(ch->mScalingKeys, ch->mNumScalingKeys) ||
            !quat_keys_finite(ch->mRotationKeys, ch->mNumRotationKeys)) {
            return fail(error, failure, CookFailure::MalformedSource,
                        "channel '" + name + "': key times not sorted / not finite, or bad key values");
        }
    }
    std::vector<std::pair<u32, s32>> order; // (channel, parent joint)
    collect_order(scene->mRootNode, -1, channelOf, order);
    for (u32 c = 0; c < clip->mNumChannels; ++c) {
        bool seen = false;
        for (const auto& entry : order) {
            if (entry.first == c) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            order.emplace_back(c, -1);
        }
    }

    an::CookedAnim anim;
    anim.name = clip->mName.C_Str();
    anim.sample_rate = options.sample_rate;
    anim.duration = static_cast<f32>(durationSec);
    anim.frame_count = an::cooked_anim_frame_count(anim.duration, anim.sample_rate);
    anim.looping = options.looping;
    anim.joints.resize(order.size());
    for (usize j = 0; j < order.size(); ++j) {
        const aiNodeAnim* ch = clip->mChannels[order[j].first];
        an::CookedAnimJoint& joint = anim.joints[j];
        joint.name = ch->mNodeName.C_Str();
        joint.parent = order[j].second;
        for (u32 f = 0; f < anim.frame_count; ++f) {
            const double t = static_cast<double>(anim.frame_time(f)) * tps;
            if (ch->mNumPositionKeys > 0u) joint.translation.push_back(sample_vec(ch->mPositionKeys, ch->mNumPositionKeys, t));
            if (ch->mNumRotationKeys > 0u) joint.rotation.push_back(sample_quat(ch->mRotationKeys, ch->mNumRotationKeys, t));
            if (ch->mNumScalingKeys > 0u) joint.scale.push_back(sample_vec(ch->mScalingKeys, ch->mNumScalingKeys, t));
        }
    }

    for (const AnimCookEvent& ev : options.events) {
        if (ev.name.empty() || !(ev.time >= 0.f) || !(ev.time <= anim.duration)) {
            return fail(error, failure, CookFailure::InvalidArgument,
                        "event '" + ev.name + "' needs a name and a time in [0, duration]");
        }
        anim.events.push_back({ev.name, ev.time});
    }
    std::sort(anim.events.begin(), anim.events.end(), [](const an::CookedAnimEvent& a, const an::CookedAnimEvent& b) {
        return a.time != b.time ? a.time < b.time : a.name < b.name;
    });

    if (options.root_motion) {
        u32 root = 0;
        if (!options.root_joint.empty()) {
            bool found = false;
            for (usize j = 0; j < anim.joints.size(); ++j) {
                if (anim.joints[j].name == options.root_joint) {
                    root = static_cast<u32>(j);
                    found = true;
                    break;
                }
            }
            if (!found) {
                return fail(error, failure, CookFailure::InvalidArgument,
                            "root joint '" + options.root_joint + "' is not animated by the clip");
            }
        }
        const an::CookedAnimJoint& joint = anim.joints[root];
        anim.has_root_motion = true;
        anim.root_joint = root;
        anim.root_motion.resize(anim.frame_count);
        double yawAccum = 0.0;
        f32 prevYaw = joint.rotation.empty() ? 0.f : yaw_of(joint.rotation.front());
        for (u32 f = 0; f < anim.frame_count; ++f) {
            an::CookedAnimRootSample& s = anim.root_motion[f];
            if (!joint.translation.empty()) {
                s.dx = joint.translation[f].x - joint.translation.front().x;
                s.dy = joint.translation[f].y - joint.translation.front().y;
                s.dz = joint.translation[f].z - joint.translation.front().z;
            }
            if (!joint.rotation.empty() && f > 0) {
                const f32 yaw = yaw_of(joint.rotation[f]);
                double d = static_cast<double>(yaw) - prevYaw;
                constexpr double kPi = 3.14159265358979323846;
                while (d > kPi) d -= 2.0 * kPi;
                while (d < -kPi) d += 2.0 * kPi;
                yawAccum += d;
                prevYaw = yaw;
            }
            s.dyaw = static_cast<f32>(yawAccum);
        }
    }
    out = std::move(anim);
    if (failure != nullptr) {
        *failure = CookFailure::None;
    }
    return true;
#endif
}

CookStubWriteResult cook_anim_file(const std::string& input_path, const std::string& output_path,
                                   const AnimCookOptions& options) {
    CookStubWriteResult result;
    if (output_path.empty()) {
        result.failure = CookFailure::InvalidArgument;
        result.note = "empty output path";
        return result;
    }
    an::CookedAnim anim;
    std::string error;
    CookFailure failure = CookFailure::None;
    if (!import_anim_file(input_path, options, anim, &error, &failure)) {
        result.failure = failure;
        result.note = "anim import failed: " + error;
        return result;
    }
    const std::vector<u8> bytes = an::encode_cooked_anim(anim, &error);
    if (bytes.empty()) {
        result.failure = CookFailure::MalformedSource;
        result.note = "anim encode refused: " + error;
        return result;
    }
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(output_path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        result.failure = CookFailure::WriteFailed;
        result.note = "cannot write " + output_path;
        return result;
    }
    result.ok = true;
    result.byteCount = static_cast<u32>(bytes.size());
    result.note = "fuseanim '" + anim.name + "' (" + std::to_string(anim.joints.size()) + " joints, " +
                  std::to_string(anim.frame_count) + " frames @ " + std::to_string(static_cast<int>(anim.sample_rate)) +
                  " Hz, " + std::to_string(anim.events.size()) + " events" +
                  (anim.has_root_motion ? ", root motion" : "") + ")";
    return result;
}

} // namespace fuse::cook
