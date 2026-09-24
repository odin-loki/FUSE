// FUSE Relight RL-3.5: Logic graph options (RL-0.6 registry; each also answers to its relight.* / rtx.* twin).
//
//   rtx.graph.enable             upstream GraphManager::enable (default true): graphs load; off unloads them all.
//   rtx.graph.pauseGraphUpdates  upstream GraphManager::pauseGraphUpdates (default false): state kept, no updates.
//   relight.logic.fixedDeltaTime seconds per frame the live runtime feeds the graphs (0: the measured frame time,
//                                as upstream's GlobalTime; set it for deterministic captures).
//   relight.logic.keyboard       the live runtime samples the keyboard for KeyboardInput (Windows builds).
//   relight.logic.recordValues   the capture record lists every graph instance's outputs per frame.
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_types.hpp>

namespace fuse::relight::logic {

struct LogicOptions {
    FUSE_RELIGHT_OPTION("rtx.graph", bool, enable, true,
                        "Enable graph loading.  If disabled, all graphs will be unloaded, losing any state.");
    FUSE_RELIGHT_OPTION("rtx.graph", bool, pauseGraphUpdates, false,
                        "Pause graph updating.  If enabled, graphs logic will not be updated, but graph state will be retained.");
    FUSE_RELIGHT_OPTION_ENV("relight.logic", float, fixedDeltaTime, 0.0f, "FUSE_RELIGHT_LOGIC_FIXED_DELTA_TIME",
                            "Seconds per frame fed to Logic graphs (Time, Smooth, Velocity). 0: the measured time between "
                            "presented frames. Set it for deterministic captures.");
    FUSE_RELIGHT_OPTION_ENV("relight.logic", bool, keyboard, true, "FUSE_RELIGHT_LOGIC_KEYBOARD",
                            "Sample the keyboard each frame for the KeyboardInput component (Windows builds).");
    FUSE_RELIGHT_OPTION_ENV("relight.logic", bool, recordValues, true, "FUSE_RELIGHT_LOGIC_RECORD_VALUES",
                            "Write every graph instance's outputs into the capture record's replace_frame line.");
};

/// References every option above (static libraries: keeps the registrations linked).
void registerLogicOptions();

} // namespace fuse::relight::logic
