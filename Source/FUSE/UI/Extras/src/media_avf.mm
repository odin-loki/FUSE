// SystemVideo backend: AVFoundation (AVAssetReader), Apple platforms. UNVERIFIED: written without a Mac to build or
// run it on; the logic mirrors media_mf.cpp. Video is read as 32BGRA and turned into RGBA, audio as interleaved
// 32-bit float PCM. Seeking rebuilds the reader with a time range starting at the target (AVAssetReader cannot seek).
#if defined(__APPLE__)
#include "media_internal.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::ui::media::detail {
namespace {

class AvfDecoder final : public Decoder {
public:
    StreamKind kind;
    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    StreamInfo si;
    explicit AvfDecoder(StreamKind k) : kind(k) {}
    ~AvfDecoder() override { [reader cancelReading]; }

    NSDictionary* settings() const {
        if (kind == StreamKind::Video) return @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
        return @{AVFormatIDKey : @(kAudioFormatLinearPCM), AVLinearPCMIsFloatKey : @YES, AVLinearPCMBitDepthKey : @32,
                 AVLinearPCMIsNonInterleaved : @NO, AVLinearPCMIsBigEndianKey : @NO};
    }
    bool startAt(double t) {
        [reader cancelReading];
        NSError* e = nil;
        reader = [[AVAssetReader alloc] initWithAsset:asset error:&e];
        if (!reader) return false;
        output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings()];
        output.alwaysCopiesSampleData = NO;
        if (![reader canAddOutput:output]) return false;
        [reader addOutput:output];
        if (t > 0.0) reader.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(t, 600000), kCMTimePositiveInfinity);
        return [reader startReading];
    }
    bool open(const std::string& path, std::string* err) {
        auto fail = [&](const char* m) { if (err) *err = m; return false; };
        @autoreleasepool {
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
            asset = [AVURLAsset URLAssetWithURL:url options:nil];
            if (!asset) return fail("AVFoundation cannot open the file");
            NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:kind == StreamKind::Video ? AVMediaTypeVideo : AVMediaTypeAudio];
            if (tracks.count == 0) return fail(kind == StreamKind::Video ? "no video stream" : "no audio stream");
            track = tracks[0];
            si.duration = CMTimeGetSeconds(asset.duration);
            if (!std::isfinite(si.duration)) si.duration = 0.0;
            if (kind == StreamKind::Video) {
                const CGSize sz = CGSizeApplyAffineTransform(track.naturalSize, CGAffineTransformIdentity);
                si.width = u32(std::lround(sz.width)), si.height = u32(std::lround(sz.height));
                si.fps = track.nominalFrameRate > 0.f ? track.nominalFrameRate : 30.f;
                si.frames = u32(std::llround(si.duration * double(si.fps)));
                if (!si.width || !si.height || !si.frames) return fail("no video frames");
            } else {
                CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
                const AudioStreamBasicDescription* asbd = fd ? CMAudioFormatDescriptionGetStreamBasicDescription(fd) : nullptr;
                if (!asbd) return fail("bad audio format");
                si.rate = u32(asbd->mSampleRate), si.channels = asbd->mChannelsPerFrame;
                if (!si.rate || !si.channels) return fail("bad audio format");
            }
            if (!startAt(0.0)) return fail("AVAssetReader could not start");
        }
        return true;
    }
    StreamInfo info() const override { return si; }
    bool seekFrame(u32 f) override { return startAt(double(f) / double(si.fps)); }
    bool seekSeconds(double s) override { return startAt(s); }
    bool readFrame(u32* rgba, u32& index) override {
        @autoreleasepool {
            CMSampleBufferRef sb = [output copyNextSampleBuffer];
            if (!sb) return false;
            bool ok = false;
            if (CVImageBufferRef ib = CMSampleBufferGetImageBuffer(sb)) {
                CVPixelBufferLockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
                const u8* base = static_cast<const u8*>(CVPixelBufferGetBaseAddress(ib));
                const std::size_t stride = CVPixelBufferGetBytesPerRow(ib);
                const u32 w = std::min<u32>(si.width, u32(CVPixelBufferGetWidth(ib))), h = std::min<u32>(si.height, u32(CVPixelBufferGetHeight(ib)));
                if (base) {
                    for (u32 y = 0; y < h; ++y) {
                        const u8* row = base + y * stride;
                        u32* o = rgba + std::size_t(y) * si.width;
                        for (u32 x = 0; x < w; ++x) o[x] = u32(row[x * 4 + 2]) | u32(row[x * 4 + 1]) << 8 | u32(row[x * 4]) << 16 | 0xFF000000u;
                    }
                    const double t = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sb));
                    index = std::isfinite(t) ? u32(std::max(0.0, std::round(t * double(si.fps)))) : 0u;
                    ok = true;
                }
                CVPixelBufferUnlockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
            }
            CFRelease(sb);
            return ok;
        }
    }
    std::size_t readAudio(std::vector<f32>& out) override {
        @autoreleasepool {
            CMSampleBufferRef sb = [output copyNextSampleBuffer];
            if (!sb) return 0;
            std::size_t n = 0;
            if (CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb)) {
                const std::size_t len = CMBlockBufferGetDataLength(bb);
                n = len / sizeof(f32);
                const std::size_t at = out.size();
                out.resize(at + n);
                CMBlockBufferCopyDataBytes(bb, 0, n * sizeof(f32), out.data() + at);
            }
            CFRelease(sb);
            return n ? n : 1;
        }
    }
};
} // namespace

bool backendAvailable() { return true; }
std::string backendName() { return "avfoundation"; }
std::unique_ptr<Decoder> openFile(const std::string& path, StreamKind kind, std::string* error) {
    auto d = std::make_unique<AvfDecoder>(kind);
    if (!d->open(path, error)) return nullptr;
    return d;
}
std::unique_ptr<Decoder> openPipeline(const std::string&, StreamKind, std::string* error) {
    if (error) *error = "pipeline descriptions need GStreamer";
    return nullptr;
}

} // namespace fuse::ui::media::detail
#endif
