#include <fuse/editor/editor_timeline.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fuse::editor {
namespace {

constexpr f32 kDeg = 57.29577951308232f;

int channelIndex(const std::string& channel) {
    for (size_t i = 0; i < kTimelineChannels.size(); ++i) {
        if (channel == kTimelineChannels[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

void EditorTimeline::clear() {
    m_tracks.clear();
    m_durationMs = 5000;
    m_playheadMs = 0;
    touch();
}

void EditorTimeline::setPlayheadMs(cinematics::TimelineMs ms) {
    m_playheadMs = std::clamp<cinematics::TimelineMs>(ms, 0, m_durationMs);
}

u32 EditorTimeline::entityOf(const cinematics::PropertyTrack& track) {
    return static_cast<u32>(std::strtoul(track.target_object_id().c_str(), nullptr, 10));
}

int EditorTimeline::findTrack(u32 entityIndex, const std::string& channel) const {
    const std::string id = std::to_string(entityIndex);
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].target_object_id() == id && m_tracks[i].property_path() == channel) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int EditorTimeline::ensureTrack(u32 entityIndex, const std::string& channel) {
    const int found = findTrack(entityIndex, channel);
    if (found >= 0) {
        return found;
    }
    cinematics::PropertyTrack track("Entity #" + std::to_string(entityIndex) + " " + channel);
    track.set_target_object_id(std::to_string(entityIndex));
    track.set_property_path(channel);
    m_tracks.push_back(std::move(track));
    touch();
    return static_cast<int>(m_tracks.size()) - 1;
}

u32 EditorTimeline::keyCount() const {
    u32 n = 0;
    for (const auto& t : m_tracks) {
        n += static_cast<u32>(t.keyframes().size());
    }
    return n;
}

std::array<f32, 3> EditorTimeline::eulerDeg(const ecs::quat& q) {
    // Intrinsic X-Y-Z (q = qX * qY * qZ).
    const f32 sy = std::clamp(2.f * (q.w * q.y + q.x * q.z), -1.f, 1.f);
    const f32 y = std::asin(sy);
    const f32 x = std::atan2(2.f * (q.w * q.x - q.y * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y));
    const f32 z = std::atan2(2.f * (q.w * q.z - q.x * q.y), 1.f - 2.f * (q.y * q.y + q.z * q.z));
    return {x * kDeg, y * kDeg, z * kDeg};
}

ecs::quat EditorTimeline::fromEulerDeg(f32 x, f32 y, f32 z) {
    const f32 cx = std::cos(x / kDeg * 0.5f), sx = std::sin(x / kDeg * 0.5f);
    const f32 cy = std::cos(y / kDeg * 0.5f), sy = std::sin(y / kDeg * 0.5f);
    const f32 cz = std::cos(z / kDeg * 0.5f), sz = std::sin(z / kDeg * 0.5f);
    // qX * qY * qZ
    return ecs::quat{sx * cy * cz + cx * sy * sz, cx * sy * cz - sx * cy * sz, cx * cy * sz + sx * sy * cz,
                     cx * cy * cz - sx * sy * sz};
}

f32 EditorTimeline::channelValue(const ecs::Transform& t, const std::string& channel) {
    const int c = channelIndex(channel);
    if (c >= 3 && c <= 5) {
        return eulerDeg(t.rotation)[static_cast<size_t>(c - 3)];
    }
    switch (c) {
    case 0: return t.position.x;
    case 1: return t.position.y;
    case 2: return t.position.z;
    case 6: return t.scale.x;
    case 7: return t.scale.y;
    case 8: return t.scale.z;
    default: return 0.f;
    }
}

void EditorTimeline::setChannels(ecs::Transform& t, const std::array<f32, 9>& v, const std::array<bool, 9>& set) {
    if (set[0]) t.position.x = v[0];
    if (set[1]) t.position.y = v[1];
    if (set[2]) t.position.z = v[2];
    if (set[3] || set[4] || set[5]) {
        std::array<f32, 3> e = eulerDeg(t.rotation);
        for (int i = 0; i < 3; ++i) {
            if (set[static_cast<size_t>(3 + i)]) {
                e[static_cast<size_t>(i)] = v[static_cast<size_t>(3 + i)];
            }
        }
        t.rotation = fromEulerDeg(e[0], e[1], e[2]);
    }
    if (set[6]) t.scale.x = v[6];
    if (set[7]) t.scale.y = v[7];
    if (set[8]) t.scale.z = v[8];
    t.dirty = true;
}

bool EditorTimeline::keyTransform(const ecs::Registry& registry, ecs::EntityID entity,
                                  cinematics::TimelineMs timeMs) {
    if (!registry.alive(entity)) {
        return false;
    }
    const ecs::Transform* t = registry.get<ecs::Transform>(entity);
    if (t == nullptr) {
        return false;
    }
    for (const char* channel : kTimelineChannels) {
        cinematics::PropertyTrack& track = m_tracks[static_cast<size_t>(ensureTrack(entity.index, channel))];
        auto& keys = track.keyframes();
        keys.erase(std::remove_if(keys.begin(), keys.end(),
                                  [&](const cinematics::PropertyKeyframe& k) { return k.time_ms == timeMs; }),
                   keys.end());
        track.add_keyframe({timeMs, channelValue(*t, channel)});
        track.sort_keyframes();
    }
    if (timeMs > m_durationMs) {
        m_durationMs = timeMs;
    }
    touch();
    return true;
}

int EditorTimeline::moveKey(int track, int key, cinematics::TimelineMs timeMs) {
    if (track < 0 || track >= static_cast<int>(m_tracks.size())) {
        return -1;
    }
    auto& keys = m_tracks[static_cast<size_t>(track)].keyframes();
    if (key < 0 || key >= static_cast<int>(keys.size())) {
        return -1;
    }
    timeMs = std::clamp<cinematics::TimelineMs>(timeMs, 0, m_durationMs);
    const f32 value = keys[static_cast<size_t>(key)].value;
    keys.erase(keys.begin() + key);
    // Landing on another key replaces it.
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [&](const cinematics::PropertyKeyframe& k) { return k.time_ms == timeMs; }),
               keys.end());
    keys.push_back({timeMs, value});
    m_tracks[static_cast<size_t>(track)].sort_keyframes();
    touch();
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].time_ms == timeMs) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool EditorTimeline::deleteKey(int track, int key) {
    if (track < 0 || track >= static_cast<int>(m_tracks.size())) {
        return false;
    }
    auto& keys = m_tracks[static_cast<size_t>(track)].keyframes();
    if (key < 0 || key >= static_cast<int>(keys.size())) {
        return false;
    }
    keys.erase(keys.begin() + key);
    if (keys.empty()) {
        m_tracks.erase(m_tracks.begin() + track);
    }
    touch();
    return true;
}

u32 EditorTimeline::apply(ecs::Registry& registry, cinematics::TimelineMs timeMs) const {
    // Gather per entity so rotation channels combine into one quaternion write.
    struct Pending {
        u32 entity;
        std::array<f32, 9> values{};
        std::array<bool, 9> set{};
    };
    std::vector<Pending> pending;
    for (const auto& track : m_tracks) {
        const int c = channelIndex(track.property_path());
        if (c < 0 || track.keyframes().empty()) {
            continue;
        }
        const u32 e = entityOf(track);
        auto it = std::find_if(pending.begin(), pending.end(), [&](const Pending& p) { return p.entity == e; });
        if (it == pending.end()) {
            pending.push_back({e, {}, {}});
            it = pending.end() - 1;
        }
        it->values[static_cast<size_t>(c)] = track.sample_at(timeMs);
        it->set[static_cast<size_t>(c)] = true;
    }
    u32 touched = 0;
    for (const Pending& p : pending) {
        registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
            if (id.index == p.entity) {
                setChannels(t, p.values, p.set);
                ++touched;
            }
        });
    }
    return touched;
}

bool EditorTimeline::save(const std::string& path, std::string* error) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error != nullptr) {
            *error = "cannot write " + path;
        }
        return false;
    }
    out << "FUSEEDTL 1\nduration " << m_durationMs << "\n";
    for (const auto& t : m_tracks) {
        out << "track " << t.target_object_id() << ' ' << t.property_path() << ' ' << t.keyframes().size() << '\n';
        for (const auto& k : t.keyframes()) {
            out << "key " << k.time_ms << ' ' << k.value << '\n';
        }
    }
    return static_cast<bool>(out);
}

bool EditorTimeline::load(const std::string& path, std::string* error) {
    clear();
    std::ifstream in(path);
    if (!in) {
        if (error != nullptr) {
            *error = "no timeline " + path;
        }
        return false;
    }
    std::string magic;
    int version = 0;
    in >> magic >> version;
    if (magic != "FUSEEDTL" || version != 1) {
        if (error != nullptr) {
            *error = "not a FUSEEDTL 1 file";
        }
        return false;
    }
    std::string word;
    while (in >> word) {
        if (word == "duration") {
            cinematics::TimelineMs d = 0;
            in >> d;
            setDurationMs(d);
        } else if (word == "track") {
            std::string id, channel;
            size_t n = 0;
            in >> id >> channel >> n;
            const int ti = ensureTrack(static_cast<u32>(std::strtoul(id.c_str(), nullptr, 10)), channel);
            for (size_t i = 0; i < n && in >> word && word == "key"; ++i) {
                cinematics::PropertyKeyframe k;
                in >> k.time_ms >> k.value;
                m_tracks[static_cast<size_t>(ti)].add_keyframe(k);
            }
            m_tracks[static_cast<size_t>(ti)].sort_keyframes();
        }
    }
    touch();
    return true;
}

bool TimelineEditCommand::merge(const UndoCommand& other) {
    const auto* o = dynamic_cast<const TimelineEditCommand*>(&other);
    if (o == nullptr || !m_mergeable || !o->m_mergeable || &o->m_timeline != &m_timeline ||
        o->m_description != m_description) {
        return false;
    }
    m_after = o->m_after;
    return true;
}

} // namespace fuse::editor
