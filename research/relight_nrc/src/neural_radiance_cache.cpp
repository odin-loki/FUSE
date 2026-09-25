// FUSE Relight RL-5.4 research track: the online-trained neural radiance cache (see neural_radiance_cache.hpp).
#include <relight_nrc/neural_radiance_cache.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::relight::research {

namespace nn = fuse::renderer::neural;
namespace pt = fuse::relight::render::pathtrace;

NrcSettings NrcSettings::defaults() {
    NrcSettings s;
    s.net.inputDims = 3;
    s.net.encoding = nn::NeuralEncoding::HashGrid;
    s.net.levels = 12;
    s.net.featuresPerLevel = 2;
    s.net.log2TableSize = 14;
    s.net.baseResolution = 4;
    s.net.perLevelScale = 1.5f;
    s.net.hiddenWidth = 32;
    s.net.hiddenLayers = 2;
    s.net.outputDims = 3;
    s.net.hiddenActivation = nn::NeuralActivation::ReLU;
    s.net.outputActivation = nn::NeuralActivation::None;
    s.net.precision = nn::NeuralPrecision::F32;
    s.adam.learningRate = 1e-2f;
    return s;
}

bool NeuralRadianceCache::init(const NrcSettings& settings) {
    m_settings = settings;
    if (m_settings.net.inputDims != 3u || m_settings.net.outputDims != 3u || m_settings.maxBatch == 0u) {
        return false;
    }
    if (!m_net.init(m_settings.net, m_settings.seed) ||
        !m_trainer.init(m_net, m_settings.adam, m_settings.maxBatch)) {
        return false;
    }
    for (int a = 0; a < 3; ++a) {
        const float extent = m_settings.boundsMax[a] - m_settings.boundsMin[a];
        m_scale[a] = extent > 0.f ? 1.f / extent : 1.f;
    }
    m_stats = NrcStats{};
    return true;
}

void NeuralRadianceCache::encodeInput(const float p[3], const float n[3], float* x) const {
    for (int a = 0; a < 3; ++a) {
        const float q = p[a] + n[a] * m_settings.normalOffset;
        x[a] = std::clamp((q - m_settings.boundsMin[a]) * m_scale[a], 0.f, 1.f);
    }
}

bool NeuralRadianceCache::trainFrame(const pt::Word* records, u32 recordCount) {
    if (!valid() || records == nullptr) {
        return false;
    }
    const std::size_t need = std::size_t(recordCount) * 3u;
    if (m_inputs.size() < need) { // grows with the record count only (steady state: no allocation)
        m_inputs.resize(need);
        m_targets.resize(need);
    }
    u32 count = 0;
    for (u32 i = 0; i < recordCount; ++i) {
        const pt::Word& w0 = records[i * pt::kRcRecordWords];
        if (!(w0.w > 0.5f)) {
            continue;
        }
        const pt::Word& w1 = records[i * pt::kRcRecordWords + 1u];
        const pt::Word& w2 = records[i * pt::kRcRecordWords + 2u];
        const float p[3] = {w0.x, w0.y, w0.z};
        const float n[3] = {w1.x, w1.y, w1.z};
        encodeInput(p, n, &m_inputs[std::size_t(count) * 3u]);
        const float t[3] = {w2.x, w2.y, w2.z};
        for (int c = 0; c < 3; ++c) {
            const float v = t[c];
            m_targets[std::size_t(count) * 3u + c] = v > 0.f ? std::min(v, m_settings.maxRadiance) : 0.f; // NaN -> 0
        }
        ++count;
    }
    // Deterministic shuffle (the frame's LCG), so consecutive windows are random batches.
    u32 state = 0x9E3779B9u ^ (m_stats.frames * 747796405u + 1u);
    for (u32 i = count; i > 1u; --i) {
        state = state * 1664525u + 1013904223u;
        const u32 j = (state >> 8) % i;
        for (int c = 0; c < 3; ++c) {
            std::swap(m_inputs[std::size_t(i - 1u) * 3u + c], m_inputs[std::size_t(j) * 3u + c]);
            std::swap(m_targets[std::size_t(i - 1u) * 3u + c], m_targets[std::size_t(j) * 3u + c]);
        }
    }
    float lossSum = 0.f;
    u32 steps = 0;
    if (count > 0u) {
        const u32 batch = std::min(count, m_settings.maxBatch);
        for (u32 s = 0; s < m_settings.stepsPerFrame; ++s) {
            const u32 first = (s * batch) % count;
            const u32 n = std::min(batch, count - first);
            const float loss = m_trainer.step(m_net, &m_inputs[std::size_t(first) * 3u],
                                              &m_targets[std::size_t(first) * 3u], n);
            if (loss < 0.f) {
                return false;
            }
            lossSum += loss;
            ++steps;
        }
    }
    ++m_stats.frames;
    m_stats.steps += steps;
    m_stats.samples = count;
    m_stats.loss = steps != 0u ? lossSum / float(steps) : 0.f;
    return true;
}

void NeuralRadianceCache::query(const float p[3], const float n[3], float out[3]) const {
    float x[3];
    encodeInput(p, n, x);
    nn::NeuralScratch scratch;
    float y[3] = {0.f, 0.f, 0.f};
    m_net.infer(x, y, scratch);
    for (int c = 0; c < 3; ++c) {
        out[c] = y[c] > 0.f ? y[c] : 0.f;
    }
}

namespace {
bool nrcLookup(void* user, const float p[3], const float n[3], float out[3]) {
    static_cast<const NeuralRadianceCache*>(user)->query(p, n, out);
    return true;
}
} // namespace

pt::RadianceCacheLookup NeuralRadianceCache::lookup() const {
    pt::RadianceCacheLookup l;
    l.fn = &nrcLookup;
    l.user = const_cast<NeuralRadianceCache*>(this);
    return l;
}

} // namespace fuse::relight::research
