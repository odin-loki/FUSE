#pragma once
// Multimedia extras: a video source backed by the platform's own decoder, loaded at runtime, and an audio level meter.
//
//   SystemVideo  plays any file the system decoder handles through widgets::videoWidget, with its audio track
//                through os::AudioOutput. Linux / BSD: GStreamer 1.x (decodebin picks the installed codecs) and
//                Windows: Media Foundation (IMFSourceReader; tested under Wine), both loaded at runtime. Apple:
//                AVFoundation (AVAssetReader, linked; UNVERIFIED, written without a Mac; os::AudioOutput has no
//                CoreAudio backend yet, so audio is decoded and tapped but not heard there).
//                Decoding streams: a small frame cache, sequential reads during playback, a seek for jumps, so memory
//                does not grow with the clip length. Paths are passed to the decoder as-is (no quoting rules).
//   audioLevelMeter  a per-channel peak / RMS meter in dBFS with ballistics (instant attack, timed release), a
//                peak-hold tick and a latching clip lamp. Feed it interleaved float samples.
#include "fuse/ui/widgets/widgets.hpp"

#include <functional>
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

    /// `maxFrames` caps frameCount(). Opens the audio track too when the file has one.
    bool openFile(const std::string& path, u32 maxFrames = 1u << 30, std::string* error = nullptr);
    /// GStreamer source descriptions (e.g. "videotestsrc num-buffers=10"), each followed by the conversion and the
    /// sink; an empty `audioDescription` means no audio. For tests and generated content.
    bool openPipeline(const std::string& sourceDescription, u32 maxFrames = 1u << 30, std::string* error = nullptr,
                      const std::string& audioDescription = {});
    bool valid() const;

    u32 width() const;
    u32 height() const;
    u32 frameCount() const;
    f32 fps() const;
    double durationSeconds() const;
    /// Frame `index` as opaque RGBA8 (R in the low byte), decoded on demand; nullptr when out of range or the
    /// decoder failed. The pointer stays valid until `cacheSlots()` further distinct frames have been requested.
    const u32* frame(u32 index) const;
    static constexpr u32 cacheSlots() { return 8; }
    /// Decode statistics: frames decoded, seeks issued, frames currently cached.
    u32 decodedFrames() const;
    u32 seekCount() const;
    u32 cachedFrames() const;

    // ---- audio ----
    bool hasAudio() const;
    u32 audioRate() const;
    u32 audioChannels() const;
    /// Start the audio track at `fromSeconds` on a worker thread (stops a running one first). Without an output
    /// device (none available, or setAudioOutputEnabled(false)) it is paced by the wall clock instead, so the tap
    /// and the clock still work.
    bool playAudio(double fromSeconds = 0.0, std::string* error = nullptr);
    void stopAudio();
    bool audioPlaying() const;
    /// Seconds of the track heard so far (output position minus device latency); < 0 when audio is not playing.
    double audioClock() const;
    u64 audioFramesPlayed() const;
    /// Receives each decoded block (interleaved float) on the audio thread, e.g. for audioLevelMeterPush via a queue.
    void setAudioTap(std::function<void(std::span<const f32> interleaved, u32 channels, u32 rate)> tap);
    void setAudioOutputEnabled(bool enabled);
    /// The os::AudioOutput backend in use ("pulse", "alsa", "winmm", "none").
    std::string audioBackend() const;

    /// A VideoSource for videoWidget (valid while this object lives): frames on demand, and playback / seek drive
    /// the audio track, whose clock paces the video.
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
