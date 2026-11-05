#pragma once

// FUSE_ASSET_PLAN §5.3 validation rules shared by `fuse_assetcheck` (the gate tool, descriptor driven) and
// the strict cook (fuse::project::AssetCooker refuses assets that fail them). Header only, standard library
// only, so the standalone tool and the cook link the same rule code and cannot drift apart.
//
// Each check appends human-readable violations; an empty list means the rule passed. The measurement
// helpers (BS.1770 loudness, loop seam ratio, sample peak) are also the cook chain's own meters
// (Tools/FUSE/Cook/src/audio_cook.cpp forwards to them), so the gate measures exactly what the cook
// normalised against.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fuse::assetcheck {

using Violations = std::vector<std::string>;

inline std::string fmtNumber(double v, int precision = 3) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return buffer;
}

inline std::uint64_t fnv1a64(const void* data, std::size_t size, std::uint64_t hash = 14695981039346656037ull) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= std::uint64_t(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

// ---- geometry (asset_geometry) ----------------------------------------------------------------------------

/// Smallest triangle area (square metres) that is not "zero-area".
inline constexpr double kMinTriangleArea = 1e-10;

/// Triangle list sanity: finite positions, indices in range, no degenerate (repeated index) or zero-area
/// triangles. `Real` is float or double, xyz per vertex. `where` prefixes the messages.
template <class Real>
void checkTriangles(const Real* positions, std::size_t vertexCount, const std::uint32_t* indices, std::size_t indexCount,
                    const std::string& where, Violations& v) {
    std::size_t nonFinite = 0, degenerate = 0, outOfRange = 0;
    for (std::size_t i = 0; i < vertexCount * 3u; ++i) {
        if (!std::isfinite(double(positions[i]))) ++nonFinite;
    }
    if (indexCount % 3u != 0u) {
        v.push_back(where + ": index count " + std::to_string(indexCount) + " is not a multiple of 3");
    }
    for (std::size_t t = 0; t + 2u < indexCount; t += 3u) {
        const std::uint32_t a = indices[t], b = indices[t + 1u], c = indices[t + 2u];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) {
            ++outOfRange;
            continue;
        }
        if (a == b || b == c || a == c) {
            ++degenerate;
            continue;
        }
        const double ux = double(positions[b * 3u]) - double(positions[a * 3u]);
        const double uy = double(positions[b * 3u + 1u]) - double(positions[a * 3u + 1u]);
        const double uz = double(positions[b * 3u + 2u]) - double(positions[a * 3u + 2u]);
        const double vx = double(positions[c * 3u]) - double(positions[a * 3u]);
        const double vy = double(positions[c * 3u + 1u]) - double(positions[a * 3u + 1u]);
        const double vz = double(positions[c * 3u + 2u]) - double(positions[a * 3u + 2u]);
        const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        if (!(0.5 * std::sqrt(cx * cx + cy * cy + cz * cz) > kMinTriangleArea)) ++degenerate;
    }
    if (nonFinite > 0u) v.push_back(where + ": " + std::to_string(nonFinite) + " non-finite position components");
    if (outOfRange > 0u) v.push_back(where + ": " + std::to_string(outOfRange) + " triangles index past the vertex count");
    if (degenerate > 0u) v.push_back(where + ": " + std::to_string(degenerate) + " degenerate / zero-area triangles");
}

// ---- LOD presence (asset_lod_presence) --------------------------------------------------------------------

/// §5.3: a mesh drawn farther than this needs LODs, an impostor or a cluster DAG.
inline constexpr double kLodPresenceDistance = 50.0;

/// `lodLevels` counts LOD 0 (separate LOD files and LOD sections embedded in the FMSH both count).
inline void checkLodPresence(double maxViewDistance, std::size_t lodLevels, bool impostor, bool dag, Violations& v) {
    if (!(maxViewDistance > kLodPresenceDistance)) {
        return;
    }
    if (lodLevels >= 2u || impostor || dag) {
        return;
    }
    v.push_back("mesh is drawn up to " + fmtNumber(maxViewDistance, 0) + " m (> " + fmtNumber(kLodPresenceDistance, 0) +
                " m) but has no LOD chain, impostor or cluster DAG (" + std::to_string(lodLevels) + " LOD level)");
}

// ---- skin (asset_skin) --------------------------------------------------------------------------------------

inline constexpr std::size_t kMaxSkinInfluences = 4;
inline constexpr double kSkinWeightSumTolerance = 1e-3;

/// Skeleton identity hash: FNV-1a 64 over, per joint in order, the name bytes, a 0 byte and the parent index
/// (s32 little endian). Bind matrices are not part of the identity (a rig can be re-posed without re-skinning).
inline std::uint64_t skeletonHash(const std::vector<std::string>& names, const std::vector<std::int32_t>& parents) {
    std::uint64_t h = 14695981039346656037ull;
    for (std::size_t i = 0; i < names.size(); ++i) {
        h = fnv1a64(names[i].data(), names[i].size(), h);
        const std::uint8_t zero = 0;
        h = fnv1a64(&zero, 1u, h);
        const std::int32_t p = i < parents.size() ? parents[i] : -1;
        const std::uint8_t le[4] = {std::uint8_t(std::uint32_t(p) & 0xffu), std::uint8_t((std::uint32_t(p) >> 8) & 0xffu),
                                    std::uint8_t((std::uint32_t(p) >> 16) & 0xffu), std::uint8_t((std::uint32_t(p) >> 24) & 0xffu)};
        h = fnv1a64(le, 4u, h);
    }
    return h;
}

/// Parse an FSKL v1 skeleton (fuse::animation::Skeleton::save: "FSKL", u32 version 1, u32 bone count, per bone
/// char name[64], s32 parent, 2 x mat4) into names + parents.
inline bool parseFskl(const std::uint8_t* d, std::size_t size, std::vector<std::string>& names,
                      std::vector<std::int32_t>& parents, std::string& error) {
    constexpr std::size_t kBone = 64u + 4u + 2u * 64u;
    auto u32At = [&](std::size_t o) {
        return std::uint32_t(d[o]) | (std::uint32_t(d[o + 1]) << 8) | (std::uint32_t(d[o + 2]) << 16) | (std::uint32_t(d[o + 3]) << 24);
    };
    if (size < 12u || std::memcmp(d, "FSKL", 4) != 0 || u32At(4) != 1u) {
        error = "not an FSKL v1 skeleton";
        return false;
    }
    const std::uint32_t count = u32At(8);
    if (size != 12u + std::size_t(count) * kBone) {
        error = "FSKL size mismatch";
        return false;
    }
    names.clear();
    parents.clear();
    for (std::uint32_t b = 0; b < count; ++b) {
        const std::uint8_t* bone = d + 12u + std::size_t(b) * kBone;
        std::size_t len = 0;
        while (len < 64u && bone[len] != 0u) ++len;
        names.emplace_back(reinterpret_cast<const char*>(bone), len);
        parents.push_back(std::int32_t(u32At(std::size_t(bone - d) + 64u)));
    }
    return true;
}

/// Per-vertex influences: `stride` joint / weight slots per vertex (FMSH stores 4). Checks: weights finite and
/// in [0, 1], at most kMaxSkinInfluences non-zero weights, weights sum to 1 (+-kSkinWeightSumTolerance), joint
/// indices of weighted slots < `rigJointCount` (skipped when 0: rig unknown).
inline void checkSkinWeights(const std::uint16_t* joints, const float* weights, std::size_t vertexCount, std::size_t stride,
                             std::uint32_t rigJointCount, const std::string& where, Violations& v) {
    std::size_t badSum = 0, tooMany = 0, badWeight = 0, badJoint = 0, firstBadSum = 0;
    double worstSum = 1.0;
    for (std::size_t vtx = 0; vtx < vertexCount; ++vtx) {
        double sum = 0.0;
        std::size_t used = 0;
        for (std::size_t k = 0; k < stride; ++k) {
            const double w = weights[vtx * stride + k];
            if (!std::isfinite(w) || w < 0.0 || w > 1.0) {
                ++badWeight;
                continue;
            }
            sum += w;
            if (w > 0.0) {
                ++used;
                if (rigJointCount != 0u && joints[vtx * stride + k] >= rigJointCount) ++badJoint;
            }
        }
        if (used > kMaxSkinInfluences) ++tooMany;
        if (std::fabs(sum - 1.0) > kSkinWeightSumTolerance) {
            if (badSum == 0u) {
                firstBadSum = vtx;
                worstSum = sum;
            }
            ++badSum;
        }
    }
    if (badWeight > 0u) v.push_back(where + ": " + std::to_string(badWeight) + " skin weights non-finite or outside [0,1]");
    if (tooMany > 0u) {
        v.push_back(where + ": " + std::to_string(tooMany) + " vertices with more than " + std::to_string(kMaxSkinInfluences) +
                    " weights per vertex");
    }
    if (badSum > 0u) {
        v.push_back(where + ": skin weights of " + std::to_string(badSum) + " vertices do not sum to 1 (vertex " +
                    std::to_string(firstBadSum) + " sums to " + fmtNumber(worstSum, 4) + ")");
    }
    if (badJoint > 0u) {
        v.push_back(where + ": " + std::to_string(badJoint) + " weighted joint indices past the rig's " + std::to_string(rigJointCount) +
                    " joints");
    }
}

inline std::string hashHex(std::uint64_t h) {
    char b[24];
    std::snprintf(b, sizeof(b), "0x%016llx", static_cast<unsigned long long>(h));
    return b;
}

inline void checkSkeletonHash(std::uint64_t declared, std::uint64_t rig, Violations& v) {
    if (declared != rig) {
        v.push_back("skeleton hash " + hashHex(declared) + " does not match the rig (" + hashHex(rig) + ")");
    }
}

// ---- audio (asset_audio) ------------------------------------------------------------------------------------

inline constexpr std::uint32_t kAudioSampleRate = 48000u;
inline constexpr double kAudioLoudnessToleranceLu = 1.0;
/// A sample at or above this magnitude (-0.009 dBFS) counts as clipped.
inline constexpr double kAudioClipLevel = 0.999;
/// Loop seam jump larger than this multiple of the local sample-to-sample slope is a click.
inline constexpr double kAudioLoopSeamClickRatio = 2.0;
inline constexpr double kAudioSilenceLufs = -144.0;
inline constexpr double kAudioOneShotLufs = -16.0;
inline constexpr double kAudioBedLufs = -23.0;

namespace detail {
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

/// ITU-R BS.1770-4 K-weighting (pre-filter high shelf + RLB high-pass) for `rate`.
inline void kWeighting(std::uint32_t rate, Biquad& shelf, Biquad& highpass) {
    constexpr double kPi = 3.14159265358979323846;
    const double fs = static_cast<double>(rate);
    {
        const double f0 = 1681.974450955533;
        const double gain_db = 3.999843853973347;
        const double q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / fs);
        const double vh = std::pow(10.0, gain_db / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + k / q + k * k;
        highpass.b0 = 1.0;
        highpass.b1 = -2.0;
        highpass.b2 = 1.0;
        highpass.a1 = 2.0 * (k * k - 1.0) / a0;
        highpass.a2 = (1.0 - k / q + k * k) / a0;
    }
}

/// 5.1 (L R C LFE Ls Rs): LFE excluded, surrounds +1.5 dB.
inline double channelWeight(std::uint32_t channel, std::uint32_t channels) {
    if (channels == 6u) {
        if (channel == 3u) return 0.0;
        if (channel >= 4u) return 1.41;
    }
    return 1.0;
}

inline double blockLoudness(double meanSquare) {
    return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10(meanSquare) : kAudioSilenceLufs;
}
} // namespace detail

/// ITU-R BS.1770-4 integrated loudness (K-weighting, 400 ms blocks with 75 % overlap, -70 LUFS absolute and
/// -10 LU relative gates) of interleaved PCM. A clip shorter than one block is measured as a single block.
inline double integratedLoudness(const float* samples, std::uint32_t frames, std::uint32_t channels, std::uint32_t rate) {
    if (samples == nullptr || frames == 0u || channels == 0u || rate == 0u) {
        return kAudioSilenceLufs;
    }
    detail::Biquad shelf, highpass;
    detail::kWeighting(rate, shelf, highpass);
    std::vector<double> energy(frames, 0.0);
    for (std::uint32_t c = 0; c < channels; ++c) {
        const double w = detail::channelWeight(c, channels);
        if (w == 0.0) continue;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        double z1 = 0.0, z2 = 0.0, v1 = 0.0, v2 = 0.0;
        for (std::uint32_t f = 0; f < frames; ++f) {
            const double x = samples[static_cast<std::size_t>(f) * channels + c];
            const double y = shelf.b0 * x + shelf.b1 * x1 + shelf.b2 * x2 - shelf.a1 * y1 - shelf.a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            const double o = highpass.b0 * y + highpass.b1 * z1 + highpass.b2 * z2 - highpass.a1 * v1 - highpass.a2 * v2;
            z2 = z1;
            z1 = y;
            v2 = v1;
            v1 = o;
            energy[f] += w * o * o;
        }
    }
    const std::uint32_t step = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(std::lround(rate * 0.1)));
    const std::uint32_t block = step * 4u;
    std::vector<double> blocks;
    if (frames < block) {
        double sum = 0.0;
        for (double e : energy) sum += e;
        blocks.push_back(sum / frames);
    } else {
        const std::uint32_t segments = frames / step;
        std::vector<double> seg(segments, 0.0);
        for (std::uint32_t s = 0; s < segments; ++s) {
            double sum = 0.0;
            for (std::uint32_t i = 0; i < step; ++i) sum += energy[static_cast<std::size_t>(s) * step + i];
            seg[s] = sum;
        }
        for (std::uint32_t s = 0; s + 4u <= segments; ++s) {
            blocks.push_back((seg[s] + seg[s + 1u] + seg[s + 2u] + seg[s + 3u]) / block);
        }
    }
    double absSum = 0.0;
    std::size_t absCount = 0;
    for (double z : blocks) {
        if (detail::blockLoudness(z) > -70.0) {
            absSum += z;
            ++absCount;
        }
    }
    if (absCount == 0u) return kAudioSilenceLufs;
    const double relativeGate = detail::blockLoudness(absSum / static_cast<double>(absCount)) - 10.0;
    double relSum = 0.0;
    std::size_t relCount = 0;
    for (double z : blocks) {
        const double l = detail::blockLoudness(z);
        if (l > -70.0 && l > relativeGate) {
            relSum += z;
            ++relCount;
        }
    }
    if (relCount == 0u) return kAudioSilenceLufs;
    return detail::blockLoudness(relSum / static_cast<double>(relCount));
}

inline float samplePeak(const float* samples, std::size_t count) {
    float peak = 0.0f;
    for (std::size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(samples[i]));
    return peak;
}

/// Loop seam jump |x[0] - x[N-1]| divided by the largest adjacent-sample step within 64 frames of the seam
/// (max over channels). A continuous loop is <= ~1; a click is well above kAudioLoopSeamClickRatio.
inline float loopSeamRatio(const float* samples, std::uint32_t frames, std::uint32_t channels) {
    if (samples == nullptr || channels == 0u || frames < 4u) return 0.0f;
    const std::uint32_t window = std::min<std::uint32_t>(64u, frames / 2u);
    double worst = 0.0;
    for (std::uint32_t c = 0; c < channels; ++c) {
        auto at = [&](std::uint32_t f) { return static_cast<double>(samples[static_cast<std::size_t>(f) * channels + c]); };
        const double jump = std::abs(at(0) - at(frames - 1u));
        double local = 0.0;
        for (std::uint32_t i = 1; i < window; ++i) {
            local = std::max(local, std::abs(at(i) - at(i - 1u)));
            const std::uint32_t t = frames - i;
            local = std::max(local, std::abs(at(t) - at(t - 1u)));
        }
        const double ratio = jump <= 1e-6 ? 0.0 : jump / std::max(local, 1e-6);
        worst = std::max(worst, ratio);
    }
    return static_cast<float>(worst);
}

/// What the audio rule looks at (measured by the caller with the helpers above).
struct AudioFacts {
    std::uint32_t sampleRate = 0;
    double peak = 0.0;               ///< linear sample peak
    double lufs = kAudioSilenceLufs; ///< integrated loudness
    bool hasTarget = false;          ///< loudness target applies (normalised clip)
    double targetLufs = 0.0;
    bool loop = false;
    double seamRatio = 0.0;          ///< loopSeamRatio (when `loop`)
};

/// asset_audio: 48 kHz, loudness within +-1 LU of the target, no clipping, no click at the loop seam.
inline void checkAudio(const AudioFacts& a, const std::string& where, Violations& v) {
    if (a.sampleRate != kAudioSampleRate) {
        v.push_back(where + ": sample rate " + std::to_string(a.sampleRate) + " Hz, FUSE audio is 48 kHz");
    }
    if (a.peak >= kAudioClipLevel) {
        v.push_back(where + ": clipping (sample peak " + fmtNumber(20.0 * std::log10(std::max(a.peak, 1e-9)), 2) + " dBFS)");
    }
    if (a.hasTarget && !(std::fabs(a.lufs - a.targetLufs) <= kAudioLoudnessToleranceLu)) {
        v.push_back(where + ": loudness " + fmtNumber(a.lufs, 2) + " LUFS, target " + fmtNumber(a.targetLufs, 1) + " LUFS +-" +
                    fmtNumber(kAudioLoudnessToleranceLu, 0) + " LU");
    }
    if (a.loop && a.seamRatio > kAudioLoopSeamClickRatio) {
        v.push_back(where + ": loop seam click (seam ratio " + fmtNumber(a.seamRatio, 2) + " > " +
                    fmtNumber(kAudioLoopSeamClickRatio, 1) + ")");
    }
}

// ---- texture (asset_budget_texture subset the cook can check) ------------------------------------------------

inline bool isPowerOfTwo(std::uint32_t x) { return x != 0u && (x & (x - 1u)) == 0u; }

/// Normal maps are linear and BC5 (§1.3). `format` empty: not checked.
inline void checkNormalMapEncoding(bool normalMap, bool srgb, const std::string& format, const std::string& where, Violations& v) {
    if (normalMap && srgb) v.push_back(where + ": normal map must be linear, not sRGB");
    if (normalMap && !format.empty() && format != "BC5") {
        v.push_back(where + ": normal map format '" + format + "' (normal maps are BC5, never BC7/BC1)");
    }
}

/// Content-library textures (fuse_assetcheck budget_texture): power-of-two dimensions + checkNormalMapEncoding.
/// The cook only applies checkNormalMapEncoding: it also cooks UI / logo / sample images of any size.
inline void checkTextureFormat(std::uint32_t width, std::uint32_t height, bool normalMap, bool srgb, const std::string& format,
                               const std::string& where, Violations& v) {
    if (!isPowerOfTwo(width) || !isPowerOfTwo(height)) {
        v.push_back(where + " is " + std::to_string(width) + "x" + std::to_string(height) + ", not a power of two");
    }
    checkNormalMapEncoding(normalMap, srgb, format, where, v);
}

// ---- golden (asset_golden) -----------------------------------------------------------------------------------

/// §5.4: mean LDR-FLIP of a render against its golden must not exceed this.
inline constexpr double kGoldenMaxMeanFlip = 0.05;
/// Fallback metric when NVIDIA FLIP is not vendored: mean absolute linear-RGB error (0..1) must not exceed this.
inline constexpr double kGoldenMaxMeanAbsError = 0.02;

inline void checkGolden(bool sameSize, double meanError, bool flip, const std::string& where, Violations& v) {
    if (!sameSize) {
        v.push_back(where + ": render and golden differ in size");
        return;
    }
    const double limit = flip ? kGoldenMaxMeanFlip : kGoldenMaxMeanAbsError;
    if (!(meanError <= limit)) {
        v.push_back(where + ": mean " + (flip ? "FLIP " : "abs error ") + fmtNumber(meanError, 4) + " over the golden threshold " +
                    fmtNumber(limit, 3));
    }
}

} // namespace fuse::assetcheck
