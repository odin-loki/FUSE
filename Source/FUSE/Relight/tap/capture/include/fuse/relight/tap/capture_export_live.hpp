// FUSE Relight RL-1.8 live: the capture (USDA + POCO store + DDS) written from inside d3d9.dll.
//
// CaptureTap (capture_tap.hpp) runs the Wave R1 capture packages on the dispatcher's events; with
// relight.tap.captureExport on it also owns a LiveCaptureExport, which does what the RL-1.8 replay tool
// (tests/capture_export/rl_capture_export_replay.cpp) does on a recorded stream: at every presented frame the
// committed draws go to SceneModel (RL-1.7 instances, every frame, so instance ids are those of the whole run)
// and, inside the capture window, to GameCapturer (capture/export) with the frame's lights and main camera;
// the capture is written (capture_writer.hpp) when the window closes or at device destruction, whichever
// comes first. The DDS files come from TextureTracker's canonical mip 0 (the tap keeps it after hashing
// while the export is on).
//
// Options (RL-0.6; each also answers to its rtx.* twin; CaptureTapConfig::fromOptions, so capture mode only):
//   relight.tap.captureExport            on / off (env FUSE_RELIGHT_TAP_CAPTURE_EXPORT, default off)
//   relight.tap.captureExportDir         output directory (env FUSE_RELIGHT_TAP_CAPTURE_EXPORT_DIR, default
//                                        relight_capture); device n > 0 writes <dir>.<n>; a directory that
//                                        exists and is not empty is never overwritten: <dir>_1, _2, ... instead
//   relight.tap.captureExportFirstFrame  first captured frame (tap frame index: presents before it)
//                                        (env FUSE_RELIGHT_TAP_CAPTURE_EXPORT_FIRST_FRAME, default 0)
//   relight.tap.captureExportFrames      number of captured frames, 0: until the device is destroyed
//                                        (env FUSE_RELIGHT_TAP_CAPTURE_EXPORT_FRAMES, default 0)
//   relight.tap.captureExportGameId      Remaster game id (store namespace); empty: the executable's name
//                                        without extension (env FUSE_RELIGHT_TAP_CAPTURE_EXPORT_GAME_ID)
// Capture metadata: window title and stage "capture_<exe stem>", exe name = the executable's file name, as
// rl_capture_export_replay names them from the app (its --game defaults to the app name too), so a live
// capture of an RL-0.4 app and the replay of the same run are equal under Tools/FUSE/Relight/capture_diff.py.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fuse::relight::tap {

class CaptureTap;
struct CaptureDrawRecord;

struct CaptureExportConfig {
    std::string dir;              ///< output directory; empty: no capture export
    std::uint64_t firstFrame = 0; ///< first captured frame
    std::uint64_t frames = 0;     ///< captured frames; 0: every frame until device destruction
    std::string gameId;           ///< empty: executable stem
    std::string windowTitle;      ///< empty: executable stem
    std::string exeName;          ///< empty: executable file name
    std::string stageName;        ///< empty: "capture_" + executable stem

    bool enabled() const { return !dir.empty(); }
    /// The relight.tap.captureExport* options, resolved now (dir empty when captureExport is off).
    static CaptureExportConfig fromOptions();
};

/// What the export wrote (valid once written()).
struct CaptureExportResult {
    std::string dir; ///< the directory actually written
    bool ok = false; ///< written without I/O error or writer problem
    std::vector<std::string> errors;
    std::size_t framesCaptured = 0, meshes = 0, materials = 0, textures = 0, instances = 0, lights = 0, keys = 0;
};

/// The executable's file name ("app.exe") and stem ("app"); "game" when it cannot be determined.
std::string processExeName();
std::string processExeStem();

class LiveCaptureExport {
public:
    /// `tap` owns this object and calls it under its lock.
    LiveCaptureExport(CaptureTap& tap, CaptureExportConfig config);
    ~LiveCaptureExport();
    LiveCaptureExport(const LiveCaptureExport&) = delete;
    LiveCaptureExport& operator=(const LiveCaptureExport&) = delete;

    /// One flushed frame (CaptureTap::flushFrame, geometry categories applied). `presented` false: the
    /// draws after the last Present at device destruction (not a frame; ignored, as the replay does).
    /// Writes the capture when this was the window's last frame.
    void onFrame(std::uint64_t frame, const std::vector<CaptureDrawRecord>& draws, bool presented);
    /// Writes the capture now if it has not been written (device destruction). Idempotent.
    void finish();

    bool written() const { return m_written; }
    const CaptureExportResult& result() const { return m_result; }
    const CaptureExportConfig& config() const { return m_config; }

private:
    struct State;
    CaptureTap& m_tap;
    CaptureExportConfig m_config;
    std::unique_ptr<State> m_state;
    bool m_written = false;
    CaptureExportResult m_result;
};

} // namespace fuse::relight::tap
