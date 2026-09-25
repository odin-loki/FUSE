// FUSE Relight RL-6.1: developer overlay options (see overlay_options.hpp).
#include <fuse/relight/overlay/overlay_options.hpp>

namespace fuse::relight::overlay {

namespace {
const options::OptionBase* const kRegisteredOptions[] = {
    &OverlayOptions::enable,        &OverlayOptions::startVisible, &OverlayOptions::scale,
    &OverlayOptions::debugView,     &OverlayOptions::captureFrames, &OverlayOptions::captureDir,
    &OverlayOptions::deterministic, &OverlayOptions::script,        &OverlayOptions::statsPath,
    &OverlayOptions::dumpPath,
};
} // namespace

void registerOverlayOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
}

OverlayConfig OverlayConfig::fromOptions() {
    registerOverlayOptions();
    OverlayConfig c;
    c.enable = OverlayOptions::enable();
    c.startVisible = OverlayOptions::startVisible();
    c.deterministic = OverlayOptions::deterministic();
    c.scale = OverlayOptions::scale();
    const std::int32_t frames = OverlayOptions::captureFrames();
    c.captureFrames = frames > 0 ? static_cast<std::uint32_t>(frames) : 1u;
    c.captureDir = OverlayOptions::captureDir();
    c.script = OverlayOptions::script();
    c.statsPath = OverlayOptions::statsPath();
    c.dumpPath = OverlayOptions::dumpPath();
    return c;
}

} // namespace fuse::relight::overlay
