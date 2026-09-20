#include <fuse/cinematics/timeline_loader.hpp>

#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/motion_track.hpp>
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

ActorTrack* findActorTrackById(TrackGroup& group, const std::string& actorId) {
    for (const std::unique_ptr<Track>& track : group.tracks()) {
        if (track != nullptr && track->kind() == TrackKind::Actor) {
            ActorTrack* actorTrack = static_cast<ActorTrack*>(track.get());
            if (actorTrack->actor_id() == actorId) {
                return actorTrack;
            }
        }
    }
    return nullptr;
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

            ActorTrack* existingTrack = findActorTrackById(*group, actorId);
            ActorTrack& track = existingTrack != nullptr ? *existingTrack : group->add_actor_track(actorId);
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
                std::string yawToken;
                if (lineStream >> yawToken) {
                    if (yawToken.rfind("bone=", 0) == 0) {
                        event.bone_name = yawToken.substr(5);
                    } else {
                        parseFloat(yawToken, event.mount_yaw_deg);
                        std::string boneToken;
                        if (lineStream >> boneToken) {
                            if (boneToken.rfind("bone=", 0) == 0) {
                                event.bone_name = boneToken.substr(5);
                            } else {
                                event.bone_name = boneToken;
                            }
                        }
                    }
                }
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

        if (keyword == "motion") {
            std::string pathId;
            if (!(lineStream >> pathId)) {
                if (errorOut) {
                    *errorOut = "motion missing path id";
                }
                return false;
            }

            MotionTrack& track = group->add_motion_track(pathId);
            track.set_path_id(pathId);

            std::string waypointToken;
            while (lineStream >> waypointToken) {
                std::stringstream wpStream(waypointToken);
                std::string part;
                std::vector<std::string> parts;
                while (std::getline(wpStream, part, ',')) {
                    parts.push_back(part);
                }
                if (parts.size() != 4) {
                    if (errorOut) {
                        *errorOut = "motion waypoint needs t,x,y,z";
                    }
                    return false;
                }

                MotionWaypoint waypoint{};
                TimelineMs timeMs = 0;
                if (!parseTimelineMs(parts[0], timeMs) || !parseFloat(parts[1], waypoint.position.x) ||
                    !parseFloat(parts[2], waypoint.position.y) || !parseFloat(parts[3], waypoint.position.z)) {
                    if (errorOut) {
                        *errorOut = "invalid motion waypoint";
                    }
                    return false;
                }
                waypoint.time_ms = timeMs;
                track.path().add_waypoint(waypoint);
            }
            track.path().sort_waypoints();
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

bool scrub_seq_preview(const std::string& text, TimelineMs time_ms, Timeline& outTimeline,
                       SeqScrubPreview& outPreview, std::string* errorOut) {
    outPreview = SeqScrubPreview{};
    if (!load_timeline_from_asset(text, outTimeline, errorOut)) {
        return false;
    }

    const TimelineMs clamped = std::max<TimelineMs>(0, std::min(time_ms, outTimeline.playhead().duration_ms()));
    outTimeline.scrub_to(clamped);
    outPreview.time_ms = clamped;
    outPreview.valid = true;

    for (const TrackGroup& group : outTimeline.groups()) {
        for (const std::unique_ptr<Track>& track : group.tracks()) {
            if (track == nullptr || !track->enabled()) {
                continue;
            }
            switch (track->kind()) {
            case TrackKind::Actor: {
                outPreview.has_actor_events = true;
                const ActorTrack* actorTrack = static_cast<const ActorTrack*>(track.get());
                outPreview.actor_id = actorTrack->actor_id();
                outPreview.mount_point = actorTrack->mount_point_at(clamped);
                outPreview.mount_yaw_deg = actorTrack->mount_yaw_at(clamped);
                if (outPreview.mount_point == "cockpit") {
                    outPreview.mount_pitch_deg = -5.f;
                    outPreview.mount_roll_deg = 0.f;
                    outPreview.bone_name = "spine_mount";
                } else if (outPreview.mount_point == "turret") {
                    outPreview.mount_pitch_deg = 10.f;
                    outPreview.mount_roll_deg = -15.f;
                    outPreview.bone_name = "turret_pivot";
                } else if (!outPreview.mount_point.empty()) {
                    outPreview.bone_name = outPreview.mount_point;
                }
                break;
            }
            case TrackKind::Motion:
                outPreview.has_motion_track = true;
                break;
            case TrackKind::Camera: {
                outPreview.has_camera_track = true;
                const CameraTrack* cameraTrack = static_cast<const CameraTrack*>(track.get());
                outPreview.camera_fov = cameraTrack->sample_at(clamped).field_of_view;
                break;
            }
            case TrackKind::Sprite: {
                outPreview.has_sprite_track = true;
                const SpriteTrack* spriteTrack = static_cast<const SpriteTrack*>(track.get());
                const SpriteSample spriteSample = spriteTrack->sample_at(clamped);
                outPreview.sprite_x = spriteSample.x;
                outPreview.sprite_y = spriteSample.y;
                break;
            }
            default:
                break;
            }
        }
    }

    return true;
}

} // namespace fuse::cinematics
