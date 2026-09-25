#pragma once

// WP-9.1 test helpers shared by the CPU gates (test_rp_neural_cpu.cpp) and the Lavapipe gates (test_rp_neural.cpp):
// a deterministic RNG, the configuration matrix and the 2D "image" the fit tests learn.

#include <fuse/renderer/neural/neural_mlp.hpp>

#include <cmath>
#include <vector>

namespace nn_test {

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using namespace fuse::renderer::neural;

struct Rng {
    u64 state;
    explicit Rng(u64 seed) : state(seed * 0x9E3779B97F4A7C15ull + 1u) {}
    u64 next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }
    f32 uniform() { return static_cast<f32>(next() >> 40) * (1.0f / 16777216.0f); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * uniform(); }
};

struct NamedConfig {
    const char* name;
    NeuralNetConfig config;
};

/// The configuration matrix: every encoding, activation and precision, 2D / 3D hash grids (dense + hashed levels).
inline std::vector<NamedConfig> configMatrix() {
    std::vector<NamedConfig> out;
    auto add = [&](const char* name, NeuralNetConfig c) { out.push_back({name, c}); };
    NeuralNetConfig c{};
    c.inputDims = 3;
    c.encoding = NeuralEncoding::Identity;
    c.hiddenWidth = 16;
    c.hiddenLayers = 2;
    c.outputDims = 3;
    c.hiddenActivation = NeuralActivation::ReLU;
    c.outputActivation = NeuralActivation::None;
    add("identity relu fp32", c);
    c.hiddenActivation = NeuralActivation::LeakyReLU;
    c.outputActivation = NeuralActivation::Sigmoid;
    c.precision = NeuralPrecision::F16;
    add("identity leaky/sigmoid fp16", c);

    c = NeuralNetConfig{};
    c.inputDims = 2;
    c.encoding = NeuralEncoding::Frequency;
    c.frequencies = 6;
    c.hiddenWidth = 32;
    c.hiddenLayers = 3;
    c.outputDims = 3;
    c.hiddenActivation = NeuralActivation::ReLU;
    c.outputActivation = NeuralActivation::Sigmoid;
    add("frequency relu/sigmoid fp32", c);
    c.inputDims = 4;
    c.frequencies = 4;
    c.hiddenActivation = NeuralActivation::Sigmoid;
    c.outputActivation = NeuralActivation::LeakyReLU;
    c.precision = NeuralPrecision::F16;
    add("frequency 4D sigmoid/leaky fp16", c);

    c = NeuralNetConfig{};
    c.inputDims = 2;
    c.encoding = NeuralEncoding::HashGrid;
    c.levels = 8;
    c.featuresPerLevel = 2;
    c.log2TableSize = 10;
    c.baseResolution = 4;
    c.perLevelScale = 1.6f;
    c.hiddenWidth = 32;
    c.hiddenLayers = 2;
    c.outputDims = 3;
    c.hiddenActivation = NeuralActivation::ReLU;
    c.outputActivation = NeuralActivation::None;
    add("hashgrid 2D relu fp32", c);
    c.precision = NeuralPrecision::F16;
    c.hiddenActivation = NeuralActivation::LeakyReLU;
    c.outputActivation = NeuralActivation::Sigmoid;
    add("hashgrid 2D leaky/sigmoid fp16", c);

    c = NeuralNetConfig{};
    c.inputDims = 3;
    c.encoding = NeuralEncoding::HashGrid;
    c.levels = 16;
    c.featuresPerLevel = 2;
    c.log2TableSize = 14;
    c.baseResolution = 8;
    c.perLevelScale = 1.4f;
    c.hiddenWidth = 64;
    c.hiddenLayers = 2;
    c.outputDims = 4;
    c.hiddenActivation = NeuralActivation::ReLU;
    c.outputActivation = NeuralActivation::None;
    add("hashgrid 3D (radiance cache) fp32", c);
    c.featuresPerLevel = 4;
    c.levels = 12;
    c.log2TableSize = 12;
    c.hiddenWidth = 48;
    c.hiddenLayers = 0;
    c.outputDims = 1;
    c.precision = NeuralPrecision::F16;
    c.outputActivation = NeuralActivation::Sigmoid;
    add("hashgrid 3D F4 linear fp16", c);
    return out;
}

/// Random inputs in [lo, hi) (hash grids see [-0.05, 1.05): clamping covered).
inline void randomInputs(Rng& rng, const NeuralNetConfig& c, u32 count, std::vector<f32>& out) {
    out.resize(static_cast<size_t>(count) * c.inputDims);
    const bool grid = c.encoding == NeuralEncoding::HashGrid;
    for (f32& v : out) {
        v = grid ? rng.range(-0.05f, 1.05f) : rng.range(-1.0f, 1.0f);
    }
}

/// Params perturbed off the initialisation (non-zero biases, larger tables), deterministic.
inline void perturb(NeuralNet& net, u64 seed) {
    Rng rng(seed);
    std::vector<f32> p = net.params();
    for (f32& v : p) {
        v += rng.range(-0.25f, 0.25f);
    }
    net.setParams(p.data(), static_cast<u32>(p.size()));
}

/// The 2D target the fit tests learn: smooth colour waves plus a sharp-edged disc (x, y in [0, 1]).
inline void targetImage(f32 x, f32 y, f32* rgb) {
    const f32 dx = x - 0.62f;
    const f32 dy = y - 0.38f;
    const f32 disc = (dx * dx + dy * dy) < 0.04f ? 1.0f : 0.0f;
    rgb[0] = 0.5f + 0.4f * std::sin(7.0f * x) * std::cos(5.0f * y);
    rgb[1] = 0.5f + 0.35f * std::sin(9.0f * (x + y));
    rgb[2] = 0.15f + 0.7f * disc;
}

inline NeuralNetConfig imageFitConfig(NeuralPrecision precision) {
    NeuralNetConfig c{};
    c.inputDims = 2;
    c.encoding = NeuralEncoding::HashGrid;
    c.levels = 8;
    c.featuresPerLevel = 2;
    c.log2TableSize = 12;
    c.baseResolution = 4;
    c.perLevelScale = 1.6f;
    c.hiddenWidth = 32;
    c.hiddenLayers = 2;
    c.outputDims = 3;
    c.hiddenActivation = NeuralActivation::ReLU;
    c.outputActivation = NeuralActivation::None;
    c.precision = precision;
    return c;
}

} // namespace nn_test
