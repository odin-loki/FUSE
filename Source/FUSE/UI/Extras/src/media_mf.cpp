// SystemVideo backend: Windows Media Foundation (IMFSourceReader), mfplat / mfreadwrite loaded at runtime through the
// os seam. Video is delivered as RGB32 (the reader's video processor converts) and turned into RGBA; audio as float.
#if defined(_WIN32)
#include "media_internal.hpp"

#include "fuse/ui/os.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::ui::media::detail {
namespace {
// GUIDs defined here (no mfuuid import library needed)
constexpr GUID kMediaTypeVideo = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMediaTypeAudio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kRGB32 = {0x00000016, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kAudioFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kMT_MAJOR_TYPE = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
constexpr GUID kMT_SUBTYPE = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
constexpr GUID kMT_FRAME_SIZE = {0x1652c33d, 0xd6b2, 0x4012, {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
constexpr GUID kMT_FRAME_RATE = {0xc459a2e8, 0x3d2c, 0x4e44, {0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0}};
constexpr GUID kMT_DEFAULT_STRIDE = {0x644b4e48, 0x1e02, 0x4516, {0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6}};
constexpr GUID kMT_AUDIO_SAMPLES_PER_SECOND = {0x5faeeae7, 0x0290, 0x4c31, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
constexpr GUID kMT_AUDIO_NUM_CHANNELS = {0x37e48bf5, 0x645e, 0x4c5b, {0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a}};
constexpr GUID kPD_DURATION = {0x6c990d33, 0xbb8e, 0x477a, {0x85, 0x98, 0x0d, 0x5d, 0x96, 0xfc, 0xd8, 0x8a}};
constexpr GUID kSR_ENABLE_VIDEO_PROCESSING = {0xfb394f3d, 0xccf1, 0x42ee, {0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d}};
constexpr GUID kGuidNull = {};
constexpr DWORD kFirstVideo = 0xFFFFFFFC, kFirstAudio = 0xFFFFFFFD, kAllStreams = 0xFFFFFFFE, kMediaSource = 0xFFFFFFFF;
constexpr DWORD kFlagEnd = 0x2 /*MF_SOURCE_READERF_ENDOFSTREAM*/, kFlagError = 0x1;

struct Mf {
    std::unique_ptr<os::Library> plat, rw, ole;
    HRESULT(WINAPI* MFStartup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* MFCreateMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* MFCreateAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* MFCreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
    HRESULT(WINAPI* CoInitializeEx)(LPVOID, DWORD) = nullptr;
    bool ok = false;
};
Mf& mf() {
    static Mf& m = *new Mf{};
    static const bool once = [] {
        m.plat = os::loadLibrary({"mfplat.dll"});
        m.rw = os::loadLibrary({"mfreadwrite.dll"});
        m.ole = os::loadLibrary({"ole32.dll"});
        if (!m.plat || !m.rw || !m.ole) return false;
        m.MFStartup = m.plat->get<decltype(m.MFStartup)>("MFStartup");
        m.MFCreateMediaType = m.plat->get<decltype(m.MFCreateMediaType)>("MFCreateMediaType");
        m.MFCreateAttributes = m.plat->get<decltype(m.MFCreateAttributes)>("MFCreateAttributes");
        m.MFCreateSourceReaderFromURL = m.rw->get<decltype(m.MFCreateSourceReaderFromURL)>("MFCreateSourceReaderFromURL");
        m.CoInitializeEx = m.ole->get<decltype(m.CoInitializeEx)>("CoInitializeEx");
        if (!m.MFStartup || !m.MFCreateMediaType || !m.MFCreateAttributes || !m.MFCreateSourceReaderFromURL || !m.CoInitializeEx) return false;
        m.CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m.ok = SUCCEEDED(m.MFStartup(0x00020070 /*MF_VERSION*/, 0 /*MFSTARTUP_FULL*/));
        return m.ok;
    }();
    (void)once;
    return m;
}
template <class T> void release(T*& p) {
    if (p) p->Release(), p = nullptr;
}

class MfDecoder final : public Decoder {
public:
    StreamKind kind;
    IMFSourceReader* reader = nullptr;
    StreamInfo si;
    long stride = 0;
    DWORD stream = 0;
    bool ended = false;
    explicit MfDecoder(StreamKind k) : kind(k), stream(k == StreamKind::Video ? kFirstVideo : kFirstAudio) {}
    ~MfDecoder() override { release(reader); }

    bool open(const std::string& path, std::string* err) {
        auto fail = [&](const char* m) { if (err) *err = m; return false; };
        Mf& m = mf();
        m.CoInitializeEx(nullptr, COINIT_MULTITHREADED); // this thread may be new (the audio worker)
        const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), int(path.size()), nullptr, 0);
        std::wstring w(std::size_t(std::max(n, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), int(path.size()), w.data(), n);
        IMFAttributes* attr = nullptr;
        if (FAILED(m.MFCreateAttributes(&attr, 1))) return fail("MFCreateAttributes failed");
        attr->SetUINT32(kSR_ENABLE_VIDEO_PROCESSING, TRUE);
        const HRESULT hr = m.MFCreateSourceReaderFromURL(w.c_str(), attr, &reader);
        release(attr);
        if (FAILED(hr) || !reader) return fail("Media Foundation cannot open the file");
        reader->SetStreamSelection(kAllStreams, FALSE);
        if (FAILED(reader->SetStreamSelection(stream, TRUE)))
            return fail(kind == StreamKind::Video ? "no video stream" : "no audio stream");
        IMFMediaType* want = nullptr;
        m.MFCreateMediaType(&want);
        want->SetGUID(kMT_MAJOR_TYPE, kind == StreamKind::Video ? kMediaTypeVideo : kMediaTypeAudio);
        want->SetGUID(kMT_SUBTYPE, kind == StreamKind::Video ? kRGB32 : kAudioFloat);
        const HRESULT st = reader->SetCurrentMediaType(stream, nullptr, want);
        release(want);
        if (FAILED(st)) return fail(kind == StreamKind::Video ? "no decodable video stream" : "no decodable audio stream");
        IMFMediaType* cur = nullptr;
        if (FAILED(reader->GetCurrentMediaType(stream, &cur))) return fail("no output type");
        if (kind == StreamKind::Video) {
            UINT64 size = 0, rate = 0;
            cur->GetUINT64(kMT_FRAME_SIZE, &size);
            si.width = u32(size >> 32), si.height = u32(size & 0xFFFFFFFFu);
            const u32 num = SUCCEEDED(cur->GetUINT64(kMT_FRAME_RATE, &rate)) ? u32(rate >> 32) : 0, den = u32(rate & 0xFFFFFFFFu);
            si.fps = num && den ? f32(num) / f32(den) : 30.f;
            UINT32 s = 0;
            stride = SUCCEEDED(cur->GetUINT32(kMT_DEFAULT_STRIDE, &s)) ? long(INT32(s)) : long(si.width) * 4;
            // a stride below one packed row is not a byte stride (Wine reports pixels): keep only its sign
            if ((stride < 0 ? -stride : stride) < long(si.width) * 4) stride = stride < 0 ? -long(si.width) * 4 : long(si.width) * 4;
        } else {
            UINT32 r = 0, c = 0;
            cur->GetUINT32(kMT_AUDIO_SAMPLES_PER_SECOND, &r);
            cur->GetUINT32(kMT_AUDIO_NUM_CHANNELS, &c);
            si.rate = r, si.channels = c;
        }
        release(cur);
        if (kind == StreamKind::Video && (!si.width || !si.height)) return fail("bad video size");
        if (kind == StreamKind::Audio && (!si.rate || !si.channels)) return fail("bad audio format");
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(reader->GetPresentationAttribute(kMediaSource, kPD_DURATION, &pv)) && pv.vt == VT_UI8)
            si.duration = double(pv.uhVal.QuadPart) / 1e7;
        if (kind == StreamKind::Video) {
            if (si.duration > 0.0) si.frames = u32(std::llround(si.duration * double(si.fps)));
            if (!si.frames) { // count once
                std::vector<u32> tmp(std::size_t(si.width) * si.height);
                u32 idx = 0, n = 0;
                while (readFrame(tmp.data(), idx)) n = std::max(n, idx + 1);
                si.frames = n, si.atEnd = true;
                si.duration = double(n) / si.fps;
            }
            if (!si.frames) return fail("no video frames");
        }
        return true;
    }
    StreamInfo info() const override { return si; }
    bool seekTime(double t) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = LONGLONG(std::llround(std::max(0.0, t) * 1e7));
        ended = false;
        return SUCCEEDED(reader->SetCurrentPosition(kGuidNull, pv));
    }
    // MF seeks to the key frame before the target; readFrame reports true indices so the caller skips ahead
    bool seekFrame(u32 f) override { return seekTime(double(f) / double(si.fps)); }
    bool seekSeconds(double s) override { return seekTime(s); }
    IMFSample* next(LONGLONG& ts) {
        while (!ended) {
            DWORD idx = 0, flags = 0;
            IMFSample* s = nullptr;
            if (FAILED(reader->ReadSample(stream, 0, &idx, &flags, &ts, &s)) || (flags & kFlagError)) return ended = true, nullptr;
            if (flags & kFlagEnd) ended = true;
            if (s) return s;
        }
        return nullptr;
    }
    bool readFrame(u32* rgba, u32& index) override {
        LONGLONG ts = 0;
        IMFSample* s = next(ts);
        if (!s) return false;
        IMFMediaBuffer* b = nullptr;
        bool ok = false;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                const long as = stride < 0 ? -stride : stride;
                if (len >= DWORD(as) * si.height) {
                    for (u32 y = 0; y < si.height; ++y) {
                        const BYTE* row = p + (stride < 0 ? std::size_t(si.height - 1 - y) : std::size_t(y)) * as;
                        u32* o = rgba + std::size_t(y) * si.width;
                        for (u32 x = 0; x < si.width; ++x) // BGRX -> RGBA
                            o[x] = u32(row[x * 4 + 2]) | u32(row[x * 4 + 1]) << 8 | u32(row[x * 4]) << 16 | 0xFF000000u;
                    }
                    index = u32(std::llround(double(ts) / 1e7 * double(si.fps)));
                    ok = true;
                }
                b->Unlock();
            }
            release(b);
        }
        s->Release();
        return ok;
    }
    std::size_t readAudio(std::vector<f32>& out) override {
        LONGLONG ts = 0;
        IMFSample* s = next(ts);
        if (!s) return 0;
        std::size_t n = 0;
        IMFMediaBuffer* b = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                n = len / sizeof(f32);
                const std::size_t at = out.size();
                out.resize(at + n);
                std::memcpy(out.data() + at, p, n * sizeof(f32));
                b->Unlock();
            }
            release(b);
        }
        s->Release();
        return n ? n : 1;
    }
};
} // namespace

bool backendAvailable() { return mf().ok; }
std::string backendName() { return mf().ok ? "media foundation" : std::string(); }
std::unique_ptr<Decoder> openFile(const std::string& path, StreamKind kind, std::string* error) {
    if (!mf().ok) {
        if (error) *error = "Media Foundation is not available";
        return nullptr;
    }
    auto d = std::make_unique<MfDecoder>(kind);
    if (!d->open(path, error)) return nullptr;
    return d;
}
std::unique_ptr<Decoder> openPipeline(const std::string&, StreamKind, std::string* error) {
    if (error) *error = "pipeline descriptions need GStreamer";
    return nullptr;
}

} // namespace fuse::ui::media::detail
#endif
