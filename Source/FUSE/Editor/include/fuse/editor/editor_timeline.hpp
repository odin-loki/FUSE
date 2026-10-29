#pragma once

#include <fuse/cinematics/property_track.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/editor/undo_stack.hpp>

#include <array>
#include <string>
#include <vector>

namespace fuse::editor {

/// Animatable Transform channels ("position.x" … "scale.z"; rotation as XYZ Euler degrees).
inline constexpr std::array<const char*, 9> kTimelineChannels = {
    "position.x", "position.y", "position.z", "rotation.x", "rotation.y",
    "rotation.z", "scale.x",    "scale.y",    "scale.z"};

/// Editor keyframe timeline (GUI-5): per-entity Transform channel lanes built on
/// `cinematics::PropertyTrack` (target_object_id = entity index, property_path = channel).
/// Qt-free; the Qt Timeline dock draws and edits it, and edits go through `TimelineEditCommand`.
class EditorTimeline {
public:
    using Tracks = std::vector<cinematics::PropertyTrack>;

    void clear();
    [[nodiscard]] const Tracks& tracks() const { return m_tracks; }
    [[nodiscard]] Tracks& tracks() { return m_tracks; }
    [[nodiscard]] cinematics::TimelineMs durationMs() const { return m_durationMs; }
    void setDurationMs(cinematics::TimelineMs ms) { m_durationMs = ms > 0 ? ms : 1; }
    [[nodiscard]] cinematics::TimelineMs playheadMs() const { return m_playheadMs; }
    void setPlayheadMs(cinematics::TimelineMs ms);
    [[nodiscard]] u32 revision() const { return m_revision; }
    void touch() { ++m_revision; }

    /// Track index for (entity, channel) or -1; `ensureTrack` creates it.
    [[nodiscard]] int findTrack(u32 entityIndex, const std::string& channel) const;
    int ensureTrack(u32 entityIndex, const std::string& channel);
    [[nodiscard]] static u32 entityOf(const cinematics::PropertyTrack& track);
    [[nodiscard]] u32 keyCount() const;

    /// Key every channel of `entity`'s Transform at `timeMs` (replaces keys at the same time).
    bool keyTransform(const ecs::Registry& registry, ecs::EntityID entity, cinematics::TimelineMs timeMs);
    /// Move key `key` of track `track` to `timeMs` (re-sorts; returns the key's new index or -1).
    int moveKey(int track, int key, cinematics::TimelineMs timeMs);
    bool deleteKey(int track, int key);
    /// Write sampled channel values into the registry at `timeMs`. Returns entities touched.
    u32 apply(ecs::Registry& registry, cinematics::TimelineMs timeMs) const;

    static f32 channelValue(const ecs::Transform& t, const std::string& channel);
    static void setChannels(ecs::Transform& t, const std::array<f32, 9>& values, const std::array<bool, 9>& set);
    static std::array<f32, 3> eulerDeg(const ecs::quat& q);
    static ecs::quat fromEulerDeg(f32 x, f32 y, f32 z);

    /// `<scene>.timeline` sidecar ("FUSEEDTL 1"). Missing file → load returns false, timeline cleared.
    bool save(const std::string& path, std::string* error = nullptr) const;
    bool load(const std::string& path, std::string* error = nullptr);
    static std::string sidecarPath(const std::string& scenePath) { return scenePath + ".timeline"; }

private:
    Tracks m_tracks;
    cinematics::TimelineMs m_durationMs = 5000;
    cinematics::TimelineMs m_playheadMs = 0;
    u32 m_revision = 0;
};

/// Undoable timeline edit: snapshots of the track list before / after (keys are small).
class TimelineEditCommand final : public UndoCommand {
public:
    TimelineEditCommand(EditorTimeline& timeline, EditorTimeline::Tracks before, EditorTimeline::Tracks after,
                        std::string description, bool mergeable = false)
        : m_timeline(timeline), m_before(std::move(before)), m_after(std::move(after)),
          m_description(std::move(description)), m_mergeable(mergeable) {}
    void execute() override { set_(m_after); }
    void undo() override { set_(m_before); }
    std::string description() const override { return m_description; }
    bool merge(const UndoCommand& other) override;

private:
    void set_(const EditorTimeline::Tracks& tracks) {
        m_timeline.tracks() = tracks;
        m_timeline.touch();
    }
    EditorTimeline& m_timeline;
    EditorTimeline::Tracks m_before;
    EditorTimeline::Tracks m_after;
    std::string m_description;
    bool m_mergeable;
};

} // namespace fuse::editor
