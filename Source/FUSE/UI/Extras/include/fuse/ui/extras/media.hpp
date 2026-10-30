#pragma once
// Multimedia extras: a video source backed by the platform's own decoder, loaded at runtime, and an audio level meter.
//
//   SystemVideo  decodes any file the system decoder can play into frames for widgets::videoWidget. Linux / BSD:
//                GStreamer 1.x (libgstreamer-1.0 + libgstapp-1.0) loaded through the os seam, nothing linked at build
//                time; decodebin picks whatever codecs are installed. Other platforms: available() is false for now.
//                Frames are decoded up front (up to maxFrames) so seeking is random access; this suits UI clips, not
//                feature-length video.
//   audioLevelMeter  a per-channel peak / RMS meter in dBFS with ballistics (instant attack, timed release), a
//                peak-hold tick and a latching clip lamp. Feed it interleaved float samples.
#include "fuse/ui/widgets/widgets.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::media {

class SystemVideo {
public:
    SystemVideo();
    ~SystemVideo();
    SystemVideo(const SystemVideo&) = delete;
    SystemVideo& operator=(const SystemVideo&) = delete;

    /// True when the system decoder could be loaded (GStreamer core, app sink and the decodebin element).
    static bool available();
    /// "gstreamer 1.x.y", or empty when unavailable.
    static std::string backendName();

    bool openFile(const std::string& path, u32 maxFrames = 3000, std::string* error = nullptr);
    /// Decode a GStreamer source description (e.g. "videotestsrc num-buffers=10"); it is followed by the RGBA
    /// conversion and the sink. For tests and generated content.
    bool openPipeline(const std::string& sourceDescription, u32 maxFrames = 3000, std::string* error = nullptr);
    bool valid() const;

    u32 width() const;
    u32 height() const;
    u32 frameCount() const;
    f32 fps() const;
    /// Frame `index` as opaque RGBA8 (R in the low byte); nullptr when out of range.
    const u32* frame(u32 index) const;
    /// A VideoSource over the decoded frames (valid while this object lives).
    const widgets::VideoSource& source() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace fuse::ui::media

namespace fuse::ui::widgets {

struct AudioLevels {
    f32 peakDb = -120.f; ///< displayed peak (instant attack, releaseDbPerSec fall)
    f32 rmsDb = -120.f;  ///< displayed RMS (same ballistics)
    f32 holdDb = -120.f; ///< peak-hold tick
    bool clipped = false; ///< latched once a sample reaches 0 dBFS; cleared by a click or audioLevelMeterReset
};
struct AudioMeterOptions {
    f32 floorDb = -60.f;
    f32 releaseDbPerSec = 24.f;
    f32 holdSec = 1.5f;
    f32 warnDb = -12.f;  ///< green below, yellow from here
    f32 dangerDb = -3.f; ///< red from here
    bool vertical = true;
    bool scale = true; ///< draw the dB tick labels
};

/// value() = loudest displayed peak in dBFS.
NodeHandle audioLevelMeter(UiContext& ui, NodeHandle parent, u32 channels, const AudioMeterOptions& opt = {}, std::string_view id = {});
/// Feed `frames` interleaved samples of `channels` (extra meter channels stay silent); time advances frames / sampleRate.
void audioLevelMeterPush(UiContext& ui, NodeHandle meter, std::span<const f32> interleaved, u32 channels, f32 sampleRate);
/// Advance the ballistics with silence (e.g. when the stream pauses).
void audioLevelMeterAdvance(UiContext& ui, NodeHandle meter, f32 seconds);
void audioLevelMeterReset(UiContext& ui, NodeHandle meter);
AudioLevels audioLevelMeterLevels(const UiContext& ui, NodeHandle meter, u32 channel);
u32 audioLevelMeterChannels(const UiContext& ui, NodeHandle meter);

} // namespace fuse::ui::widgets
