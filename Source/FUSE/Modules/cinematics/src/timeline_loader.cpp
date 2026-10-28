#include <fuse/cinematics/timeline_loader.hpp>

#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/motion_track.hpp>
#include <fuse/cinematics/seq_import.hpp>
#include <fuse/cinematics/sprite_track.hpp>

#include <cctype>
#include <sstream>
#include <utility>

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

std::string formatSeqImportError(const SeqImportError& error) {
    return "line " + std::to_string(error.line) + ": " + error.message;
}

int firstEmbeddedNulLine(const std::string& text) {
    int line = 1;
    for (const char ch : text) {
        if (ch == '\0') {
            return line;
        }
        if (ch == '\n') {
            ++line;
        }
    }
    return 0;
}

/// A line that opens like JSON, XML, or another quoted document is not the `.seq` dialect.
/// Tokens inside a real seq statement (for example an apostrophe in an id) stay on the seq path.
bool isForeignSeqDialectLine(const std::string& line) {
    const char head = line[0];
    if (head == '{' || head == '}' || head == '[' || head == ']' || head == '<' || head == '>' || head == '"' ||
        head == '\'') {
        return true;
    }
    return line.find("\":") != std::string::npos;
}

SeqImportResult failImport(int line, SeqImportErrorKind kind, std::string message) {
    SeqImportResult failed;
    failed.errors.push_back(SeqImportError{line, kind, std::move(message)});
    return failed;
}

} // namespace

SeqImportResult import_seq(const std::string& text) {
    if (trim(text).empty()) {
        return failImport(1, SeqImportErrorKind::EmptyFile, "empty file");
    }

    if (const int nulLine = firstEmbeddedNulLine(text); nulLine != 0) {
        return failImport(nulLine, SeqImportErrorKind::NotSeqDialect, "not the .seq dialect");
    }

    Timeline timeline;
    TimelineMs durationMs = 0;
    bool sawDuration = false;

    std::stringstream stream(text);
    std::string line;
    int lineNumber = 0;
    TrackGroup* group = nullptr;

    while (std::getline(stream, line)) {
        ++lineNumber;
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (isForeignSeqDialectLine(line)) {
            return failImport(lineNumber, SeqImportErrorKind::NotSeqDialect, "not the .seq dialect");
        }

        std::stringstream lineStream(line);
        std::string keyword;
        if (!(lineStream >> keyword)) {
            continue;
        }

        if (keyword.rfind("duration_ms", 0) == 0) {
            const std::size_t eq = keyword.find('=');
            if (eq == std::string::npos) {
                return failImport(lineNumber, SeqImportErrorKind::InvalidSyntax, "expected duration_ms=N");
            }
            if (!parseTimelineMs(keyword.substr(eq + 1), durationMs)) {
                return failImport(lineNumber, SeqImportErrorKind::InvalidSyntax, "invalid duration_ms");
            }
            sawDuration = true;
            continue;
        }

        if (group == nullptr) {
            group = &timeline.add_group("AssetDirector");
        }

        if (keyword == "sprite") {
            std::string targetId;
            if (!(lineStream >> targetId)) {
                return failImport(lineNumber, SeqImportErrorKind::InvalidSyntax, "sprite missing target id");
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
                    return failImport(lineNumber, SeqImportErrorKind::BadKeyframe, "sprite keyframe needs t,x,y,a");
                }

                SpriteKeyframe keyframe{};
                TimelineMs timeMs = 0;
                if (!parseTimelineMs(parts[0], timeMs) || !parseFloat(parts[1], keyframe.x) ||
                    !parseFloat(parts[2], keyframe.y) || !parseFloat(parts[3], keyframe.alpha)) {
                    return failImport(lineNumber, SeqImportErrorKind::BadKeyframe, "invalid sprite keyframe");
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
                    return failImport(lineNumber, SeqImportErrorKind::BadKeyframe,
                                      "camera keyframe needs t,px,py,pz,fov");
                }

                CameraKeyframe keyframe{};
                TimelineMs timeMs = 0;
                if (!parseTimelineMs(parts[0], timeMs) || !parseFloat(parts[1], keyframe.position.x) ||
                    !parseFloat(parts[2], keyframe.position.y) || !parseFloat(parts[3], keyframe.position.z) ||
                    !parseFloat(parts[4], keyframe.field_of_view)) {
                    return failImport(lineNumber, SeqImportErrorKind::BadKeyframe, "invalid camera keyframe");
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
                return failImport(lineNumber, SeqImportErrorKind::InvalidSyntax, "actor event missing fields");
            }

            ActorTrack* existingTrack = findActorTrackById(*group, actorId);
            ActorTrack& track = existingTrack != nullptr ? *existingTrack : group->add_actor_track(actorId);
            track.set_actor_id(actorId);

            ActorEvent event{};
            event.actor_id = actorId;
            if (eventKind == "mount") {
                std::string mountPoint;
                if (!(lineStream >> mountPoint) || !parseTimelineMs(timeOrMount, event.time_ms)) {
                    return failImport(lineNumber, SeqImportErrorKind::InvalidSyntax,
                                      "actor mount needs time and mount_point");
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
                    return failImport(lineNumber, SeqImportErrorKind::InvalidSyntax, "actor unmount needs time");
                }
                event.kind = ActorEventKind::Unmount;
            } else {
                return failImport(lineNumber, SeqImportErrorKind::InvalidSyntax, "unknown actor event: " + eventKind);
            }

            track.add_actor_event(event);
            track.sort_actor_events();
            continue;
        }

        if (keyword == "motion") {
            std::string pathId;
            if (!(lineStream >> pathId)) {
                return failImport(lineNumber, SeqImportErrorKind::InvalidSyntax, "motion missing path id");
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
                    return failImport(lineNumber, SeqImportErrorKind::BadKeyframe, "motion waypoint needs t,x,y,z");
                }

                MotionWaypoint waypoint{};
                TimelineMs timeMs = 0;
                if (!parseTimelineMs(parts[0], timeMs) || !parseFloat(parts[1], waypoint.position.x) ||
                    !parseFloat(parts[2], waypoint.position.y) || !parseFloat(parts[3], waypoint.position.z)) {
                    return failImport(lineNumber, SeqImportErrorKind::BadKeyframe, "invalid motion waypoint");
                }
                waypoint.time_ms = timeMs;
                track.path().add_waypoint(waypoint);
            }
            track.path().sort_waypoints();
            continue;
        }

        return failImport(lineNumber, SeqImportErrorKind::UnknownTrack, "unknown track: " + keyword);
    }

    if (!sawDuration) {
        return failImport(lineNumber > 0 ? lineNumber : 1, SeqImportErrorKind::InvalidSyntax,
                          "timeline asset missing duration_ms");
    }

    timeline.playhead().set_duration_ms(durationMs);
    SeqImportResult result;
    result.ok = true;
    result.timeline = std::move(timeline);
    return result;
}

bool load_timeline_from_asset(const std::string& text, Timeline& outTimeline, std::string* errorOut) {
    SeqImportResult imported = import_seq(text);
    if (!imported.ok) {
        outTimeline = Timeline{};
        if (errorOut != nullptr) {
            if (!imported.errors.empty()) {
                *errorOut = formatSeqImportError(imported.errors.front());
            } else {
                *errorOut = "seq import failed";
            }
        }
        return false;
    }

    outTimeline = std::move(imported.timeline);
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
