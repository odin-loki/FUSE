// FUSE Relight RL-1.1: tap / device-import selection through the RL-0.6 options.
#include <fuse/relight/tap/tap_config.hpp>

#include <fuse/relight/tap/null_tap.hpp>
#include <fuse/relight/tap/recording_tap.hpp>

#include <fuse/relight/options/options.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace fuse::relight::tap {

namespace {

struct TapOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.tap", std::string, mode, "off", "FUSE_RELIGHT_TAP_MODE",
                            "Relight tap on the D3D9 front end: off (no tap; every hook is a null check), "
                            "null (events dispatched and ignored) or record (events written as JSON Lines).");
    FUSE_RELIGHT_OPTION_ENV("relight.tap", std::string, recordPath, "relight_tap.jsonl",
                            "FUSE_RELIGHT_TAP_RECORD_PATH",
                            "Output file of the recording tap (relight.tap.mode = record).");
};

struct DeviceOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.device", bool, import, true, "FUSE_RELIGHT_DEVICE_IMPORT",
                            "FUSE creates the Vulkan instance and device and DXVK imports them (plan AD-2). "
                            "Off: DXVK creates its own.");
};

struct VkOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.vk", bool, validation, false, "FUSE_RELIGHT_VK_VALIDATION",
                            "Enable VK_LAYER_KHRONOS_validation on the FUSE-created Vulkan instance when the "
                            "loader offers it, and count validation messages through VK_EXT_debug_utils.");
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool relightEnvEnabled() {
    const char* v = std::getenv("FUSE_RELIGHT");
    return !(v && v[0] == '0' && v[1] == '\0');
}

} // namespace

bool parseTapMode(std::string_view text, TapMode& out) {
    const std::string s = lower(text);
    if (s == "off" || s == "0" || s == "none" || s.empty()) {
        out = TapMode::Off;
    } else if (s == "null") {
        out = TapMode::Null;
    } else if (s == "record" || s == "recording") {
        out = TapMode::Record;
    } else {
        return false;
    }
    return true;
}

const char* tapModeName(TapMode mode) {
    switch (mode) {
    case TapMode::Off:
        return "off";
    case TapMode::Null:
        return "null";
    case TapMode::Record:
        return "record";
    }
    return "off";
}

RuntimeConfig resolveRuntimeConfig() {
    RuntimeConfig config;
    config.relightEnabled = relightEnvEnabled();
    if (!config.relightEnabled) {
        config.tapMode = TapMode::Off;
        config.importDevice = false;
        return config;
    }
    if (!options::OptionSystem::isInitialized()) {
        options::OptionSystem::initialize();
    }
    TapMode mode = TapMode::Off;
    if (!parseTapMode(TapOptions::mode(), mode)) {
        std::fprintf(stderr, "fuse-relight: unknown relight.tap.mode '%s'; tap off\n", TapOptions::mode().c_str());
    }
    config.tapMode = mode;
    if (!TapOptions::recordPath().empty()) {
        config.recordPath = TapOptions::recordPath();
    }
    config.importDevice = DeviceOptions::import();
    config.vkValidation = VkOptions::validation();
    return config;
}

const RuntimeConfig& runtimeConfig() {
    static std::once_flag once;
    static RuntimeConfig config;
    std::call_once(once, [] { config = resolveRuntimeConfig(); });
    return config;
}

std::unique_ptr<IRelightTap> createTap(const RuntimeConfig& config, unsigned deviceOrdinal) {
    if (!config.relightEnabled) {
        return nullptr;
    }
    switch (config.tapMode) {
    case TapMode::Off:
        return nullptr;
    case TapMode::Null:
        return std::make_unique<NullTap>();
    case TapMode::Record: {
        std::string path = config.recordPath;
        if (deviceOrdinal > 0) {
            path += "." + std::to_string(deviceOrdinal);
        }
        return std::make_unique<RecordingTap>(path);
    }
    }
    return nullptr;
}

} // namespace fuse::relight::tap
