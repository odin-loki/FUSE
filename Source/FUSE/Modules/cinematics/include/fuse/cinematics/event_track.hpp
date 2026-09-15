#pragma once

// Ore: Engine/source/Verve/Extension/Script/VScriptEventTrack.h
//      third_party/addons/Verve/Engine/source/Verve/Extension/Script/VScriptEventTrack.h

#include <fuse/cinematics/track.hpp>

#include <string>

namespace fuse::cinematics {

/// Script / director cue lane (Verve VScriptEventTrack without Torque Con:: bridge).
class EventTrack : public Track {
public:
    explicit EventTrack(const std::string& label = "EventTrack");

    TrackKind kind() const override { return TrackKind::Event; }

    const std::string& script_hook_id() const { return script_hook_id_; }
    void set_script_hook_id(const std::string& id) { script_hook_id_ = id; }

private:
    std::string script_hook_id_;
};

} // namespace fuse::cinematics
