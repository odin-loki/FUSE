#pragma once

// WP-9.1 neural runtime, CPU reference: a small fixed-function MLP (input encoding -> linear layers with
// ReLU / LeakyReLU / sigmoid) for neural radiance caching and neural materials, a deterministic trainer
// (MSE loss, backprop into the weights and the hash-grid tables, Adam), and the weight file format.
//
//   NeuralNetConfig c;  c.encoding = NeuralEncoding::HashGrid; ...
//   NeuralNet net;  net.init(c, seed);                   // deterministic initialisation
//   NeuralScratch scratch;  scratch.reserve(net);        // once: inference makes no heap allocation afterwards
//   net.infer(x, y, scratch);                            // one sample
//   NeuralTrainer trainer;  trainer.init(net, adam, maxBatch);
//   f32 loss = trainer.step(net, inputs, targets, count); // one Adam step, no heap allocation
//   std::vector<u8> file;  writeNeuralWeights(net, file);  readNeuralWeights(file.data(), file.size(), net);
//
// Arithmetic: every sum runs in index order (bias first, then inputs 0..n-1), and the library is compiled
// without multiply-add contraction, so inference is reproducible bit for bit (the naive oracle in
// test_rp_neural_cpu.cpp) and the GPU kernels differ only by their sin / cos / exp.
//
// fp16 networks: the parameters are binary16 values (NeuralNet::params() holds them widened to fp32, exactly);
// the trainer keeps an fp32 master copy and re-rounds it after every step (round to nearest even).

#include <fuse/renderer/neural/neural_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::neural {

struct NeuralNetConfig {
    u32 inputDims = 2;
    NeuralEncoding encoding = NeuralEncoding::HashGrid;
    u32 frequencies = 6;            ///< Frequency encoding bands
    u32 levels = 8;                 ///< HashGrid
    u32 featuresPerLevel = 2;       ///< HashGrid: 1, 2 or 4
    u32 log2TableSize = 12;         ///< HashGrid: entries per level = 2^log2TableSize
    u32 baseResolution = 4;         ///< HashGrid: level 0 cells per axis
    f32 perLevelScale = 1.5f;       ///< HashGrid: resolution growth per level
    u32 hiddenWidth = 32;
    u32 hiddenLayers = 2;           ///< 0: one linear layer (encoded -> output)
    u32 outputDims = 3;
    NeuralActivation hiddenActivation = NeuralActivation::ReLU;
    NeuralActivation outputActivation = NeuralActivation::None;
    f32 leakySlope = 0.01f;
    NeuralPrecision precision = NeuralPrecision::F32;
};

/// Encoded width of a configuration (0 when the encoding is invalid).
u32 neuralEncodedDims(const NeuralNetConfig& c);
/// Null when `c` is usable, else the first violated limit.
const char* validateNeuralConfig(const NeuralNetConfig& c);

// --- binary16 -------------------------------------------------------------------------------------------
/// fp32 -> fp16 bits, round to nearest even (overflow -> inf, NaN kept quiet).
u16 f32ToF16(f32 v);
/// fp16 bits -> fp32 (exact).
f32 f16ToF32(u16 h);
inline f32 roundToF16(f32 v) { return f16ToF32(f32ToF16(v)); }

class NeuralNet;

/// Per-thread inference / training workspace (fixed size: no allocation per call).
struct NeuralScratch {
    f32 a[kNnMaxWidth] = {};
    f32 b[kNnMaxWidth] = {};
};

class NeuralNet {
public:
    /// Validates `config`, builds the header and initialises the parameters deterministically from `seed`
    /// (weights uniform in +-sqrt(6 / (in + out)), biases 0, hash tables uniform in +-1e-4). False when invalid.
    bool init(const NeuralNetConfig& config, u64 seed);
    bool valid() const { return m_valid; }

    const NeuralNetConfig& config() const { return m_config; }
    const NeuralGpuHeader& header() const { return m_header; }
    u32 encodedDims() const { return m_header.encodedDims; }
    u32 paramCount() const { return static_cast<u32>(m_params.size()); }
    /// The effective parameters (fp16 networks: binary16 values widened to fp32).
    const std::vector<f32>& params() const { return m_params; }
    /// Replaces the parameters (count must match; fp16 networks round them). Bumps version().
    bool setParams(const f32* values, u32 count);
    /// A new process-wide unique stamp on every parameter change (GPU upload tracking).
    u64 version() const { return m_version; }

    /// Encodes one input (x: inputDims floats) into out[encodedDims()].
    void encode(const f32* x, f32* out) const;
    /// One sample: x[inputDims] -> y[outputDims]. No heap allocation.
    void infer(const f32* x, f32* y, NeuralScratch& scratch) const;
    /// count samples, tightly packed.
    void inferBatch(const f32* x, f32* y, u32 count, NeuralScratch& scratch) const;

    /// Hash-grid corner lookup shared with the trainer: for level l, the 2^dims table entries (element index of
    /// feature 0) and their interpolation weights. Returns the corner count.
    u32 gridCorners(const f32* x, u32 level, u32* index, f32* weight) const;

    /// GPU blob: NeuralGpuHeader followed by the parameters in the network's precision (fp16: packed halves,
    /// padded to 4 bytes). The bytes a NeuralGpu slot holds.
    u64 gpuBlobBytes() const;
    void writeGpuBlob(void* dst) const;

private:
    friend class NeuralTrainer;
    friend bool readNeuralWeights(const u8* data, usize size, NeuralNet& net);
    bool build(const NeuralNetConfig& config);
    void quantize();

    NeuralNetConfig m_config{};
    NeuralGpuHeader m_header{};
    std::vector<f32> m_params;
    u64 m_version = 0;
    bool m_valid = false;
};

f32 applyNeuralActivation(NeuralActivation act, f32 v, f32 leakySlope);

// --- training ---------------------------------------------------------------------------------------------
struct NeuralAdamDesc {
    f32 learningRate = 1e-2f;
    f32 beta1 = 0.9f;
    f32 beta2 = 0.99f;
    f32 epsilon = 1e-15f;
};

/// Deterministic trainer: loss = mean over samples and outputs of (y - target)^2. Gradients are accumulated in
/// sample order, so a run is reproducible bit for bit on one build.
class NeuralTrainer {
public:
    /// Allocates the per-sample activation cache for up to maxBatch samples, the gradient and Adam moments.
    bool init(const NeuralNet& net, const NeuralAdamDesc& adam, u32 maxBatch);
    /// Forward + backward over the batch, then one Adam step (writes the parameters, bumps net.version()).
    /// Returns the batch loss before the step; negative when count exceeds maxBatch or the net mismatches.
    /// No heap allocation.
    f32 step(NeuralNet& net, const f32* inputs, const f32* targets, u32 count);
    /// Forward + backward only: the mean loss and dLoss/dParam (fp32 master parameters) in gradient().
    f32 computeGradient(const NeuralNet& net, const f32* inputs, const f32* targets, u32 count);
    const std::vector<f32>& gradient() const { return m_grad; }
    u32 steps() const { return m_step; }

private:
    NeuralAdamDesc m_adam{};
    u32 m_maxBatch = 0;
    u32 m_step = 0;
    u32 m_actStride = 0;           ///< floats per sample in m_acts
    std::vector<f32> m_acts;       ///< per sample: encoded input, then every layer's post-activation output
    std::vector<f32> m_grad;
    std::vector<f32> m_m;
    std::vector<f32> m_v;
    std::vector<f32> m_master;     ///< fp32 master parameters
    f32 m_delta[kNnMaxWidth] = {};
    f32 m_deltaIn[kNnMaxWidth] = {};
};

// --- weight file ------------------------------------------------------------------------------------------
// "FNNW" little-endian file:
//   0   char[4] magic "FNNW"          4  u32 version (1)       8  u32 headerBytes (80)    12 u32 paramCount
//   16  u32 inputDims                 20 u32 encoding           24 u32 frequencies         28 u32 levels
//   32  u32 featuresPerLevel          36 u32 log2TableSize      40 u32 baseResolution      44 f32 perLevelScale
//   48  u32 hiddenWidth               52 u32 hiddenLayers       56 u32 outputDims          60 u32 hiddenActivation
//   64  u32 outputActivation          68 f32 leakySlope         72 u32 precision           76 u32 FNV-1a of the payload
//   80  payload: paramCount x (f32 | f16), the parameter layout of NeuralNet (hash tables, then per layer W, b)
inline constexpr u32 kNeuralFileVersion = 1u;
inline constexpr u32 kNeuralFileHeaderBytes = 80u;

void writeNeuralWeights(const NeuralNet& net, std::vector<u8>& out);
/// False (net untouched) on a bad magic / version / config / size / checksum.
bool readNeuralWeights(const u8* data, usize size, NeuralNet& net);

} // namespace fuse::renderer::neural
