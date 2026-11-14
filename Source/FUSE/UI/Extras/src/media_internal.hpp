// SystemVideo decoder backends (internal). One Decoder reads one stream of a source: video frames as RGBA8 or audio
// as interleaved float. Backends: GStreamer (media_gst.cpp, Linux / BSD), Media Foundation (media_mf.cpp, Windows),
// AVFoundation (media_avf.mm, Apple, UNVERIFIED). All load the system framework at runtime.
#pragma once
#include "fuse/ui/extras/media.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fuse::ui::media::detail {

enum class StreamKind { Video, Audio };
struct StreamInfo {
    u32 width = 0, height = 0;
    f32 fps = 0.f;
    u32 frames = 0; ///< video frame count (from the duration, or counted)
    double duration = 0.0;
    u32 rate = 0, channels = 0; ///< audio
    bool atEnd = false;         ///< the decoder was left at the end (frames were counted); the next read must seek
};
class Decoder {
public:
    virtual ~Decoder() = default;
    virtual StreamInfo info() const = 0;
    /// Video: the next readFrame returns frame `frame` (or the first frame after it).
    virtual bool seekFrame(u32 frame) = 0;
    /// Video: decodes the next frame into `rgba` (width * height, R in the low byte, opaque) and reports its index.
    virtual bool readFrame(u32* rgba, u32& index) = 0;
    /// Audio: reposition to `seconds`.
    virtual bool seekSeconds(double seconds) = 0;
    /// Audio: appends the next decoded block (interleaved float); 0 at the end of the stream or on error.
    virtual std::size_t readAudio(std::vector<f32>& out) = 0;
};

bool backendAvailable();
std::string backendName();
std::unique_ptr<Decoder> openFile(const std::string& path, StreamKind kind, std::string* error);
/// GStreamer only: a source description followed by the conversion and the sink.
std::unique_ptr<Decoder> openPipeline(const std::string& description, StreamKind kind, std::string* error);

} // namespace fuse::ui::media::detail
