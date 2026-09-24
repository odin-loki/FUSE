// FUSE Relight RL-4.1: frame orchestration options (see frame_options.hpp).
#include <fuse/relight/render/frame/frame_options.hpp>

#include <cctype>
#include <string>

namespace fuse::relight::render::frame {

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &FrameOptions::mode, &FrameOptions::solidColor, &FrameOptions::textureSwap, &FrameOptions::injectAtUi,
    &FrameOptions::statsPath,
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

void registerFrameOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
}

bool parseFrameMode(std::string_view text, FrameMode& out) {
    const std::string s = lower(text);
    if (s == "off" || s == "0" || s == "none" || s.empty()) {
        out = FrameMode::Off;
    } else if (s == "passthrough") {
        out = FrameMode::Passthrough;
    } else if (s == "solid") {
        out = FrameMode::Solid;
    } else {
        return false;
    }
    return true;
}

const char* frameModeName(FrameMode mode) {
    switch (mode) {
    case FrameMode::Passthrough:
        return "passthrough";
    case FrameMode::Solid:
        return "solid";
    case FrameMode::Off:
        break;
    }
    return "off";
}

bool parseRgbHex(std::string_view text, std::uint32_t& out) {
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1);
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.size() != 6) {
        return false;
    }
    std::uint32_t v = 0;
    for (char c : text) {
        const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                      : (c >= 'a' && c <= 'f')                    ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F')                    ? c - 'A' + 10
                                                                  : -1;
        if (d < 0) {
            return false;
        }
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    out = v;
    return true;
}

FrameConfig FrameConfig::fromOptions() {
    FrameConfig c;
    if (!parseFrameMode(FrameOptions::mode(), c.mode)) {
        c.mode = FrameMode::Off;
    }
    std::uint32_t rgb = 0;
    if (parseRgbHex(FrameOptions::solidColor(), rgb)) {
        c.solidColor = rgb;
    }
    c.textureSwap = FrameOptions::textureSwap();
    c.injectAtUi = FrameOptions::injectAtUi();
    c.statsPath = FrameOptions::statsPath();
    return c;
}

} // namespace fuse::relight::render::frame
