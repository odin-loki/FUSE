#include <fuse/cinematics/vactor_bridge.hpp>

#include <fuse/cinematics/camera_track.hpp>
#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/cinematics/motion_track.hpp>
#include <fuse/cinematics/sprite_track.hpp>

namespace fuse::cinematics {

namespace {

const ActorTrack* find_first_actor_track(const Timeline& timeline) {
    for (const TrackGroup& group : timeline.groups()) {
        for (const std::unique_ptr<Track>& track : group.tracks()) {
            if (track != nullptr && track->enabled() && track->kind() == TrackKind::Actor) {
                return static_cast<const ActorTrack*>(track.get());
            }
        }
    }
    return nullptr;
}

} // namespace

void VActorBridge::bind(const std::string& actor_id, fuse::SceneObject3D* object) {
    BoundActorState& state = m_actors[actor_id];
    state.object = object;
    if (object != nullptr) {
        state.baseX = object->x();
        state.baseY = object->y();
        state.baseZ = object->z();
    }
}

void VActorBridge::apply_mount(const std::string& actor_id, const std::string& mount_point) {
    m_mountPoints[actor_id] = mount_point;
    ++m_mountCount;
}

void VActorBridge::apply_unmount(const std::string& actor_id) {
    m_mountPoints.erase(actor_id);
    const auto actorIt = m_actors.find(actor_id);
    if (actorIt != m_actors.end()) {
        actorIt->second.mounted = false;
        actorIt->second.offset = {};
        if (actorIt->second.object != nullptr) {
            actorIt->second.object->setPosition(actorIt->second.baseX,
                                                actorIt->second.baseY);
            actorIt->second.object->setZ(actorIt->second.baseZ);
            actorIt->second.object->setYawDeg(0.f);
        }
    }
    ++m_unmountCount;
}

const std::string& VActorBridge::mount_point_for(const std::string& actor_id) const {
    const auto it = m_mountPoints.find(actor_id);
    if (it != m_mountPoints.end()) {
        return it->second;
    }
    static const std::string kEmpty;
    return kEmpty;
}

ShapeBaseMountOffset VActorBridge::mount_offset_for(const std::string& mount_point) const {
    if (mount_point == "cockpit") {
        return {0.f, 0.f, 1.5f, 15.f};
    }
    if (mount_point == "vehicle_seat") {
        return {0.f, 0.5f, 0.75f, 0.f};
    }
    if (mount_point == "turret") {
        return {0.f, 1.25f, 2.f, 90.f};
    }
    return {};
}

void VActorBridge::apply_shapebase_attach(const std::string& actor_id,
                                          const std::string& mount_point,
                                          float mount_yaw_deg) {
    apply_mount(actor_id, mount_point);

    BoundActorState& state = m_actors[actor_id];
    if (state.object != nullptr) {
        state.baseX = state.object->x();
        state.baseY = state.object->y();
        state.baseZ = state.object->z();
    }

    state.offset = mount_offset_for(mount_point);
    state.offset.yaw_deg += mount_yaw_deg;
    state.mounted = true;
    ++m_shapebaseAttachCount;
    sync_bound_objects();
}

void VActorBridge::sync_bound_objects() {
    for (auto& entry : m_actors) {
        BoundActorState& state = entry.second;
        if (state.object == nullptr || !state.mounted) {
            continue;
        }

        state.object->setPosition(state.baseX + state.offset.x + state.motionX,
                                  state.baseY + state.offset.y + state.motionY);
        state.object->setZ(state.baseZ + state.offset.z + state.motionZ);
        state.object->setYawDeg(state.offset.yaw_deg);
    }
    ++m_syncCount;
}

const MotionTrack* find_first_motion_track(const Timeline& timeline) {
    for (const TrackGroup& group : timeline.groups()) {
        for (const std::unique_ptr<Track>& track : group.tracks()) {
            if (track != nullptr && track->enabled() && track->kind() == TrackKind::Motion) {
                return static_cast<const MotionTrack*>(track.get());
            }
        }
    }
    return nullptr;
}

void VActorBridge::sync_motion_from_timeline(const Timeline& timeline) {
    const MotionTrack* motionTrack = find_first_motion_track(timeline);
    if (motionTrack == nullptr) {
        return;
    }

    const MotionSample sample = motionTrack->sample_at(timeline.playhead().time_ms());
    for (auto& entry : m_actors) {
        BoundActorState& state = entry.second;
        if (state.object == nullptr || !state.mounted) {
            continue;
        }
        state.motionX = sample.position.x * 0.01f;
        state.motionY = sample.position.y * 0.01f;
        state.motionZ = sample.position.z * 0.01f;
    }

    sync_bound_objects();
    ++m_motionSyncCount;
}

void drain_actor_cues(const Timeline& timeline, VActorBridge& bridge, TimelineMs since_ms) {
    const ActorTrack* actorTrack = find_first_actor_track(timeline);
    if (actorTrack == nullptr) {
        return;
    }

    const TimelineMs time_ms = timeline.playhead().time_ms();
    for (const ActorEvent& event : actorTrack->actor_events()) {
        if (event.time_ms <= since_ms || event.time_ms > time_ms) {
            continue;
        }
        if (event.kind == ActorEventKind::Mount) {
            bridge.apply_shapebase_attach(event.actor_id, event.mount_point, event.mount_yaw_deg);
        } else {
            bridge.apply_unmount(event.actor_id);
        }
    }
}

Timeline make_outpost_intro_30s_stub() {
    Timeline timeline;
    timeline.playhead().set_duration_ms(30'000);

    TrackGroup& group = timeline.add_group("OutpostIntro");

    SpriteTrack& spriteTrack = group.add_sprite_track("outpost_hud");
    spriteTrack.set_target_sprite_id("hud_sprite");
    spriteTrack.add_keyframe({0, -20.f, 0.f, 1.f});
    spriteTrack.add_keyframe({15'000, 0.f, 10.f, 1.f});
    spriteTrack.add_keyframe({30'000, 40.f, 20.f, 1.f});
    spriteTrack.sort_keyframes();

    CameraTrack& cameraTrack = group.add_camera_track("outpost_camera");
    CameraKeyframe start{};
    start.time_ms = 0;
    start.position = {0.f, 0.f, 8.f};
    start.field_of_view = 55.f;
    CameraKeyframe mid{};
    mid.time_ms = 15'000;
    mid.position = {0.f, 30.f, 12.f};
    mid.field_of_view = 70.f;
    CameraKeyframe end{};
    end.time_ms = 30'000;
    end.position = {0.f, 60.f, 15.f};
    end.field_of_view = 85.f;
    cameraTrack.add_keyframe(start);
    cameraTrack.add_keyframe(mid);
    cameraTrack.add_keyframe(end);
    cameraTrack.sort_keyframes();

    ActorTrack& actorTrack = group.add_actor_track("outpost_player");
    actorTrack.set_actor_id("agent_3d");
    ActorEvent mount{};
    mount.time_ms = 2'000;
    mount.kind = ActorEventKind::Mount;
    mount.actor_id = "agent_3d";
    mount.mount_point = "cockpit";
    actorTrack.add_actor_event(mount);
    actorTrack.sort_actor_events();

    MotionTrack& motionTrack = group.add_motion_track("outpost_intro");
    motionTrack.set_path_id("outpost_intro");
    MotionWaypoint wp0{};
    wp0.time_ms = 0;
    wp0.position = {0.f, 0.f, 0.f};
    MotionWaypoint wp1{};
    wp1.time_ms = 15'000;
    wp1.position = {10.f, 0.f, 5.f};
    MotionWaypoint wp2{};
    wp2.time_ms = 30'000;
    wp2.position = {20.f, 5.f, 10.f};
    motionTrack.path().add_waypoint(wp0);
    motionTrack.path().add_waypoint(wp1);
    motionTrack.path().add_waypoint(wp2);
    motionTrack.path().sort_waypoints();

    return timeline;
}

bool load_outpost_intro_30s_from_asset(Timeline& outTimeline, std::string* errorOut) {
    static const char* kAssetText =
        "# Outpost intro 30s sequence\n"
        "duration_ms=30000\n"
        "sprite hud_sprite 0,-20,0,1 15000,0,10,1 30000,40,20,1\n"
        "camera 0,0,0,8,55 15000,0,30,12,70 30000,0,60,15,85\n"
        "motion outpost_intro 0,0,0,0 15000,10,0,5 30000,20,5,10\n"
        "actor agent_3d mount 2000 cockpit 15\n"
        "actor agent_3d unmount 28000\n";

    return load_timeline_from_asset(kAssetText, outTimeline, errorOut);
}

} // namespace fuse::cinematics
