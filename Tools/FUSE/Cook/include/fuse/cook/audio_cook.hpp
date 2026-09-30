#pragma once

// Audio cook chain (MP-B7.9-AUDIO-IMPORT / AP-W8.3 / UNI-U7-AUDIO-1, cook half of MP-B7.2-OGG-RUNTIME).
//
//   decode (WAV 8/16/24/32-bit int, 32/64-bit float, WAVE_FORMAT_EXTENSIBLE; native FLAC; Ogg Vorbis)
//   -> optional mono downmix
//   -> windowed-sinc polyphase resample to the target rate (48 kHz default)
//   -> trim leading / trailing silence
//   -> optional seamless loop (zero-crossing aligned equal-power crossfade, loop-seam click check)
//   -> EBU R128 / ITU-R BS.1770-4 integrated-loudness normalisation (beds -23 LUFS, one-shots -16 LUFS)
//      with a look-ahead peak limiter for the ceiling, or plain peak normalisation
//   -> Ogg Vorbis quality VBR (q4 beds, q5 one-shots; fixed stream serial) or PCM_F32
//   -> `.fuseaudio` container (the layout parsed by fuse::audio::parse_fuseaudio).
//
// Every stage is deterministic: the same source and options give byte-identical output.
// FLAC / Ogg Vorbis come from the vendored xiph libraries (cmake/FuseXiph.cmake); MP3 is not decoded.

#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

/// Interleaved f32 PCM (nominal range [-1, 1]).
struct AudioBuffer {
    std::vector<float> samples;
    u32 frames = 0;
    u32 channels = 0;
    u32 sample_rate = 0;
};

enum class AudioSourceFormat : u8 { Unknown, Wav, Flac, OggVorbis, Mp3 };

/// Sound class: picks the loudness target and the Vorbis quality (FUSE_ASSET_PLAN §1.2 audio rows).
/// Auto = Bed when the trimmed clip is at least `kAudioAutoBedSeconds` long, else OneShot.
enum class AudioClass : u8 { Auto, OneShot, Bed };
enum class AudioNormalise : u8 { None, Loudness, Peak };
enum class AudioCookFormat : u8 { OggVorbis, PcmF32 };

inline constexpr f32 kAudioBedLufs = -23.0f;
inline constexpr f32 kAudioOneShotLufs = -16.0f;
inline constexpr f32 kAudioBedVorbisQuality = 0.4f;     ///< Vorbis q4 (vorbis_encode_init_vbr scale 0..1)
inline constexpr f32 kAudioOneShotVorbisQuality = 0.5f; ///< Vorbis q5
inline constexpr f32 kAudioAutoBedSeconds = 8.0f;
inline constexpr u32 kAudioDefaultSampleRate = 48000u;
inline constexpr u32 kAudioOggSerial = 0x46434F4Bu; ///< "FCOK": fixed Ogg stream serial (deterministic bytes)
/// Loudness reported for silence / clips with no block above the absolute gate.
inline constexpr f64 kAudioSilenceLufs = -144.0;

struct AudioCookOptions {
    u32 target_sample_rate = kAudioDefaultSampleRate; ///< 0 keeps the source rate
    bool force_mono = false;                          ///< average all channels into one
    AudioClass audio_class = AudioClass::Auto;
    AudioNormalise normalise = AudioNormalise::Loudness;
    f32 target_lufs = 0.0f;       ///< 0 = class default (-23 / -16 LUFS)
    f32 peak_ceiling_dbfs = -1.0f; ///< sample-peak ceiling enforced after loudness gain (limiter)
    f32 peak_target_dbfs = -1.0f;  ///< AudioNormalise::Peak target
    bool trim_silence = true;
    f32 trim_threshold_db = -60.0f; ///< relative to the clip's sample peak
    u32 trim_head_pad_ms = 1;
    u32 trim_tail_pad_ms = 20;
    bool make_loop = false;
    u32 loop_crossfade_ms = 50; ///< clamped to a quarter of the clip
    AudioCookFormat format = AudioCookFormat::OggVorbis;
    f32 ogg_quality = -1.0f; ///< < -0.1 = class default (q4 beds, q5 one-shots); else -0.1 .. 1.0
};

struct AudioCookReport {
    AudioSourceFormat source_format = AudioSourceFormat::Unknown;
    u32 source_rate = 0;
    u32 source_channels = 0;
    u32 source_frames = 0;
    u32 output_rate = 0;
    u32 output_channels = 0;
    u32 output_frames = 0;
    u32 trimmed_head_frames = 0;
    u32 trimmed_tail_frames = 0;
    AudioClass resolved_class = AudioClass::OneShot;
    f64 source_lufs = kAudioSilenceLufs;
    f64 output_lufs = kAudioSilenceLufs; ///< measured on the PCM handed to the encoder
    f64 target_lufs = 0.0;
    f64 gain_db = 0.0;
    f32 output_peak_dbfs = -144.0f;
    bool peak_limited = false; ///< the limiter engaged to hold the ceiling
    bool looped = false;
    u32 loop_crossfade_frames = 0;
    f32 loop_seam_ratio = 0.0f; ///< see loop_seam_ratio(); <= kAudioLoopSeamClickRatio passes
    bool loop_seam_click = false;
    f32 ogg_quality = 0.0f;
    u32 payload_bytes = 0;
};

/// Seam jump larger than this multiple of the local sample-to-sample slope is a click.
inline constexpr f32 kAudioLoopSeamClickRatio = 2.0f;

// ---- individual stages (exposed for tests and tools) --------------------------------------------------

bool audio_cook_vorbis_available();
bool audio_cook_flac_available();

AudioSourceFormat sniff_audio_format(const u8* bytes, usize size);
const char* audio_source_format_name(AudioSourceFormat format);
const char* audio_class_name(AudioClass audio_class);

bool decode_wav(const u8* bytes, usize size, AudioBuffer& out, std::string* error = nullptr);
bool decode_flac_stream(const u8* bytes, usize size, AudioBuffer& out, std::string* error = nullptr);
bool decode_ogg_vorbis_stream(const u8* bytes, usize size, AudioBuffer& out, std::string* error = nullptr);
/// Sniffs the container and dispatches to the matching decoder.
bool decode_audio(const u8* bytes, usize size, AudioBuffer& out, std::string* error = nullptr,
                  AudioSourceFormat* detected = nullptr);
bool decode_audio_file(const std::string& path, AudioBuffer& out, std::string* error = nullptr,
                       AudioSourceFormat* detected = nullptr);

void downmix_to_mono(AudioBuffer& buffer);

/// Band-limited resample (Kaiser-windowed sinc, 32 zero crossings at the lower rate, exact rational
/// polyphase table when the reduced upsampling factor is <= 2048, else 2048 interpolated phases).
AudioBuffer resample_audio(const AudioBuffer& in, u32 target_rate);

/// ITU-R BS.1770-4 integrated loudness (K-weighting, 400 ms blocks with 75 % overlap, -70 LUFS absolute
/// and -10 LU relative gates). Channel weights: 1.0, except 1.41 for channels 4/5 of a 5.1 layout.
/// A clip shorter than one block is measured as a single block over its whole length.
f64 measure_integrated_loudness(const AudioBuffer& buffer);
f32 sample_peak(const AudioBuffer& buffer);
void apply_gain(AudioBuffer& buffer, f64 gain_linear);
/// Look-ahead (1.5 ms) peak limiter holding |x| <= ceiling_linear; returns true when it engaged.
bool limit_peaks(AudioBuffer& buffer, f32 ceiling_linear);

/// Drop leading / trailing frames below `threshold_db` relative to the clip peak (keeping the pads).
void trim_silence(AudioBuffer& buffer, f32 threshold_db, u32 head_pad_frames, u32 tail_pad_frames,
                  u32* trimmed_head = nullptr, u32* trimmed_tail = nullptr);

/// Make the clip loop seamlessly: the head starts at a rising zero crossing, the tail is cut at a
/// rising zero crossing `crossfade_frames` before the end and the removed tail is equal-power
/// crossfaded into the head, so playback wraps from the last frame to frame 0 continuously.
bool make_seamless_loop(AudioBuffer& buffer, u32 crossfade_frames, u32* used_crossfade = nullptr);
/// Seam jump |x[0] - x[N-1]| divided by the largest adjacent-sample step within 64 frames of the seam
/// (max over channels). A continuous loop is <= ~1; a click is well above kAudioLoopSeamClickRatio.
f32 loop_seam_ratio(const AudioBuffer& buffer);

/// Quality-VBR Ogg Vorbis encode with a fixed stream serial (deterministic bytes).
bool encode_vorbis_stream(const AudioBuffer& buffer, f32 quality, std::vector<u8>& out,
                          u32 serial = kAudioOggSerial);

// ---- full chain -------------------------------------------------------------------------------------

/// Runs the chain on an already-decoded buffer and writes `.fuseaudio` (FUSEAUDIO_OGG / FUSEAUDIO_PCM_F32).
CookStubWriteResult cook_audio_buffer(AudioBuffer buffer, const std::string& output_path,
                                      const AudioCookOptions& options, AudioCookReport* report = nullptr);
/// Decode `input_path` (WAV / FLAC / Ogg Vorbis) and run the chain.
CookStubWriteResult cook_audio_file(const std::string& input_path, const std::string& output_path,
                                    const AudioCookOptions& options, AudioCookReport* report = nullptr);

} // namespace fuse::cook
