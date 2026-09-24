// FUSE Relight RL-1.1: runtime selection of the tap and of device import, through the RL-0.6
// options (rtx.conf / user.conf / dxvk.conf and per-option environment variables).
//
//   FUSE_RELIGHT=0              master switch (environment only): Relight stays out entirely -
//                               no options are read, DXVK creates its own Vulkan device and every
//                               tap hook sees a null pointer. This is the RL-0.2 behaviour.
//   relight.tap.mode            off | null | record | capture (env FUSE_RELIGHT_TAP_MODE, default off)
//   relight.tap.recordPath      JSON Lines output of the recording tap
//                               (env FUSE_RELIGHT_TAP_RECORD_PATH, default relight_tap.jsonl)
//   relight.tap.capturePath     per-frame capture record of the capture tap (capture_tap.hpp)
//                               (env FUSE_RELIGHT_TAP_CAPTURE_PATH, default relight_capture.jsonl)
//   relight.tap.captureRecord   capture mode also writes the recording tap's event stream to
//                               recordPath, from the same events (env FUSE_RELIGHT_TAP_CAPTURE_RECORD,
//                               default false)
//   relight.device.import       FUSE creates VkInstance/VkDevice, DXVK imports them (plan AD-2)
//                               (env FUSE_RELIGHT_DEVICE_IMPORT, default true)
//   relight.vk.validation       enable VK_LAYER_KHRONOS_validation on the FUSE instance when the
//                               loader has it, and count debug-utils messages
//                               (env FUSE_RELIGHT_VK_VALIDATION, default false)
//
// Every option also answers to its rtx.* twin (RL-0.6 alias rule), e.g. rtx.tap.mode in rtx.conf.
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace fuse::relight::tap {

/// Capture: the live in-process capture (TextureTracker RL-1.4, GeometryCapture RL-1.3, the RL-1.2
/// classifier) behind CaptureTap (capture_tap.hpp; built by createTapForDevice, device_tap.hpp).
enum class TapMode { Off, Null, Record, Capture };

/// Parses "off" / "null" / "record" / "capture" (case-insensitive; "0"/"none" = off). Returns false
/// and leaves `out` unchanged for anything else.
bool parseTapMode(std::string_view text, TapMode& out);
const char* tapModeName(TapMode mode);

struct RuntimeConfig {
    bool relightEnabled = true; ///< false when FUSE_RELIGHT=0
    TapMode tapMode = TapMode::Off;
    std::string recordPath = "relight_tap.jsonl";
    std::string capturePath = "relight_capture.jsonl";
    bool captureRecord = false;
    bool importDevice = true;
    bool vkValidation = false;
};

/// Reads FUSE_RELIGHT and, unless it is "0", initialises the RL-0.6 option system (if nothing did
/// yet) and resolves the options above. Computed once per process; thread-safe.
const RuntimeConfig& runtimeConfig();

/// Resolves the options now (tests; runtimeConfig() caches its first result).
RuntimeConfig resolveRuntimeConfig();

/// The tap for `config`: nullptr for Off (or Relight disabled), a NullTap, or a RecordingTap
/// writing config.recordPath. `deviceOrdinal` distinguishes devices in one process: device 0
/// writes the path as given, device n > 0 writes <path>.<n> (devicePath()).
/// Capture needs the capture packages, which depend on this library: createTap returns nullptr
/// for it, and createTapForDevice (device_tap.hpp, fuse_relight_tap_capture) builds every mode.
std::unique_ptr<IRelightTap> createTap(const RuntimeConfig& config, unsigned deviceOrdinal);

/// `path` for device `deviceOrdinal`: as given for device 0, <path>.<n> for device n > 0.
std::string devicePath(const std::string& path, unsigned deviceOrdinal);

} // namespace fuse::relight::tap
