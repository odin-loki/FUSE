#pragma once

// Ore: Engine/source/Verve/VActor/VActor.h (mount/unmount event signal)
//      third_party/addons/Verve/Engine/source/Verve/VActor/VActor.h

#include <fuse/cinematics/track.hpp>

#include <string>

namespace fuse::cinematics {

enum class ActorEventKind {
    Mount,
    Unmount,
};

/// Actor mount/unmount cue (VActor `k_MountEvent` / `k_UnmountEvent` ore).
struct ActorEvent {
    TimelineMs time_ms = 0;
    ActorEventKind kind = ActorEventKind::Mount;
    std::string actor_id;
    std::string mount_point;
    float mount_yaw_deg = 0.f;
};

/// Animated actor lane stub (Verve VActor without ShapeBase bridge).
class ActorTrack : public Track {
public:
    explicit ActorTrack(const std::string& label = "ActorTrack");

    TrackKind kind() const override { return TrackKind::Actor; }

    const std::string& actor_id() const { return actor_id_; }
    void set_actor_id(const std::string& id) { actor_id_ = id; }

    void add_actor_event(const ActorEvent& event);
    void sort_actor_events();

    const std::vector<ActorEvent>& actor_events() const { return actor_events_; }

    /// Active mount point at `time_ms`, or empty when unmounted.
    std::string mount_point_at(TimelineMs time_ms) const;
    /// Active ShapeBase mount yaw (degrees) at `time_ms`.
    float mount_yaw_at(TimelineMs time_ms) const;

private:
    std::string actor_id_;
    std::vector<ActorEvent> actor_events_;
};

} // namespace fuse::cinematics
