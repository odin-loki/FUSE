#include <fuse/animation/cooked_anim.hpp>

#include <fuse/animation/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fuse::animation {

namespace {

constexpr u32 kMaxJoints = 1u << 16;
constexpr u32 kMaxFrames = 1u << 22;
constexpr u32 kMaxEvents = 1u << 16;
constexpr double kSmallestThreeRange = 0.70710678118654752440; // 1 / sqrt(2)
constexpr u32 kUnorm20Max = (1u << 20) - 1u;

u64 fnv1a64(const u8* data, usize size) {
    u64 hash = 0xCBF29CE484222325ull;
    for (usize i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

bool finite3(const vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite4(const quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

// ---- writer ----------------------------------------------------------------------------------------------------

class Writer {
public:
    std::vector<u8> bytes;

    void u8v(u8 v) { bytes.push_back(v); }
    void u16v(u16 v) {
        bytes.push_back(static_cast<u8>(v & 0xFFu));
        bytes.push_back(static_cast<u8>(v >> 8));
    }
    void u32v(u32 v) {
        for (u32 i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<u8>((v >> (8u * i)) & 0xFFu));
        }
    }
    void u64v(u64 v) {
        for (u32 i = 0; i < 8; ++i) {
            bytes.push_back(static_cast<u8>((v >> (8u * i)) & 0xFFu));
        }
    }
    void s32v(s32 v) {
        u32 u = 0;
        std::memcpy(&u, &v, 4);
        u32v(u);
    }
    void f32v(f32 v) {
        u32 u = 0;
        std::memcpy(&u, &v, 4);
        u32v(u);
    }
    void pad4() {
        while ((bytes.size() & 3u) != 0u) {
            bytes.push_back(0);
        }
    }
    void str(const std::string& s) {
        u16v(static_cast<u16>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
        pad4();
    }
};

// ---- reader ----------------------------------------------------------------------------------------------------

class Reader {
public:
    Reader(const u8* data, usize size) : m_data(data), m_size(size) {}

    bool ok() const { return m_ok; }
    usize pos() const { return m_pos; }

    bool take(usize n) {
        if (!m_ok || n > m_size - m_pos) {
            m_ok = false;
            return false;
        }
        return true;
    }
    u8 u8v() {
        if (!take(1)) return 0;
        return m_data[m_pos++];
    }
    u16 u16v() {
        if (!take(2)) return 0;
        const u16 v = static_cast<u16>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
        m_pos += 2;
        return v;
    }
    u32 u32v() {
        if (!take(4)) return 0;
        u32 v = 0;
        for (u32 i = 0; i < 4; ++i) {
            v |= static_cast<u32>(m_data[m_pos + i]) << (8u * i);
        }
        m_pos += 4;
        return v;
    }
    u64 u64v() {
        if (!take(8)) return 0;
        u64 v = 0;
        for (u32 i = 0; i < 8; ++i) {
            v |= static_cast<u64>(m_data[m_pos + i]) << (8u * i);
        }
        m_pos += 8;
        return v;
    }
    s32 s32v() {
        const u32 u = u32v();
        s32 v = 0;
        std::memcpy(&v, &u, 4);
        return v;
    }
    f32 f32v() {
        const u32 u = u32v();
        f32 v = 0.f;
        std::memcpy(&v, &u, 4);
        return v;
    }
    void pad4() {
        while ((m_pos & 3u) != 0u) {
            if (u8v() != 0u) {
                m_ok = false;
            }
        }
    }
    std::string str() {
        const u16 n = u16v();
        std::string s;
        if (take(n)) {
            s.assign(reinterpret_cast<const char*>(m_data + m_pos), n);
            m_pos += n;
        }
        pad4();
        return s;
    }

private:
    const u8* m_data;
    usize m_size;
    usize m_pos = 0;
    bool m_ok = true;
};

// ---- track coding ----------------------------------------------------------------------------------------------

bool same3(const vec3& a, const vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same4(const quat& a, const quat& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

CookedTrackMode vec3_mode(const std::vector<vec3>& track) {
    if (track.empty()) {
        return CookedTrackMode::Absent;
    }
    for (const vec3& v : track) {
        if (!same3(v, track.front())) {
            return CookedTrackMode::Quantized;
        }
    }
    return CookedTrackMode::Constant;
}

quat normalized(const quat& q) {
    const double len = std::sqrt(static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y +
                                 static_cast<double>(q.z) * q.z + static_cast<double>(q.w) * q.w);
    if (!(len > 1e-12)) {
        return quat{};
    }
    return {static_cast<f32>(q.x / len), static_cast<f32>(q.y / len), static_cast<f32>(q.z / len),
            static_cast<f32>(q.w / len)};
}

CookedTrackMode quat_mode(const std::vector<quat>& track) {
    if (track.empty()) {
        return CookedTrackMode::Absent;
    }
    for (const quat& q : track) {
        if (!same4(q, track.front())) {
            return CookedTrackMode::Quantized;
        }
    }
    return CookedTrackMode::Constant;
}

u16 quantize_unorm16(f32 v, f32 lo, f32 hi) {
    const double range = static_cast<double>(hi) - static_cast<double>(lo);
    if (!(range > 0.0)) {
        return 0;
    }
    const double t = (static_cast<double>(v) - lo) / range;
    const double q = std::floor(std::clamp(t, 0.0, 1.0) * 65535.0 + 0.5);
    return static_cast<u16>(q);
}

f32 dequantize_unorm16(u16 q, f32 lo, f32 hi) {
    const double range = static_cast<double>(hi) - static_cast<double>(lo);
    return static_cast<f32>(static_cast<double>(lo) + range * (static_cast<double>(q) / 65535.0));
}

void write_vec3_track(Writer& w, const std::vector<vec3>& track, CookedTrackMode mode) {
    if (mode == CookedTrackMode::Constant) {
        w.f32v(track.front().x);
        w.f32v(track.front().y);
        w.f32v(track.front().z);
        return;
    }
    if (mode != CookedTrackMode::Quantized) {
        return;
    }
    f32 lo[3] = {track.front().x, track.front().y, track.front().z};
    f32 hi[3] = {lo[0], lo[1], lo[2]};
    for (const vec3& v : track) {
        const f32 c[3] = {v.x, v.y, v.z};
        for (u32 i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], c[i]);
            hi[i] = std::max(hi[i], c[i]);
        }
    }
    for (u32 i = 0; i < 3; ++i) w.f32v(lo[i]);
    for (u32 i = 0; i < 3; ++i) w.f32v(hi[i]);
    for (const vec3& v : track) {
        w.u16v(quantize_unorm16(v.x, lo[0], hi[0]));
        w.u16v(quantize_unorm16(v.y, lo[1], hi[1]));
        w.u16v(quantize_unorm16(v.z, lo[2], hi[2]));
    }
    w.pad4();
}

bool read_vec3_track(Reader& r, CookedTrackMode mode, u32 frames, std::vector<vec3>& out) {
    out.clear();
    if (mode == CookedTrackMode::Absent) {
        return true;
    }
    if (mode == CookedTrackMode::Constant) {
        vec3 v{};
        v.x = r.f32v();
        v.y = r.f32v();
        v.z = r.f32v();
        out.assign(frames, v);
        return r.ok() && finite3(v);
    }
    if (mode != CookedTrackMode::Quantized) {
        return false;
    }
    f32 lo[3], hi[3];
    for (u32 i = 0; i < 3; ++i) lo[i] = r.f32v();
    for (u32 i = 0; i < 3; ++i) hi[i] = r.f32v();
    for (u32 i = 0; i < 3; ++i) {
        if (!std::isfinite(lo[i]) || !std::isfinite(hi[i]) || hi[i] < lo[i]) {
            return false;
        }
    }
    if (!r.take(static_cast<usize>(frames) * 6u)) {
        return false;
    }
    out.resize(frames);
    for (vec3& v : out) {
        v.x = dequantize_unorm16(r.u16v(), lo[0], hi[0]);
        v.y = dequantize_unorm16(r.u16v(), lo[1], hi[1]);
        v.z = dequantize_unorm16(r.u16v(), lo[2], hi[2]);
    }
    r.pad4();
    return r.ok();
}

u64 encode_smallest_three(const quat& input) {
    const quat q = normalized(input);
    double c[4] = {q.x, q.y, q.z, q.w};
    u32 largest = 0;
    for (u32 i = 1; i < 4; ++i) {
        if (std::fabs(c[i]) > std::fabs(c[largest])) {
            largest = i;
        }
    }
    if (c[largest] < 0.0) {
        for (double& v : c) v = -v;
    }
    u64 bits = largest;
    u32 shift = 2;
    for (u32 i = 0; i < 4; ++i) {
        if (i == largest) {
            continue;
        }
        const double t = (std::clamp(c[i], -kSmallestThreeRange, kSmallestThreeRange) + kSmallestThreeRange) /
                         (2.0 * kSmallestThreeRange);
        const u64 u = static_cast<u64>(std::floor(t * kUnorm20Max + 0.5));
        bits |= std::min<u64>(u, kUnorm20Max) << shift;
        shift += 20;
    }
    return bits;
}

quat decode_smallest_three(u64 bits) {
    const u32 largest = static_cast<u32>(bits & 3u);
    double c[4] = {};
    double sum = 0.0;
    u32 shift = 2;
    for (u32 i = 0; i < 4; ++i) {
        if (i == largest) {
            continue;
        }
        const u64 u = (bits >> shift) & kUnorm20Max;
        c[i] = static_cast<double>(u) / kUnorm20Max * (2.0 * kSmallestThreeRange) - kSmallestThreeRange;
        sum += c[i] * c[i];
        shift += 20;
    }
    c[largest] = std::sqrt(std::max(0.0, 1.0 - sum));
    return normalized({static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2]), static_cast<f32>(c[3])});
}

void write_quat_track(Writer& w, const std::vector<quat>& track, CookedTrackMode mode) {
    if (mode == CookedTrackMode::Constant) {
        const quat q = normalized(track.front());
        w.f32v(q.x);
        w.f32v(q.y);
        w.f32v(q.z);
        w.f32v(q.w);
        return;
    }
    if (mode != CookedTrackMode::Quantized) {
        return;
    }
    for (const quat& q : track) {
        w.u64v(encode_smallest_three(q));
    }
}

bool read_quat_track(Reader& r, CookedTrackMode mode, u32 frames, std::vector<quat>& out) {
    out.clear();
    if (mode == CookedTrackMode::Absent) {
        return true;
    }
    if (mode == CookedTrackMode::Constant) {
        quat q{};
        q.x = r.f32v();
        q.y = r.f32v();
        q.z = r.f32v();
        q.w = r.f32v();
        if (!r.ok() || !finite4(q)) {
            return false;
        }
        out.assign(frames, normalized(q));
        return true;
    }
    if (mode != CookedTrackMode::Quantized) {
        return false;
    }
    if (!r.take(static_cast<usize>(frames) * 8u)) {
        return false;
    }
    out.resize(frames);
    for (quat& q : out) {
        const u64 bits = r.u64v();
        if ((bits >> 62) != 0u) {
            return false;
        }
        q = decode_smallest_three(bits);
    }
    return r.ok();
}

bool event_less(const CookedAnimEvent& a, const CookedAnimEvent& b) {
    if (a.time != b.time) {
        return a.time < b.time;
    }
    return a.name < b.name;
}

bool validate(const CookedAnim& anim, std::string* error) {
    if (!(anim.sample_rate > 0.f) || !std::isfinite(anim.sample_rate) || anim.sample_rate > 1000.f) {
        return fail(error, "sample_rate must be in (0, 1000] Hz");
    }
    if (!(anim.duration >= 0.f) || !std::isfinite(anim.duration)) {
        return fail(error, "duration must be finite and >= 0");
    }
    if (anim.frame_count != cooked_anim_frame_count(anim.duration, anim.sample_rate) || anim.frame_count > kMaxFrames) {
        return fail(error, "frame_count does not match duration * sample_rate");
    }
    if (anim.name.size() > 0xFFFFu) {
        return fail(error, "clip name too long");
    }
    if (anim.joints.empty() || anim.joints.size() > kMaxJoints) {
        return fail(error, "joint count must be 1..65536");
    }
    for (usize j = 0; j < anim.joints.size(); ++j) {
        const CookedAnimJoint& joint = anim.joints[j];
        const std::string where = "joints[" + std::to_string(j) + "] '" + joint.name + "': ";
        if (joint.name.empty() || joint.name.size() > 0xFFFFu) {
            return fail(error, where + "name must be 1..65535 bytes");
        }
        if (joint.parent < -1 || joint.parent >= static_cast<s32>(j)) {
            return fail(error, where + "parent must be -1 or an earlier joint");
        }
        for (usize k = 0; k < j; ++k) {
            if (anim.joints[k].name == joint.name) {
                return fail(error, where + "duplicate joint name");
            }
        }
        const auto sized = [&](usize n) { return n == 0 || n == anim.frame_count; };
        if (!sized(joint.translation.size()) || !sized(joint.rotation.size()) || !sized(joint.scale.size())) {
            return fail(error, where + "track sample count != frame_count");
        }
        for (const vec3& v : joint.translation) {
            if (!finite3(v)) return fail(error, where + "non-finite translation");
        }
        for (const vec3& v : joint.scale) {
            if (!finite3(v)) return fail(error, where + "non-finite scale");
        }
        for (const quat& q : joint.rotation) {
            if (!finite4(q)) return fail(error, where + "non-finite rotation");
        }
    }
    if (anim.has_root_motion) {
        if (anim.root_joint >= anim.joints.size()) {
            return fail(error, "root motion joint out of range");
        }
        if (anim.root_motion.size() != anim.frame_count) {
            return fail(error, "root motion sample count != frame_count");
        }
        for (const CookedAnimRootSample& s : anim.root_motion) {
            if (!std::isfinite(s.dx) || !std::isfinite(s.dy) || !std::isfinite(s.dz) || !std::isfinite(s.dyaw)) {
                return fail(error, "non-finite root motion");
            }
        }
    }
    if (anim.events.size() > kMaxEvents) {
        return fail(error, "too many events");
    }
    for (usize e = 0; e < anim.events.size(); ++e) {
        const CookedAnimEvent& ev = anim.events[e];
        if (ev.name.empty() || ev.name.size() > 0xFFFFu) {
            return fail(error, "events[" + std::to_string(e) + "]: name must be 1..65535 bytes");
        }
        if (!(ev.time >= 0.f) || !(ev.time <= anim.duration)) {
            return fail(error, "events[" + std::to_string(e) + "] '" + ev.name + "': time outside [0, duration]");
        }
        if (e > 0 && event_less(ev, anim.events[e - 1])) {
            return fail(error, "events must be sorted by time, then name");
        }
    }
    return true;
}

} // namespace

f32 CookedAnim::frame_time(u32 frame) const {
    if (!(sample_rate > 0.f)) {
        return 0.f;
    }
    return std::min(static_cast<f32>(static_cast<double>(frame) / sample_rate), duration);
}

u64 skeleton_hash_of_names(const std::vector<std::string>& names) {
    u64 hash = 0xCBF29CE484222325ull;
    for (const std::string& name : names) {
        for (const char ch : name) {
            hash ^= static_cast<u8>(ch);
            hash *= 0x100000001B3ull;
        }
        hash ^= 0u;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

u64 skeleton_hash_of(const CookedAnim& anim) {
    std::vector<std::string> names;
    names.reserve(anim.joints.size());
    for (const CookedAnimJoint& joint : anim.joints) {
        names.push_back(joint.name);
    }
    return skeleton_hash_of_names(names);
}

u64 skeleton_hash_of(const Skeleton& skeleton) {
    std::vector<std::string> names;
    names.reserve(skeleton.bones.size());
    for (const Bone& bone : skeleton.bones) {
        usize len = 0;
        while (len < sizeof(bone.name) && bone.name[len] != '\0') {
            ++len;
        }
        names.emplace_back(bone.name, len);
    }
    return skeleton_hash_of_names(names);
}

u32 cooked_anim_frame_count(f32 duration, f32 sample_rate) {
    if (!(duration > 0.f) || !(sample_rate > 0.f)) {
        return 1u;
    }
    const double frames = std::ceil(static_cast<double>(duration) * sample_rate - 1e-4);
    return static_cast<u32>(std::max(0.0, std::min(frames, static_cast<double>(kMaxFrames)))) + 1u;
}

std::vector<u8> encode_cooked_anim(const CookedAnim& anim, std::string* error) {
    if (!validate(anim, error)) {
        return {};
    }
    Writer w;
    w.u32v(kCookedAnimMagic);
    w.u32v(kCookedAnimVersion);
    w.u32v((anim.looping ? kCookedAnimFlagLooping : 0u) | (anim.has_root_motion ? kCookedAnimFlagRootMotion : 0u));
    w.u32v(anim.frame_count);
    w.f32v(anim.sample_rate);
    w.f32v(anim.duration);
    w.u64v(skeleton_hash_of(anim));
    w.u32v(static_cast<u32>(anim.joints.size()));
    w.u32v(static_cast<u32>(anim.events.size()));
    w.str(anim.name);
    for (const CookedAnimJoint& joint : anim.joints) {
        const CookedTrackMode t = vec3_mode(joint.translation);
        const CookedTrackMode r = quat_mode(joint.rotation);
        const CookedTrackMode s = vec3_mode(joint.scale);
        w.str(joint.name);
        w.s32v(joint.parent);
        w.u8v(static_cast<u8>(t));
        w.u8v(static_cast<u8>(r));
        w.u8v(static_cast<u8>(s));
        w.u8v(0);
        write_vec3_track(w, joint.translation, t);
        write_quat_track(w, joint.rotation, r);
        write_vec3_track(w, joint.scale, s);
    }
    if (anim.has_root_motion) {
        w.u32v(anim.root_joint);
        for (const CookedAnimRootSample& s : anim.root_motion) {
            w.f32v(s.dx);
            w.f32v(s.dy);
            w.f32v(s.dz);
            w.f32v(s.dyaw);
        }
    }
    for (const CookedAnimEvent& ev : anim.events) {
        w.f32v(ev.time);
        w.str(ev.name);
    }
    w.u64v(fnv1a64(w.bytes.data(), w.bytes.size()));
    return std::move(w.bytes);
}

bool read_cooked_anim(const u8* data, usize size, CookedAnim& out, std::string* error) {
    out = CookedAnim{};
    if (data == nullptr || size < 48u + 8u) {
        return fail(error, "file too small for a .fuseanim");
    }
    u64 trailer = 0;
    for (u32 i = 0; i < 8; ++i) {
        trailer |= static_cast<u64>(data[size - 8u + i]) << (8u * i);
    }
    if (trailer != fnv1a64(data, size - 8u)) {
        return fail(error, "FNV-1a trailer mismatch (corrupt or truncated)");
    }
    Reader r(data, size - 8u);
    CookedAnim anim;
    if (r.u32v() != kCookedAnimMagic) {
        return fail(error, "bad magic (not FANM)");
    }
    const u32 version = r.u32v();
    if (version != kCookedAnimVersion) {
        return fail(error, "unsupported .fuseanim version " + std::to_string(version));
    }
    const u32 flags = r.u32v();
    if ((flags & ~(kCookedAnimFlagLooping | kCookedAnimFlagRootMotion)) != 0u) {
        return fail(error, "unknown flag bits");
    }
    anim.looping = (flags & kCookedAnimFlagLooping) != 0u;
    anim.has_root_motion = (flags & kCookedAnimFlagRootMotion) != 0u;
    anim.frame_count = r.u32v();
    anim.sample_rate = r.f32v();
    anim.duration = r.f32v();
    anim.skeleton_hash = r.u64v();
    const u32 joint_count = r.u32v();
    const u32 event_count = r.u32v();
    anim.name = r.str();
    if (!r.ok() || anim.frame_count == 0u || anim.frame_count > kMaxFrames || joint_count == 0u ||
        joint_count > kMaxJoints || event_count > kMaxEvents) {
        return fail(error, "bad header counts");
    }
    anim.joints.resize(joint_count);
    for (CookedAnimJoint& joint : anim.joints) {
        joint.name = r.str();
        joint.parent = r.s32v();
        const u8 t = r.u8v();
        const u8 rot = r.u8v();
        const u8 s = r.u8v();
        if (r.u8v() != 0u || t > 2u || rot > 2u || s > 2u) {
            return fail(error, "joint '" + joint.name + "': bad track modes");
        }
        joint.translation_mode = static_cast<CookedTrackMode>(t);
        joint.rotation_mode = static_cast<CookedTrackMode>(rot);
        joint.scale_mode = static_cast<CookedTrackMode>(s);
        if (!read_vec3_track(r, joint.translation_mode, anim.frame_count, joint.translation) ||
            !read_quat_track(r, joint.rotation_mode, anim.frame_count, joint.rotation) ||
            !read_vec3_track(r, joint.scale_mode, anim.frame_count, joint.scale)) {
            return fail(error, "joint '" + joint.name + "': bad or truncated track");
        }
    }
    if (anim.has_root_motion) {
        anim.root_joint = r.u32v();
        if (!r.take(static_cast<usize>(anim.frame_count) * 16u)) {
            return fail(error, "truncated root motion");
        }
        anim.root_motion.resize(anim.frame_count);
        for (CookedAnimRootSample& s : anim.root_motion) {
            s.dx = r.f32v();
            s.dy = r.f32v();
            s.dz = r.f32v();
            s.dyaw = r.f32v();
        }
    }
    anim.events.resize(event_count);
    for (CookedAnimEvent& ev : anim.events) {
        ev.time = r.f32v();
        ev.name = r.str();
    }
    if (!r.ok()) {
        return fail(error, "truncated .fuseanim");
    }
    if (r.pos() != size - 8u) {
        return fail(error, "trailing bytes after the events");
    }
    if (!validate(anim, error)) {
        return false;
    }
    if (anim.skeleton_hash != skeleton_hash_of(anim)) {
        return fail(error, "skeleton hash does not match the joint names");
    }
    out = std::move(anim);
    return true;
}

bool read_cooked_anim_file(const std::string& path, CookedAnim& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out = CookedAnim{};
        return fail(error, "cannot open " + path);
    }
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return read_cooked_anim(bytes.data(), bytes.size(), out, error);
}

Skeleton cooked_anim_skeleton(const CookedAnim& anim) {
    Skeleton skeleton;
    skeleton.bones.resize(anim.joints.size());
    for (usize j = 0; j < anim.joints.size(); ++j) {
        Bone& bone = skeleton.bones[j];
        std::snprintf(bone.name, sizeof(bone.name), "%s", anim.joints[j].name.c_str());
        bone.parent_index = anim.joints[j].parent;
    }
    skeleton.bone_count = static_cast<u32>(skeleton.bones.size());
    return skeleton;
}

bool cooked_anim_to_clip(const CookedAnim& anim, const Skeleton* skeleton, AnimationClip& out, std::string* error,
                         u32* unmatched) {
    AnimationClip clip;
    std::snprintf(clip.name, sizeof(clip.name), "%s", anim.name.c_str());
    clip.duration = anim.duration;
    clip.sample_rate = anim.sample_rate;
    clip.looping = anim.looping;
    u32 missing = 0;
    std::vector<f32> times(anim.frame_count);
    for (u32 f = 0; f < anim.frame_count; ++f) {
        times[f] = anim.frame_time(f);
    }
    for (usize j = 0; j < anim.joints.size(); ++j) {
        const CookedAnimJoint& joint = anim.joints[j];
        if (joint.translation.empty() && joint.rotation.empty() && joint.scale.empty()) {
            continue;
        }
        AnimationClip::BoneChannels channels;
        if (skeleton == nullptr) {
            channels.bone_index = static_cast<u32>(j);
            const auto fill3 = [&](const std::vector<vec3>& src, KeyframeChannel& dst) {
                if (!src.empty()) {
                    dst.times = times;
                    dst.values_vec3 = src;
                }
            };
            fill3(joint.translation, channels.position);
            fill3(joint.scale, channels.scale);
            if (!joint.rotation.empty()) {
                channels.rotation.times = times;
                channels.rotation.values_quat = joint.rotation;
            }
            clip.bone_channels.push_back(std::move(channels));
            continue;
        }
        const s32 bone = skeleton->find_bone(joint.name.c_str());
        if (bone < 0 || static_cast<u32>(bone) >= skeleton->bones.size()) {
            ++missing;
            continue;
        }
        channels.bone_index = static_cast<u32>(bone);
        const mat4& bind = skeleton->bones[static_cast<usize>(bone)].local_transform;
        vec3 bindT{}, bindS{};
        quat bindR{};
        decompose_trs(bind, bindT, bindR, bindS);
        const mat4 inverseBind = mat4_inverse_affine(bind);
        channels.position.times = times;
        channels.rotation.times = times;
        channels.scale.times = times;
        channels.position.values_vec3.resize(anim.frame_count);
        channels.rotation.values_quat.resize(anim.frame_count);
        channels.scale.values_vec3.resize(anim.frame_count);
        for (u32 f = 0; f < anim.frame_count; ++f) {
            const vec3 t = joint.translation.empty() ? bindT : joint.translation[f];
            const quat r = joint.rotation.empty() ? bindR : joint.rotation[f];
            const vec3 s = joint.scale.empty() ? bindS : joint.scale[f];
            const mat4 local = mat4_multiply(inverseBind, mat4_from_trs(t, quat_normalize(r), s));
            decompose_trs(local, channels.position.values_vec3[f], channels.rotation.values_quat[f],
                          channels.scale.values_vec3[f]);
        }
        clip.bone_channels.push_back(std::move(channels));
    }
    if (unmatched != nullptr) {
        *unmatched = missing;
    }
    if (skeleton != nullptr && clip.bone_channels.empty() && !anim.joints.empty()) {
        return fail(error, "no clip joint matches a skeleton bone");
    }
    out = std::move(clip);
    return true;
}

bool load_cooked_anim_clip(const std::string& path, const Skeleton* skeleton, AnimationClip& out, CookedAnim* anim_out,
                           std::string* error) {
    CookedAnim anim;
    if (!read_cooked_anim_file(path, anim, error)) {
        return false;
    }
    if (!cooked_anim_to_clip(anim, skeleton, out, error)) {
        return false;
    }
    if (anim_out != nullptr) {
        *anim_out = std::move(anim);
    }
    return true;
}

} // namespace fuse::animation
