// os::AudioOutput: float PCM playback over the platform's audio API, loaded at runtime through os::loadLibrary
// (nothing is linked at build time). See os.hpp.
#include "fuse/ui/os.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace fuse::ui::os {
namespace {
bool fail(std::string* err, std::string m) {
    if (err) *err = std::move(m);
    return false;
}

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
// ---- PulseAudio simple API ----
struct PaSampleSpec {
    int format;
    std::uint32_t rate;
    std::uint8_t channels;
};
class PulseOut final : public AudioOutput {
public:
    std::unique_ptr<Library> lib;
    void* s = nullptr;
    std::uint32_t rate = 0, ch = 0;
    std::uint64_t written = 0;
    void* (*pa_simple_new)(const char*, const char*, int, const char*, const char*, const PaSampleSpec*, const void*, const void*, int*) = nullptr;
    int (*pa_simple_write)(void*, const void*, std::size_t, int*) = nullptr;
    int (*pa_simple_drain)(void*, int*) = nullptr;
    void (*pa_simple_free)(void*) = nullptr;
    std::uint64_t (*pa_simple_get_latency)(void*, int*) = nullptr;
    ~PulseOut() override {
        if (s) pa_simple_free(s);
    }
    bool open(std::uint32_t r, std::uint32_t c, std::string* err) {
        lib = loadLibrary({"libpulse-simple.so.0", "libpulse-simple.so"});
        if (!lib) return fail(err, "libpulse-simple not found");
        pa_simple_new = lib->get<decltype(pa_simple_new)>("pa_simple_new");
        pa_simple_write = lib->get<decltype(pa_simple_write)>("pa_simple_write");
        pa_simple_drain = lib->get<decltype(pa_simple_drain)>("pa_simple_drain");
        pa_simple_free = lib->get<decltype(pa_simple_free)>("pa_simple_free");
        pa_simple_get_latency = lib->get<decltype(pa_simple_get_latency)>("pa_simple_get_latency");
        if (!pa_simple_new || !pa_simple_write || !pa_simple_drain || !pa_simple_free) return fail(err, "libpulse-simple incomplete");
        const PaSampleSpec spec{5 /*PA_SAMPLE_FLOAT32LE*/, r, std::uint8_t(c)};
        int e = 0;
        s = pa_simple_new(nullptr, "FUSE", 1 /*PA_STREAM_PLAYBACK*/, nullptr, "playback", &spec, nullptr, nullptr, &e);
        if (!s) return fail(err, "PulseAudio: no server");
        rate = r, ch = c;
        return true;
    }
    bool write(const float* d, std::size_t frames) override {
        int e = 0;
        if (pa_simple_write(s, d, frames * ch * sizeof(float), &e) < 0) return false;
        written += frames;
        return true;
    }
    void drain() override {
        int e = 0;
        pa_simple_drain(s, &e);
    }
    std::uint64_t framesWritten() const override { return written; }
    double latencySeconds() const override {
        int e = 0;
        return pa_simple_get_latency ? double(pa_simple_get_latency(s, &e)) / 1e6 : 0.0;
    }
    std::string backend() const override { return "pulse"; }
    std::uint32_t sampleRate() const override { return rate; }
    std::uint32_t channels() const override { return ch; }
};

// ---- ALSA ----
class AlsaOut final : public AudioOutput {
public:
    std::unique_ptr<Library> lib;
    void* pcm = nullptr;
    std::string name;
    std::uint32_t rate = 0, ch = 0;
    std::uint64_t written = 0;
    int (*snd_pcm_open)(void**, const char*, int, int) = nullptr;
    int (*snd_pcm_set_params)(void*, int, int, unsigned, unsigned, int, unsigned) = nullptr;
    long (*snd_pcm_writei)(void*, const void*, unsigned long) = nullptr;
    int (*snd_pcm_recover)(void*, int, int) = nullptr;
    int (*snd_pcm_drain)(void*) = nullptr;
    int (*snd_pcm_close)(void*) = nullptr;
    int (*snd_pcm_delay)(void*, long*) = nullptr;
    ~AlsaOut() override {
        if (pcm) snd_pcm_close(pcm);
    }
    bool open(std::uint32_t r, std::uint32_t c, std::string_view dev, std::string* err) {
        lib = loadLibrary({"libasound.so.2", "libasound.so"});
        if (!lib) return fail(err, "libasound not found");
#define A(n) n = lib->get<decltype(n)>(#n)
        A(snd_pcm_open); A(snd_pcm_set_params); A(snd_pcm_writei); A(snd_pcm_recover); A(snd_pcm_drain); A(snd_pcm_close); A(snd_pcm_delay);
#undef A
        if (!snd_pcm_open || !snd_pcm_set_params || !snd_pcm_writei || !snd_pcm_close) return fail(err, "libasound incomplete");
        name = dev.empty() ? "default" : std::string(dev);
        if (snd_pcm_open(&pcm, name.c_str(), 0 /*PLAYBACK*/, 0) < 0) {
            pcm = nullptr;
            return fail(err, "ALSA: cannot open " + name);
        }
        // FLOAT_LE = 14, RW_INTERLEAVED = 3, soft resample on, 100 ms latency
        if (snd_pcm_set_params(pcm, 14, 3, c, r, 1, 100000) < 0) return fail(err, "ALSA: format not supported");
        rate = r, ch = c;
        return true;
    }
    bool write(const float* d, std::size_t frames) override {
        while (frames) {
            long n = snd_pcm_writei(pcm, d, frames);
            if (n < 0) {
                if (!snd_pcm_recover || snd_pcm_recover(pcm, int(n), 1) < 0) return false;
                continue;
            }
            d += std::size_t(n) * ch, frames -= std::size_t(n), written += std::uint64_t(n);
        }
        return true;
    }
    void drain() override {
        if (snd_pcm_drain) snd_pcm_drain(pcm);
    }
    std::uint64_t framesWritten() const override { return written; }
    double latencySeconds() const override {
        long d = 0;
        return snd_pcm_delay && snd_pcm_delay(pcm, &d) == 0 && rate ? double(d) / rate : 0.0;
    }
    std::string backend() const override { return "alsa:" + name; }
    std::uint32_t sampleRate() const override { return rate; }
    std::uint32_t channels() const override { return ch; }
};
#endif

#if defined(_WIN32)
// ---- waveOut (winmm), IEEE float, a ring of blocks ----
class WinmmOut final : public AudioOutput {
public:
    std::unique_ptr<Library> lib;
    HWAVEOUT h = nullptr;
    HANDLE ev = nullptr;
    std::uint32_t rate = 0, ch = 0;
    std::uint64_t written = 0, done = 0;
    static constexpr int kBlocks = 6;
    struct Block {
        WAVEHDR hdr{};
        std::vector<float> data;
        bool prepared = false;
    } blocks[kBlocks];
    int cur = 0;
    MMRESULT(WINAPI* wOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD) = nullptr;
    MMRESULT(WINAPI* wPrepare)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
    MMRESULT(WINAPI* wUnprepare)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
    MMRESULT(WINAPI* wWrite)(HWAVEOUT, LPWAVEHDR, UINT) = nullptr;
    MMRESULT(WINAPI* wReset)(HWAVEOUT) = nullptr;
    MMRESULT(WINAPI* wClose)(HWAVEOUT) = nullptr;
    ~WinmmOut() override {
        if (h) {
            wReset(h);
            for (Block& b : blocks)
                if (b.prepared) wUnprepare(h, &b.hdr, sizeof(WAVEHDR));
            wClose(h);
        }
        if (ev) CloseHandle(ev);
    }
    bool open(std::uint32_t r, std::uint32_t c, std::string* err) {
        lib = loadLibrary({"winmm.dll"});
        if (!lib) return fail(err, "winmm.dll not found");
        wOpen = lib->get<decltype(wOpen)>("waveOutOpen");
        wPrepare = lib->get<decltype(wPrepare)>("waveOutPrepareHeader");
        wUnprepare = lib->get<decltype(wUnprepare)>("waveOutUnprepareHeader");
        wWrite = lib->get<decltype(wWrite)>("waveOutWrite");
        wReset = lib->get<decltype(wReset)>("waveOutReset");
        wClose = lib->get<decltype(wClose)>("waveOutClose");
        if (!wOpen || !wPrepare || !wUnprepare || !wWrite || !wReset || !wClose) return fail(err, "winmm incomplete");
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEX f{};
        f.wFormatTag = 3; // WAVE_FORMAT_IEEE_FLOAT
        f.nChannels = WORD(c), f.nSamplesPerSec = r, f.wBitsPerSample = 32;
        f.nBlockAlign = WORD(c * 4), f.nAvgBytesPerSec = r * c * 4;
        if (wOpen(&h, WAVE_MAPPER, &f, DWORD_PTR(ev), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            h = nullptr;
            return fail(err, "waveOutOpen failed (no device)");
        }
        rate = r, ch = c;
        return true;
    }
    void wait(Block& b) {
        while (b.prepared && !(b.hdr.dwFlags & WHDR_DONE)) WaitForSingleObject(ev, 50);
        if (b.prepared) {
            done += b.hdr.dwBufferLength / (4 * ch);
            wUnprepare(h, &b.hdr, sizeof(WAVEHDR));
            b.prepared = false;
        }
    }
    bool write(const float* d, std::size_t frames) override {
        const std::size_t chunk = std::max<std::size_t>(rate / 20, 256); // 50 ms blocks
        while (frames) {
            const std::size_t n = std::min(frames, chunk);
            Block& b = blocks[cur];
            wait(b);
            b.data.assign(d, d + n * ch);
            b.hdr = {};
            b.hdr.lpData = reinterpret_cast<LPSTR>(b.data.data());
            b.hdr.dwBufferLength = DWORD(n * ch * 4);
            if (wPrepare(h, &b.hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return false;
            b.prepared = true;
            if (wWrite(h, &b.hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return false;
            cur = (cur + 1) % kBlocks;
            d += n * ch, frames -= n, written += n;
        }
        return true;
    }
    void drain() override {
        for (Block& b : blocks) wait(b);
    }
    std::uint64_t framesWritten() const override { return written; }
    double latencySeconds() const override { return rate ? double(written - done) / rate : 0.0; }
    std::string backend() const override { return "winmm"; }
    std::uint32_t sampleRate() const override { return rate; }
    std::uint32_t channels() const override { return ch; }
};
#endif
} // namespace

std::unique_ptr<AudioOutput> openAudioOutput(std::uint32_t rate, std::uint32_t channels, std::string* error, std::string_view device) {
    if (rate == 0 || channels == 0 || channels > 8) {
        if (error) *error = "bad audio format";
        return nullptr;
    }
    std::string dev(device);
    if (dev.empty()) dev = env("FUSE_UI_AUDIO_DEVICE");
    std::string e1, e2;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (!dev.starts_with("alsa:") && (dev.empty() || dev == "pulse")) {
        auto p = std::make_unique<PulseOut>();
        if (p->open(rate, channels, &e1)) return p;
        if (dev == "pulse") {
            if (error) *error = e1;
            return nullptr;
        }
    }
    auto a = std::make_unique<AlsaOut>();
    if (a->open(rate, channels, dev.starts_with("alsa:") ? std::string_view(dev).substr(5) : std::string_view(dev), &e2)) return a;
    if (error) *error = e1.empty() ? e2 : e1 + "; " + e2;
    return nullptr;
#elif defined(_WIN32)
    auto w = std::make_unique<WinmmOut>();
    if (w->open(rate, channels, &e1)) return w;
    if (error) *error = e1;
    return nullptr;
#else
    if (error) *error = "no audio output backend on this platform";
    return nullptr;
#endif
}

} // namespace fuse::ui::os
