#pragma once

// Ore: Engine/source/Verve/VActor/VActor.h (mount/unmount event signal without ShapeBase)

#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

#include <string>
#include <unordered_map>

namespace fuse::cinematics {

/// Headless VActor mount bridge — maps actor_id strings to SceneObject3D instances.
class VActorBridge {
public:
    void bind(const std::string& actor_id, fuse::SceneObject3D* object);

    void apply_mount(const std::string& actor_id, const std::string& mount_point);
    void apply_unmount(const std::string& actor_id);

    const std::string& mount_point_for(const std::string& actor_id) const;
    u32 mountCount() const { return m_mountCount; }
    u32 unmountCount() const { return m_unmountCount; }

private:
    std::unordered_map<std::string, fuse::SceneObject3D*> m_objects;
    std::unordered_map<std::string, std::string> m_mountPoints;
    u32 m_mountCount = 0;
    u32 m_unmountCount = 0;
};

/// Drain actor mount/unmount cues crossed since `since_ms` up to the playhead time.
void drain_actor_cues(const Timeline& timeline, VActorBridge& bridge, TimelineMs since_ms = 0);

/// 30s Outpost intro sequence stub (Verve VController ore without Torque content pack).
Timeline make_outpost_intro_30s_stub();

} // namespace fuse::cinematics
