#pragma once

// Ore: Verve VTrack event firing + VScriptEventTrack cue dispatch (stub queue for game-thread drain)
//      Engine/source/Verve/Extension/Script/VScriptEventTrack.h

#include <fuse/cinematics/track.hpp>
#include <fuse/cinematics/types.hpp>

#include <functional>
#include <string>
#include <vector>

namespace fuse::cinematics {

struct CueEntry {
    std::string label;
    std::string track_label;
    std::string group_label;
    TrackKind track_kind = TrackKind::Generic;
    TimelineMs trigger_ms = 0;
};

using CueDispatchHook = std::function<void(const CueEntry&)>;

/// Pending timeline cues — analogue to editor scrub / advance event dispatch without Torque bridge.
class CueQueue {
public:
    void set_dispatch_hook(CueDispatchHook hook) { dispatch_hook_ = std::move(hook); }

    void enqueue(const CueEntry& entry);
    void clear();

    bool empty() const { return pending_.empty(); }
    u32 pending_count() const { return static_cast<u32>(pending_.size()); }
    u32 total_enqueued() const { return total_enqueued_; }

    const std::vector<CueEntry>& pending() const { return pending_; }

    /// Move all pending cues out and clear the queue (game-thread commit pattern).
    std::vector<CueEntry> drain();

private:
    std::vector<CueEntry> pending_;
    CueDispatchHook dispatch_hook_;
    u32 total_enqueued_ = 0;
};

} // namespace fuse::cinematics
