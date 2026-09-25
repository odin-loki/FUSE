// WP-9.1 neural runtime: records, parameter fetch and input encodings. GLSL twin of nn_common.slang and a port of
// NeuralNet::encode / NeuralNet::gridCorners (src/neural/neural_mlp.cpp). The C++ mirror of the records is
// include/fuse/renderer/neural/neural_types.hpp (fuse_rp_neural_layout checks names, order and offsets).
//
// Every arithmetic result is held in a `precise` variable so no multiply-add is contracted: the same IEEE
// operations in the same order as the CPU reference (only sin / cos / exp are the GPU's own).
#ifndef FUSE_NN_COMMON_GLSL
#define FUSE_NN_COMMON_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_scalar_block_layout : require

#define NN_MAX_WIDTH 64
#define NN_ENC_IDENTITY 0u
#define NN_ENC_FREQUENCY 1u
#define NN_ENC_HASHGRID 2u
#define NN_ACT_NONE 0u
#define NN_ACT_RELU 1u
#define NN_ACT_LEAKY 2u
#define NN_ACT_SIGMOID 3u
#define NN_PREC_F16 1u
#define NN_PI 3.14159265358979
#define NN_PRIME1 2654435761u
#define NN_PRIME2 805459861u

// NeuralGpuHeader, 256 bytes.
struct NeuralGpuHeader {
    uint inputDims;
    uint encoding;
    uint encodedDims;
    uint layerCount;
    uint outputDims;
    uint paramPrecision;
    uint frequencies;
    uint levels;
    uint features;
    uint tableMask;
    uint hiddenActivation;
    uint outputActivation;
    float leakySlope;
    uint gridOffset;
    uint denseMask;
    uint reserved;
    uint layerIn[8];
    uint layerOut[8];
    uint weightOffset[8];
    uint biasOffset[8];
    uint levelResolution[16];
};

// NeuralPush, 40 bytes.
struct NeuralPush {
    uint64_t header;
    uint64_t params;
    uint64_t inputs;
    uint64_t outputs;
    uint count;
    uint reserved;
};

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer NnHeaderRef { NeuralGpuHeader h; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer NnWordsRef { uint v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer NnFloatsRef { float v[]; };

// Parameter i (element index) widened to fp32.
float nn_param(uint64_t params, uint prec, uint i) {
    if (prec == NN_PREC_F16) {
        const vec2 pair = unpackHalf2x16(NnWordsRef(params).v[i >> 1]);
        return (i & 1u) != 0u ? pair.y : pair.x;
    }
    return NnFloatsRef(params).v[i];
}

float nn_activation(uint act, float v, float slope) {
    if (act == NN_ACT_RELU) {
        return v > 0.0 ? v : 0.0;
    }
    if (act == NN_ACT_LEAKY) {
        precise float s = v * slope;
        return v > 0.0 ? v : s;
    }
    if (act == NN_ACT_SIGMOID) {
        precise float d = 1.0 + exp(-v);
        precise float r = 1.0 / d;
        return r;
    }
    return v;
}

// Encodes x (inputDims values) into e[0, encodedDims).
void nn_encode(NeuralGpuHeader h, uint64_t params, float x[4], inout float e[NN_MAX_WIDTH]) {
    if (h.encoding == NN_ENC_IDENTITY) {
        for (uint d = 0u; d < h.inputDims; ++d) {
            e[d] = x[d];
        }
    } else if (h.encoding == NN_ENC_FREQUENCY) {
        uint k = 0u;
        for (uint d = 0u; d < h.inputDims; ++d) {
            e[k++] = x[d];
            for (uint l = 0u; l < h.frequencies; ++l) {
                precise float band = NN_PI * float(1u << l);
                precise float s = x[d] * band;
                e[k++] = sin(s);
                e[k++] = cos(s);
            }
        }
    } else {
        const uint dims = h.inputDims;
        const uint corners = 1u << dims;
        for (uint l = 0u; l < h.levels; ++l) {
            const uint res = h.levelResolution[l];
            const float fres = float(res);
            uint cell[3] = uint[3](0u, 0u, 0u);
            float frac[3] = float[3](0.0, 0.0, 0.0);
            for (uint d = 0u; d < dims; ++d) {
                const float v = min(max(x[d], 0.0), 1.0);
                precise float pos = v * fres;
                uint c = uint(floor(pos));
                c = min(c, res - 1u);
                cell[d] = c;
                precise float f = pos - float(c);
                frac[d] = f;
            }
            const bool dense = ((h.denseMask >> l) & 1u) != 0u;
            const uint stride = res + 1u;
            const uint levelBase = h.gridOffset + l * (h.tableMask + 1u) * h.features;
            float acc[4] = float[4](0.0, 0.0, 0.0, 0.0);
            for (uint k = 0u; k < corners; ++k) {
                precise float w = 1.0;
                uint coord[3] = uint[3](0u, 0u, 0u);
                for (uint d = 0u; d < dims; ++d) {
                    const uint bit = (k >> d) & 1u;
                    coord[d] = cell[d] + bit;
                    precise float omf = 1.0 - frac[d];
                    w = w * (bit != 0u ? frac[d] : omf);
                }
                uint idx;
                if (dense) {
                    idx = coord[0] + coord[1] * stride + (dims > 2u ? coord[2] * stride * stride : 0u);
                } else {
                    idx = coord[0] ^ (coord[1] * NN_PRIME1) ^ (dims > 2u ? coord[2] * NN_PRIME2 : 0u);
                }
                idx &= h.tableMask;
                const uint base = levelBase + idx * h.features;
                for (uint f = 0u; f < h.features; ++f) {
                    precise float t = w * nn_param(params, h.paramPrecision, base + f);
                    precise float s = acc[f] + t;
                    acc[f] = s;
                }
            }
            for (uint f = 0u; f < h.features; ++f) {
                e[l * h.features + f] = acc[f];
            }
        }
    }
}

#endif
