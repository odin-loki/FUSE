#pragma once

// WP-9.1 neural runtime: constants, enums and the records shared by the C++ host code (neural_mlp.hpp, the CPU
// reference) and the GPU kernels (shaders/neural/nn_common.{glsl,slang} declare the same fields in the same
// order; fuse_rp_neural_layout checks names, order, offsets and sizes).
//
// Layout rules: 4-byte scalars, fixed arrays and 8-byte addresses only, no implicit padding, so the records are
// std430 == scalar on the GPU.
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::neural {

inline constexpr u32 kNnMaxRawInputs = 4u;  ///< raw input dimensions (hash grid: 2 or 3)
inline constexpr u32 kNnMaxWidth = 64u;     ///< widest layer (encoded input, hidden and output)
inline constexpr u32 kNnMaxLayers = 8u;     ///< linear layers (hidden layers + the output layer)
inline constexpr u32 kNnMaxLevels = 16u;    ///< hash-grid levels
inline constexpr u32 kNnMaxFrequencies = 10u;
inline constexpr u32 kNnWorkgroup = 64u;    ///< portable kernel: one sample per invocation
inline constexpr u32 kNnCoopTile = 16u;     ///< cooperative-matrix kernel: 16 x 16 x 16 fp16 tiles, 16 samples per group
/// float(pi): the frequency encoding's band l multiplies x by kNnPi * 2^l (exact power-of-two scaling).
inline constexpr f32 kNnPi = 3.14159265358979f;
/// Instant-NGP spatial hash primes (the first is 1: x is not scrambled).
inline constexpr u32 kNnPrime1 = 2654435761u;
inline constexpr u32 kNnPrime2 = 805459861u;

enum class NeuralEncoding : u32 {
    Identity = 0,  ///< the raw inputs
    Frequency = 1, ///< per input x: x, sin(pi 2^l x), cos(pi 2^l x) for l < frequencies (NeRF positional encoding)
    HashGrid = 2,  ///< multiresolution hash grid (Instant-NGP): levels x features, bi/trilinear, inputs in [0, 1]
};

enum class NeuralActivation : u32 {
    None = 0,
    ReLU = 1,
    LeakyReLU = 2, ///< x > 0 ? x : x * leakySlope
    Sigmoid = 3,   ///< 1 / (1 + exp(-x))
};

enum class NeuralPrecision : u32 {
    F32 = 0, ///< parameters stored as IEEE binary32
    F16 = 1, ///< parameters stored as IEEE binary16 (arithmetic stays fp32; the CPU widens them exactly)
};

/// The network description the GPU reads (the first 256 bytes of a NeuralGpu slot). Parameter offsets are
/// element indices into the parameter array (fp32 words, or fp16 halves packed two per 32-bit word, low first).
/// Linear layer j maps layerIn[j] -> layerOut[j]: out[o] = bias[o] + sum_i W[o * layerIn + i] * in[i], summed
/// in index order starting from the bias (the CPU reference, the naive oracle and both kernels).
struct NeuralGpuHeader {
    u32 inputDims;
    u32 encoding;       ///< NeuralEncoding
    u32 encodedDims;
    u32 layerCount;
    u32 outputDims;
    u32 paramPrecision; ///< NeuralPrecision ("precision" is a GLSL keyword)
    u32 frequencies;
    u32 levels;
    u32 features;       ///< hash grid: features per level
    u32 tableMask;      ///< hash grid: entries per level - 1 (power of two)
    u32 hiddenActivation;
    u32 outputActivation;
    f32 leakySlope;
    u32 gridOffset;     ///< hash grid: first table element
    u32 denseMask;      ///< hash grid: bit l set when level l is indexed densely ((res + 1)^dims <= entries)
    u32 reserved;
    u32 layerIn[kNnMaxLayers];
    u32 layerOut[kNnMaxLayers];
    u32 weightOffset[kNnMaxLayers];
    u32 biasOffset[kNnMaxLayers];
    u32 levelResolution[kNnMaxLevels];
};
static_assert(sizeof(NeuralGpuHeader) == 256u);

/// Push constants of the inference kernels. inputs: count x inputDims floats; outputs: count x outputDims floats.
struct NeuralPush {
    u64 header;  ///< BDA of NeuralGpuHeader
    u64 params;  ///< BDA of the parameter array
    u64 inputs;
    u64 outputs;
    u32 count;
    u32 reserved;
};
static_assert(sizeof(NeuralPush) == 40u);
static_assert(offsetof(NeuralPush, count) == 32u);

} // namespace fuse::renderer::neural
