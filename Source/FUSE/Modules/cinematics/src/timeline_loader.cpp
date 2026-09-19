#include <fuse/cinematics/timeline_loader.hpp>

#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/sprite_track.hpp>

#include <cctype>
#include <sstream>

namespace fuse::cinematics {

namespace {

std::string trim(const std::string& input) {
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(start, end - start);
}

bool parseTimelineMs(const std::string& token, TimelineMs& out) {
    try {
        out = static_cast<TimelineMs>(std::stoll(token));
        return true;
    } catch (...) {
        return false;
    }
}

bool parseFloat(const std::string& token, float& out) {
    try {
        out = std::stof(token);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool load_timeline_from_asset(const std::string& text, Timeline& outTimeline, std::string* errorOut) {
    outTimeline = Timeline{};
    TimelineMs durationMs = 0;
    bool sawDuration = false;

    std::stringstream stream(text);
    std::string line;
    TrackGroup* group = nullptr;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::stringstream lineStream(line);
        std::string keyword;
        if (!(lineStream >> keyword)) {
            continue;
        }

        if (keyword.rfind("duration_ms", 0) == 0) {
            const std::size_t eq = keyword.find('=');
            if (eq == std::string::npos) {
                if (errorOut) {
                    *errorOut = "expected duration_ms=N";
                }
                return false;
            }
            if (!parseTimelineMs(keyword.substr(eq + 1), durationMs)) {
                if (errorOut) {
                    *errorOut = "invalid duration_ms";
                }
                return false;
            }
            sawDuration = true;
            continue;
        }

        if (group == nullptr) {
            group = &outTimeline.add_group("AssetDirector");
        }

        if (keyword == "sprite") {
            std::string targetId;
            if (!(lineStream >> targetId)) {
                if (errorOut) {
                    *errorOut = "sprite missing target id";
                }
                return false;
            }

            SpriteTrack& track = group->add_sprite_track(targetId);
            track.set_target_sprite_id(targetId);

            std::string keyframeToken;
            while (lineStream >> keyframeToken) {
                std::stringstream kfStream(keyframeToken);
                std::string part;
                std::vector<std::string> parts;
                while (std::getline(kfStream, part, ',')) {
                    parts.push_back(part);
                }
                if (parts.size() != 4) {
                    if (errorOut) {
                        *errorOut = "sprite keyframe needs t,x,y,a";
                    }
                    return false;
                }

                SpriteKeyframe keyframe{};
                TimelineMs timeMs = 0;
                if (!parseTimelineMs(parts[0], timeMs) || !parseFloat(parts[1], keyframe.x) ||
                    !parseFloat(parts[2], keyframe.y) || !parseFloat(parts[3], keyframe.alpha)) {
                    if (errorOut) {
                        *errorOut = "invalid sprite keyframe";
                    }
                    return false;
                }
                keyframe.time_ms = timeMs;
                track.add_keyframe(keyframe);
            }
            track.sort_keyframes();
            continue;
        }

        if (keyword == "camera") {
            CameraTrack& track = group->add_camera_track("asset_camera");

            std::string keyframeToken;
            while (lineStream >> keyframeToken) {
                std::stringstream kfStream(keyframeToken);
                std::string part;
                std::vector<std::string> parts;
                while (std::getline(kfStream, part, ',')) {
                    parts.push_back(part);
                }
                if (parts.size() != 5) {
                    if (errorOut) {
                        *errorOut = "camera keyframe needs t,px,py,pz,fov";
                    }
                    return false;
                }

                CameraKeyframe keyframe{};
                TimelineMs timeMs = 0;
                if (!parseTimelineMs(parts[0], timeMs) || !parseFloat(parts[1], keyframe.position.x) ||
                    !parseFloat(parts[2], keyframe.position.y) || !parseFloat(parts[3], keyframe.position.z) ||
                    !parseFloat(parts[4], keyframe.field_of_view)) {
                    if (errorOut) {
                        *errorOut = "invalid camera keyframe";
                    }
                    return false;
                }
                keyframe.time_ms = timeMs;
                track.add_keyframe(keyframe);
            }
            track.sort_keyframes();
            continue;
        }

        if (keyword == "actor") {
            std::string actorId;
            std::string eventKind;
            std::string timeOrMount;
            if (!(lineStream >> actorId >> eventKind >> timeOrMount)) {
                if (errorOut) {
                    *errorOut = "actor event missing fields";
                }
                return false;
            }

            ActorTrack& track = group->add_actor_track(actorId);
            track.set_actor_id(actorId);

            ActorEvent event{};
            event.actor_id = actorId;
            if (eventKind == "mount") {
                std::string mountPoint;
                if (!(lineStream >> mountPoint) || !parseTimelineMs(timeOrMount, event.time_ms)) {
                    if (errorOut) {
                        *errorOut = "actor mount needs time and mount_point";
                    }
                    return false;
                }
                event.kind = ActorEventKind::Mount;
                event.mount_point = mountPoint;
            } else if (eventKind == "unmount") {
                if (!parseTimelineMs(timeOrMount, event.time_ms)) {
                    if (errorOut) {
                        *errorOut = "actor unmount needs time";
                    }
                    return false;
                }
                event.kind = ActorEventKind::Unmount;
            } else {
                if (errorOut) {
                    *errorOut = "unknown actor event: " + eventKind;
                }
                return false;
            }

            track.add_actor_event(event);
            track.sort_actor_events();
            continue;
        }

        if (errorOut) {
            *errorOut = "unknown timeline keyword: " + keyword;
        }
        return false;
    }

    if (!sawDuration) {
        if (errorOut) {
            *errorOut = "timeline asset missing duration_ms";
        }
        return false;
    }

    outTimeline.playhead().set_duration_ms(durationMs);
    return true;
}

} // namespace fuse::cinematics
