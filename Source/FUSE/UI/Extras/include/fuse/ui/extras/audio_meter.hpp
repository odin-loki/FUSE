// Audio level meter widget (class "audiolevelmeter"): per-channel peak / RMS bars in dBFS with ballistics (instant
// attack, timed release), a peak-hold tick and a latched clip lamp. Self-contained: it is fed interleaved float
// samples by the application (an audio callback's block through a queue, a file decoder) and needs no audio device.
// The bar colours are theme tokens (--meter-ok / --meter-warn / --meter-danger, with the classic green / yellow / red
// as fallbacks). The system video / audio output half of the branch's media module (media.hpp) is still parked.
#pragma once

#include "fuse/ui/ui.hpp"

#include <span>
#include <string_view>

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
