// WP-9.1 neural runtime: records, parameter fetch and input encodings. Slang twin of nn_common.glsl (see there);
// the kernels are compiled with -fp-mode precise (no multiply-add contraction). The C++ mirror of the records is
// include/fuse/renderer/neural/neural_types.hpp (fuse_rp_neural_layout checks names, order and offsets).

static const uint kNnMaxWidth = 64;
static const uint kNnEncIdentity = 0;
static const uint kNnEncFrequency = 1;
static const uint kNnEncHashGrid = 2;
static const uint kNnActRelu = 1;
static const uint kNnActLeaky = 2;
static const uint kNnActSigmoid = 3;
static const uint kNnPrecF16 = 1;
static const float kNnPi = 3.14159265358979;
static const uint kNnPrime1 = 2654435761u;
static const uint kNnPrime2 = 805459861u;

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

// binary16 bits -> fp32, exact, with integer ops only (f16tof32 would declare the Float16 / Int16 capabilities,
// which the device does not enable; the GLSL twin's unpackHalf2x16 needs neither).
float nnHalfToFloat(uint h) {
    const uint sign = (h & 0x8000u) << 16;
    const uint e = (h >> 10) & 0x1Fu;
    const uint m = h & 0x3FFu;
    if (e == 0u) {
        const float v = float(m) * (1.0 / 16777216.0);
        return (sign != 0u) ? -v : v;
    }
    if (e == 31u) {
        return asfloat(sign | 0x7F800000u | (m << 13));
    }
    return asfloat(sign | ((e + 112u) << 23) | (m << 13));
}

// Parameter i (element index) widened to fp32.
float nnParam(uint64_t params, uint prec, uint i) {
    if (prec == kNnPrecF16) {
        const uint word = ((uint*)params)[i >> 1];
        return nnHalfToFloat((i & 1u) != 0u ? (word >> 16) : (word & 0xFFFFu));
    }
    return ((float*)params)[i];
}

float nnActivation(uint act, float v, float slope) {
    if (act == kNnActRelu) {
        return v > 0.0 ? v : 0.0;
    }
    if (act == kNnActLeaky) {
        return v > 0.0 ? v : v * slope;
    }
    if (act == kNnActSigmoid) {
        return 1.0 / (1.0 + exp(-v));
    }
    return v;
}

// Encodes x (inputDims values) into e[0, encodedDims).
void nnEncode(NeuralGpuHeader h, uint64_t params, float x[4], inout float e[kNnMaxWidth]) {
    if (h.encoding == kNnEncIdentity) {
        for (uint d = 0; d < h.inputDims; ++d) {
            e[d] = x[d];
        }
    } else if (h.encoding == kNnEncFrequency) {
        uint k = 0;
        for (uint d = 0; d < h.inputDims; ++d) {
            e[k++] = x[d];
            for (uint l = 0; l < h.frequencies; ++l) {
                const float s = x[d] * (kNnPi * float(1u << l));
                e[k++] = sin(s);
                e[k++] = cos(s);
            }
        }
    } else {
        const uint dims = h.inputDims;
        const uint corners = 1u << dims;
        for (uint l = 0; l < h.levels; ++l) {
            const uint res = h.levelResolution[l];
            const float fres = float(res);
            uint cell[3] = { 0, 0, 0 };
            float frac[3] = { 0.0, 0.0, 0.0 };
            for (uint d = 0; d < dims; ++d) {
                const float v = min(max(x[d], 0.0), 1.0);
                const float pos = v * fres;
                const uint c = min(uint(floor(pos)), res - 1u);
                cell[d] = c;
                frac[d] = pos - float(c);
            }
            const bool dense = ((h.denseMask >> l) & 1u) != 0u;
            const uint stride = res + 1u;
            const uint levelBase = h.gridOffset + l * (h.tableMask + 1u) * h.features;
            float acc[4] = { 0.0, 0.0, 0.0, 0.0 };
            for (uint k = 0; k < corners; ++k) {
                float w = 1.0;
                uint coord[3] = { 0, 0, 0 };
                for (uint d = 0; d < dims; ++d) {
                    const uint bit = (k >> d) & 1u;
                    coord[d] = cell[d] + bit;
                    w = w * (bit != 0u ? frac[d] : 1.0 - frac[d]);
                }
                uint idx;
                if (dense) {
                    idx = coord[0] + coord[1] * stride + (dims > 2u ? coord[2] * stride * stride : 0u);
                } else {
                    idx = coord[0] ^ (coord[1] * kNnPrime1) ^ (dims > 2u ? coord[2] * kNnPrime2 : 0u);
                }
                idx &= h.tableMask;
                const uint base = levelBase + idx * h.features;
                for (uint f = 0; f < h.features; ++f) {
                    acc[f] = acc[f] + w * nnParam(params, h.paramPrecision, base + f);
                }
            }
            for (uint f = 0; f < h.features; ++f) {
                e[l * h.features + f] = acc[f];
            }
        }
    }
}
