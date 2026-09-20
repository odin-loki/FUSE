#pragma once

// Ore: Engine/source/Verve/VActor/VActor.h (mount/unmount event signal without ShapeBase)

#include <fuse/cinematics/actor_track.hpp>
#include <fuse/cinematics/mount_orientation.hpp>
#include <fuse/cinematics/timeline.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::cinematics {

struct ShapeBaseMountOffset {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float yaw_deg = 0.f;
    float pitch_deg = 0.f;
    float roll_deg = 0.f;
    MountQuaternion orientation{};
};

struct ShapeBaseBoneAttach {
    std::string bone_name;
    float offset_x = 0.f;
    float offset_y = 0.f;
    float offset_z = 0.f;
    float yaw_deg = 0.f;
    float pitch_deg = 0.f;
    float roll_deg = 0.f;
};

/// Headless VActor mount bridge — maps actor_id strings to SceneObject3D instances.
class VActorBridge {
public:
    void bind(const std::string& actor_id, fuse::SceneObject3D* object);

    void apply_mount(const std::string& actor_id, const std::string& mount_point);
    void apply_unmount(const std::string& actor_id);

    /// Apply ShapeBase mount offset to bound scene object (VActor ore without Torque).
    void apply_shapebase_attach(const std::string& actor_id,
                              const std::string& mount_point,
                              float mount_yaw_deg = 0.f);
    /// Apply chained ShapeBase mount points (vehicle_seat → turret ore).
    void apply_shapebase_mount_chain(const std::string& actor_id,
                                     const std::vector<std::string>& mount_chain,
                                     float mount_yaw_deg = 0.f);
    /// Apply ShapeBase bone attach offset (VActor bone slot ore without DTS skeleton).
    void apply_shapebase_bone_attach(const std::string& actor_id, const std::string& bone_name);
    void sync_bone_attach_from_timeline(const Timeline& timeline);
    void sync_bound_objects();
    void sync_motion_from_timeline(const Timeline& timeline);

    const std::string& mount_point_for(const std::string& actor_id) const;
    ShapeBaseMountOffset mount_offset_for(const std::string& mount_point) const;
    ShapeBaseBoneAttach bone_attach_for(const std::string& bone_name) const;
    u32 mountCount() const { return m_mountCount; }
    u32 unmountCount() const { return m_unmountCount; }
    u32 shapebaseAttachCount() const { return m_shapebaseAttachCount; }
    u32 mountChainDepth() const { return m_mountChainDepth; }
    u32 shapebaseBoneAttachCount() const { return m_shapebaseBoneAttachCount; }
    u32 boneMotionSyncCount() const { return m_boneMotionSyncCount; }
    u32 mountRotationSyncCount() const { return m_mountRotationSyncCount; }
    const std::string& bone_name_for(const std::string& actor_id) const;
    u32 runtimeAttachCount() const { return m_runtimeAttachCount; }
    bool is_runtime_attached(const std::string& actor_id) const;
    u32 syncCount() const { return m_syncCount; }
    u32 motionSyncCount() const { return m_motionSyncCount; }

private:
    struct BoundActorState {
        fuse::SceneObject3D* object = nullptr;
        float baseX = 0.f;
        float baseY = 0.f;
        float baseZ = 0.f;
        ShapeBaseMountOffset offset{};
        std::string boneName;
        bool mounted = false;
        bool runtimeAttached = false;
        float motionX = 0.f;
        float motionY = 0.f;
        float motionZ = 0.f;
    };

    std::unordered_map<std::string, BoundActorState> m_actors;
    std::unordered_map<std::string, std::string> m_mountPoints;
    u32 m_mountCount = 0;
    u32 m_unmountCount = 0;
    u32 m_shapebaseAttachCount = 0;
    u32 m_mountChainDepth = 0;
    u32 m_shapebaseBoneAttachCount = 0;
    u32 m_boneMotionSyncCount = 0;
    u32 m_mountRotationSyncCount = 0;
    u32 m_runtimeAttachCount = 0;
    u32 m_syncCount = 0;
    u32 m_motionSyncCount = 0;
};

/// Drain actor mount/unmount cues crossed since `since_ms` up to the playhead time.
void drain_actor_cues(const Timeline& timeline, VActorBridge& bridge, TimelineMs since_ms = 0);

/// 30s Outpost intro sequence stub (Verve VController ore without Torque content pack).
Timeline make_outpost_intro_30s_stub();

/// Load 30s Outpost intro from embedded Samples asset text.
bool load_outpost_intro_30s_from_asset(Timeline& outTimeline, std::string* errorOut = nullptr);

} // namespace fuse::cinematics
