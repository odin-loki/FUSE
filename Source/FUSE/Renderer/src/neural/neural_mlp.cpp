// WP-9.1 neural runtime, CPU reference: see include/fuse/renderer/neural/neural_mlp.hpp.
#include <fuse/renderer/neural/neural_mlp.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

namespace fuse::renderer::neural {

static_assert(std::endian::native == std::endian::little, "the weight file and GPU blob are little-endian");

namespace {

u64 splitmix64(u64& state) {
    u64 z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// Uniform in [-1, 1).
f32 uniformSigned(u64& state) {
    const f32 u = static_cast<f32>(splitmix64(state) >> 40) * (1.0f / 16777216.0f);
    return u * 2.0f - 1.0f;
}

bool validActivation(NeuralActivation a) { return static_cast<u32>(a) <= static_cast<u32>(NeuralActivation::Sigmoid); }

/// d act / d pre from the post-activation value.
f32 activationDerivative(NeuralActivation act, f32 y, f32 leakySlope) {
    switch (act) {
    case NeuralActivation::ReLU:
        return y > 0.0f ? 1.0f : 0.0f;
    case NeuralActivation::LeakyReLU:
        return y > 0.0f ? 1.0f : leakySlope;
    case NeuralActivation::Sigmoid:
        return y * (1.0f - y);
    case NeuralActivation::None:
    default:
        return 1.0f;
    }
}

/// Process-wide version stamps: two nets (or one net re-initialised at the same address) never share a version,
/// so NeuralGpu's per-slot tracking cannot mistake one for the other.
u64 nextNeuralVersion() {
    static std::atomic<u64> counter{0};
    return counter.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

u32 fnv1a(const u8* data, usize size) {
    u32 h = 2166136261u;
    for (usize i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

void putU32(u8* dst, u32 v) { std::memcpy(dst, &v, 4); }
void putF32(u8* dst, f32 v) { std::memcpy(dst, &v, 4); }
u32 getU32(const u8* src) {
    u32 v = 0;
    std::memcpy(&v, src, 4);
    return v;
}
f32 getF32(const u8* src) {
    f32 v = 0;
    std::memcpy(&v, src, 4);
    return v;
}

} // namespace

// --- binary16 -------------------------------------------------------------------------------------------
u16 f32ToF16(f32 v) {
    const u32 x = std::bit_cast<u32>(v);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 exp = (x >> 23) & 0xFFu;
    u32 mant = x & 0x7FFFFFu;
    if (exp == 0xFFu) {
        return static_cast<u16>(sign | 0x7C00u | (mant != 0u ? 0x200u | (mant >> 13) : 0u));
    }
    const i32 e = static_cast<i32>(exp) - 127 + 15;
    if (e >= 31) {
        return static_cast<u16>(sign | 0x7C00u);
    }
    if (e <= 0) {
        if (e < -10) {
            return static_cast<u16>(sign); // below half the smallest subnormal
        }
        mant |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - e);
        u32 h = mant >> shift;
        const u32 rem = mant & ((1u << shift) - 1u);
        const u32 half = 1u << (shift - 1u);
        if (rem > half || (rem == half && (h & 1u) != 0u)) {
            ++h;
        }
        return static_cast<u16>(sign | h);
    }
    u32 h = (static_cast<u32>(e) << 10) | (mant >> 13);
    const u32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u) != 0u)) {
        ++h; // may carry into the exponent (and to inf): still correct
    }
    return static_cast<u16>(sign | h);
}

f32 f16ToF32(u16 h) {
    const u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1Fu;
    const u32 mant = h & 0x3FFu;
    if (exp == 0u) {
        // Subnormal (or zero): mant * 2^-24, exact in fp32.
        const f32 m = static_cast<f32>(mant) * (1.0f / 16777216.0f);
        return (sign != 0u) ? -m : m;
    }
    if (exp == 31u) {
        return std::bit_cast<f32>(sign | 0x7F800000u | (mant << 13));
    }
    return std::bit_cast<f32>(sign | ((exp + 112u) << 23) | (mant << 13));
}

// --- config -----------------------------------------------------------------------------------------------
u32 neuralEncodedDims(const NeuralNetConfig& c) {
    switch (c.encoding) {
    case NeuralEncoding::Identity:
        return c.inputDims;
    case NeuralEncoding::Frequency:
        return c.inputDims * (1u + 2u * c.frequencies);
    case NeuralEncoding::HashGrid:
        return c.levels * c.featuresPerLevel;
    default:
        return 0u;
    }
}

const char* validateNeuralConfig(const NeuralNetConfig& c) {
    if (c.inputDims == 0u || c.inputDims > kNnMaxRawInputs) {
        return "inputDims must be 1..4";
    }
    if (c.encoding == NeuralEncoding::Frequency && c.frequencies > kNnMaxFrequencies) {
        return "too many frequency bands";
    }
    if (c.encoding == NeuralEncoding::HashGrid) {
        if (c.inputDims < 2u || c.inputDims > 3u) {
            return "hash grid needs 2 or 3 inputs";
        }
        if (c.levels == 0u || c.levels > kNnMaxLevels) {
            return "hash grid levels must be 1..16";
        }
        if (c.featuresPerLevel != 1u && c.featuresPerLevel != 2u && c.featuresPerLevel != 4u) {
            return "hash grid features per level must be 1, 2 or 4";
        }
        if (c.log2TableSize < 4u || c.log2TableSize > 22u) {
            return "hash grid table size must be 2^4..2^22";
        }
        if (c.baseResolution == 0u || !(c.perLevelScale >= 1.0f) || !(c.perLevelScale <= 4.0f)) {
            return "hash grid resolution";
        }
    }
    if (static_cast<u32>(c.encoding) > static_cast<u32>(NeuralEncoding::HashGrid)) {
        return "unknown encoding";
    }
    const u32 enc = neuralEncodedDims(c);
    if (enc == 0u || enc > kNnMaxWidth) {
        return "encoded width must be 1..64";
    }
    if (c.hiddenLayers + 1u > kNnMaxLayers) {
        return "too many layers";
    }
    if (c.hiddenLayers > 0u && (c.hiddenWidth == 0u || c.hiddenWidth > kNnMaxWidth)) {
        return "hidden width must be 1..64";
    }
    if (c.outputDims == 0u || c.outputDims > kNnMaxWidth) {
        return "outputDims must be 1..64";
    }
    if (!validActivation(c.hiddenActivation) || !validActivation(c.outputActivation)) {
        return "unknown activation";
    }
    if (!(c.leakySlope >= 0.0f) || !(c.leakySlope < 1.0f)) {
        return "leakySlope must be in [0, 1)";
    }
    if (static_cast<u32>(c.precision) > static_cast<u32>(NeuralPrecision::F16)) {
        return "unknown precision";
    }
    return nullptr;
}

f32 applyNeuralActivation(NeuralActivation act, f32 v, f32 leakySlope) {
    switch (act) {
    case NeuralActivation::ReLU:
        return v > 0.0f ? v : 0.0f;
    case NeuralActivation::LeakyReLU:
        return v > 0.0f ? v : v * leakySlope;
    case NeuralActivation::Sigmoid:
        return 1.0f / (1.0f + std::exp(-v));
    case NeuralActivation::None:
    default:
        return v;
    }
}

// --- NeuralNet --------------------------------------------------------------------------------------------
bool NeuralNet::build(const NeuralNetConfig& c) {
    if (validateNeuralConfig(c) != nullptr) {
        return false;
    }
    NeuralGpuHeader h{};
    h.inputDims = c.inputDims;
    h.encoding = static_cast<u32>(c.encoding);
    h.encodedDims = neuralEncodedDims(c);
    h.layerCount = c.hiddenLayers + 1u;
    h.outputDims = c.outputDims;
    h.paramPrecision = static_cast<u32>(c.precision);
    h.frequencies = c.encoding == NeuralEncoding::Frequency ? c.frequencies : 0u;
    h.hiddenActivation = static_cast<u32>(c.hiddenActivation);
    h.outputActivation = static_cast<u32>(c.outputActivation);
    h.leakySlope = c.leakySlope;
    u64 cursor = 0;
    if (c.encoding == NeuralEncoding::HashGrid) {
        const u64 entries = u64{1} << c.log2TableSize;
        h.levels = c.levels;
        h.features = c.featuresPerLevel;
        h.tableMask = static_cast<u32>(entries - 1u);
        h.gridOffset = 0u;
        f64 scale = 1.0; // iterated products: the same resolutions on every platform
        for (u32 l = 0; l < c.levels; ++l) {
            const f64 res = std::floor(static_cast<f64>(c.baseResolution) * scale);
            h.levelResolution[l] = static_cast<u32>(std::min(res, 65535.0));
            scale *= static_cast<f64>(c.perLevelScale);
            u64 dense = 1;
            for (u32 d = 0; d < c.inputDims; ++d) {
                dense *= static_cast<u64>(h.levelResolution[l]) + 1u;
            }
            if (dense <= entries) {
                h.denseMask |= 1u << l;
            }
        }
        cursor = static_cast<u64>(c.levels) * entries * c.featuresPerLevel;
    }
    for (u32 j = 0; j < h.layerCount; ++j) {
        h.layerIn[j] = j == 0u ? h.encodedDims : c.hiddenWidth;
        h.layerOut[j] = j + 1u == h.layerCount ? c.outputDims : c.hiddenWidth;
        h.weightOffset[j] = static_cast<u32>(cursor);
        cursor += static_cast<u64>(h.layerIn[j]) * h.layerOut[j];
        h.biasOffset[j] = static_cast<u32>(cursor);
        cursor += h.layerOut[j];
    }
    if (cursor > 0x7FFFFFFFull) {
        return false;
    }
    m_config = c;
    m_header = h;
    m_params.assign(static_cast<usize>(cursor), 0.0f);
    m_valid = true;
    m_version = nextNeuralVersion();
    return true;
}

bool NeuralNet::init(const NeuralNetConfig& config, u64 seed) {
    if (!build(config)) {
        m_valid = false;
        return false;
    }
    u64 state = seed;
    const NeuralGpuHeader& h = m_header;
    if (config.encoding == NeuralEncoding::HashGrid) {
        const u32 gridCount = h.weightOffset[0];
        for (u32 i = 0; i < gridCount; ++i) {
            m_params[i] = uniformSigned(state) * 1e-4f;
        }
    }
    for (u32 j = 0; j < h.layerCount; ++j) {
        const f32 s = std::sqrt(6.0f / static_cast<f32>(h.layerIn[j] + h.layerOut[j]));
        const u32 n = h.layerIn[j] * h.layerOut[j];
        for (u32 i = 0; i < n; ++i) {
            m_params[h.weightOffset[j] + i] = uniformSigned(state) * s;
        }
    }
    quantize();
    return true;
}

void NeuralNet::quantize() {
    if (m_config.precision == NeuralPrecision::F16) {
        for (f32& p : m_params) {
            p = roundToF16(p);
        }
    }
}

bool NeuralNet::setParams(const f32* values, u32 count) {
    if (!m_valid || count != m_params.size()) {
        return false;
    }
    std::memcpy(m_params.data(), values, static_cast<usize>(count) * sizeof(f32));
    quantize();
    m_version = nextNeuralVersion();
    return true;
}

u32 NeuralNet::gridCorners(const f32* x, u32 level, u32* index, f32* weight) const {
    const NeuralGpuHeader& h = m_header;
    const u32 dims = h.inputDims;
    const u32 res = h.levelResolution[level];
    const f32 fres = static_cast<f32>(res);
    u32 cell[3] = {};
    f32 frac[3] = {};
    for (u32 d = 0; d < dims; ++d) {
        const f32 v = std::min(std::max(x[d], 0.0f), 1.0f);
        const f32 pos = v * fres;
        const f32 fl = std::floor(pos);
        u32 c = static_cast<u32>(fl);
        if (c > res - 1u) {
            c = res - 1u;
        }
        cell[d] = c;
        frac[d] = pos - static_cast<f32>(c);
    }
    const bool dense = ((h.denseMask >> level) & 1u) != 0u;
    const u32 stride = res + 1u;
    const u32 corners = 1u << dims;
    const u32 levelBase = h.gridOffset + level * (h.tableMask + 1u) * h.features;
    for (u32 k = 0; k < corners; ++k) {
        f32 w = 1.0f;
        u32 coord[3] = {};
        for (u32 d = 0; d < dims; ++d) {
            const u32 bit = (k >> d) & 1u;
            coord[d] = cell[d] + bit;
            w = w * (bit != 0u ? frac[d] : 1.0f - frac[d]);
        }
        u32 idx = 0;
        if (dense) {
            idx = coord[0] + coord[1] * stride + (dims > 2u ? coord[2] * stride * stride : 0u);
        } else {
            idx = coord[0] ^ (coord[1] * kNnPrime1) ^ (dims > 2u ? coord[2] * kNnPrime2 : 0u);
        }
        idx &= h.tableMask;
        index[k] = levelBase + idx * h.features;
        weight[k] = w;
    }
    return corners;
}

void NeuralNet::encode(const f32* x, f32* out) const {
    const NeuralGpuHeader& h = m_header;
    switch (static_cast<NeuralEncoding>(h.encoding)) {
    case NeuralEncoding::Identity:
        for (u32 d = 0; d < h.inputDims; ++d) {
            out[d] = x[d];
        }
        break;
    case NeuralEncoding::Frequency: {
        u32 k = 0;
        for (u32 d = 0; d < h.inputDims; ++d) {
            out[k++] = x[d];
            for (u32 l = 0; l < h.frequencies; ++l) {
                const f32 s = x[d] * (kNnPi * static_cast<f32>(1u << l));
                out[k++] = std::sin(s);
                out[k++] = std::cos(s);
            }
        }
        break;
    }
    case NeuralEncoding::HashGrid: {
        u32 index[8];
        f32 weight[8];
        for (u32 l = 0; l < h.levels; ++l) {
            const u32 corners = gridCorners(x, l, index, weight);
            for (u32 f = 0; f < h.features; ++f) {
                f32 acc = 0.0f;
                for (u32 k = 0; k < corners; ++k) {
                    acc = acc + weight[k] * m_params[index[k] + f];
                }
                out[l * h.features + f] = acc;
            }
        }
        break;
    }
    }
}

void NeuralNet::infer(const f32* x, f32* y, NeuralScratch& scratch) const {
    const NeuralGpuHeader& h = m_header;
    f32* a = scratch.a;
    f32* b = scratch.b;
    encode(x, a);
    const f32* p = m_params.data();
    for (u32 j = 0; j < h.layerCount; ++j) {
        const u32 in = h.layerIn[j];
        const u32 out = h.layerOut[j];
        const f32* w = p + h.weightOffset[j];
        const f32* bias = p + h.biasOffset[j];
        const NeuralActivation act = static_cast<NeuralActivation>(j + 1u == h.layerCount ? h.outputActivation
                                                                                         : h.hiddenActivation);
        for (u32 o = 0; o < out; ++o) {
            f32 acc = bias[o];
            const f32* row = w + static_cast<usize>(o) * in;
            for (u32 i = 0; i < in; ++i) {
                acc = acc + row[i] * a[i];
            }
            b[o] = applyNeuralActivation(act, acc, h.leakySlope);
        }
        f32* t = a;
        a = b;
        b = t;
    }
    for (u32 o = 0; o < h.outputDims; ++o) {
        y[o] = a[o];
    }
}

void NeuralNet::inferBatch(const f32* x, f32* y, u32 count, NeuralScratch& scratch) const {
    for (u32 s = 0; s < count; ++s) {
        infer(x + static_cast<usize>(s) * m_header.inputDims, y + static_cast<usize>(s) * m_header.outputDims, scratch);
    }
}

u64 NeuralNet::gpuBlobBytes() const {
    const u64 n = m_params.size();
    const u64 paramBytes = m_config.precision == NeuralPrecision::F16 ? ((n * 2u + 3u) & ~u64{3}) : n * 4u;
    return sizeof(NeuralGpuHeader) + paramBytes;
}

void NeuralNet::writeGpuBlob(void* dst) const {
    u8* out = static_cast<u8*>(dst);
    std::memcpy(out, &m_header, sizeof(NeuralGpuHeader));
    out += sizeof(NeuralGpuHeader);
    if (m_config.precision == NeuralPrecision::F16) {
        const usize n = m_params.size();
        for (usize i = 0; i < n; ++i) {
            const u16 hbits = f32ToF16(m_params[i]);
            std::memcpy(out + i * 2u, &hbits, 2);
        }
        if ((n & 1u) != 0u) {
            const u16 zero = 0;
            std::memcpy(out + n * 2u, &zero, 2);
        }
    } else {
        std::memcpy(out, m_params.data(), m_params.size() * sizeof(f32));
    }
}

// --- NeuralTrainer ----------------------------------------------------------------------------------------
bool NeuralTrainer::init(const NeuralNet& net, const NeuralAdamDesc& adam, u32 maxBatch) {
    if (!net.valid() || maxBatch == 0u) {
        return false;
    }
    const NeuralGpuHeader& h = net.header();
    m_adam = adam;
    m_maxBatch = maxBatch;
    m_step = 0;
    m_actStride = h.encodedDims;
    for (u32 j = 0; j < h.layerCount; ++j) {
        m_actStride += h.layerOut[j];
    }
    m_acts.assign(static_cast<usize>(m_actStride) * maxBatch, 0.0f);
    m_grad.assign(net.paramCount(), 0.0f);
    m_m.assign(net.paramCount(), 0.0f);
    m_v.assign(net.paramCount(), 0.0f);
    m_master = net.params();
    return true;
}

f32 NeuralTrainer::computeGradient(const NeuralNet& net, const f32* inputs, const f32* targets, u32 count) {
    const NeuralGpuHeader& h = net.header();
    if (count == 0u || count > m_maxBatch || m_grad.size() != net.paramCount()) {
        return -1.0f;
    }
    std::fill(m_grad.begin(), m_grad.end(), 0.0f);
    const f32* p = net.params().data();
    f32* g = m_grad.data();
    const f32 lossScale = 2.0f / static_cast<f32>(count * h.outputDims);
    f64 loss = 0.0;
    for (u32 s = 0; s < count; ++s) {
        const f32* x = inputs + static_cast<usize>(s) * h.inputDims;
        const f32* t = targets + static_cast<usize>(s) * h.outputDims;
        f32* acts = m_acts.data() + static_cast<usize>(s) * m_actStride;
        // Forward (the same operations as NeuralNet::infer), keeping every layer's output.
        net.encode(x, acts);
        u32 inAt = 0;
        u32 outAt = h.encodedDims;
        for (u32 j = 0; j < h.layerCount; ++j) {
            const u32 in = h.layerIn[j];
            const u32 out = h.layerOut[j];
            const NeuralActivation act = static_cast<NeuralActivation>(j + 1u == h.layerCount ? h.outputActivation
                                                                                             : h.hiddenActivation);
            for (u32 o = 0; o < out; ++o) {
                f32 acc = p[h.biasOffset[j] + o];
                const f32* row = p + h.weightOffset[j] + static_cast<usize>(o) * in;
                for (u32 i = 0; i < in; ++i) {
                    acc = acc + row[i] * acts[inAt + i];
                }
                acts[outAt + o] = applyNeuralActivation(act, acc, h.leakySlope);
            }
            inAt = outAt;
            outAt += out;
        }
        // Output delta.
        const f32* y = acts + inAt;
        const NeuralActivation outAct = static_cast<NeuralActivation>(h.outputActivation);
        for (u32 o = 0; o < h.outputDims; ++o) {
            const f32 e = y[o] - t[o];
            loss += static_cast<f64>(e) * static_cast<f64>(e);
            m_delta[o] = lossScale * e * activationDerivative(outAct, y[o], h.leakySlope);
        }
        // Backward.
        u32 layerOutAt = inAt;
        const bool gridGrad = h.encoding == static_cast<u32>(NeuralEncoding::HashGrid);
        for (u32 jj = h.layerCount; jj-- > 0u;) {
            const u32 in = h.layerIn[jj];
            const u32 out = h.layerOut[jj];
            const u32 layerInAt = jj == 0u ? 0u : layerOutAt - in;
            const f32* a = acts + layerInAt;
            for (u32 o = 0; o < out; ++o) {
                const f32 d = m_delta[o];
                f32* grow = g + h.weightOffset[jj] + static_cast<usize>(o) * in;
                for (u32 i = 0; i < in; ++i) {
                    grow[i] += d * a[i];
                }
                g[h.biasOffset[jj] + o] += d;
            }
            if (jj == 0u && !gridGrad) {
                break;
            }
            for (u32 i = 0; i < in; ++i) {
                f32 sum = 0.0f;
                for (u32 o = 0; o < out; ++o) {
                    sum += p[h.weightOffset[jj] + static_cast<usize>(o) * in + i] * m_delta[o];
                }
                m_deltaIn[i] = jj > 0u
                                   ? sum * activationDerivative(static_cast<NeuralActivation>(h.hiddenActivation), a[i],
                                                                h.leakySlope)
                                   : sum;
            }
            std::memcpy(m_delta, m_deltaIn, in * sizeof(f32));
            layerOutAt = layerInAt;
        }
        if (gridGrad) {
            u32 index[8];
            f32 weight[8];
            for (u32 l = 0; l < h.levels; ++l) {
                const u32 corners = net.gridCorners(x, l, index, weight);
                for (u32 k = 0; k < corners; ++k) {
                    for (u32 f = 0; f < h.features; ++f) {
                        g[index[k] + f] += weight[k] * m_delta[l * h.features + f];
                    }
                }
            }
        }
    }
    return static_cast<f32>(loss / static_cast<f64>(count * h.outputDims));
}

f32 NeuralTrainer::step(NeuralNet& net, const f32* inputs, const f32* targets, u32 count) {
    const f32 loss = computeGradient(net, inputs, targets, count);
    if (loss < 0.0f) {
        return loss;
    }
    ++m_step;
    const f32 b1 = m_adam.beta1;
    const f32 b2 = m_adam.beta2;
    const f32 c1 = 1.0f - static_cast<f32>(std::pow(static_cast<f64>(b1), static_cast<f64>(m_step)));
    const f32 c2 = 1.0f - static_cast<f32>(std::pow(static_cast<f64>(b2), static_cast<f64>(m_step)));
    const usize n = m_grad.size();
    for (usize i = 0; i < n; ++i) {
        const f32 gi = m_grad[i];
        m_m[i] = b1 * m_m[i] + (1.0f - b1) * gi;
        m_v[i] = b2 * m_v[i] + (1.0f - b2) * gi * gi;
        const f32 mh = m_m[i] / c1;
        const f32 vh = m_v[i] / c2;
        m_master[i] -= m_adam.learningRate * mh / (std::sqrt(vh) + m_adam.epsilon);
    }
    net.setParams(m_master.data(), static_cast<u32>(n));
    return loss;
}

// --- weight file ------------------------------------------------------------------------------------------
void writeNeuralWeights(const NeuralNet& net, std::vector<u8>& out) {
    const NeuralNetConfig& c = net.config();
    const u32 n = net.paramCount();
    const bool half = c.precision == NeuralPrecision::F16;
    const usize payload = static_cast<usize>(n) * (half ? 2u : 4u);
    out.assign(kNeuralFileHeaderBytes + payload, 0u);
    u8* d = out.data();
    std::memcpy(d, "FNNW", 4);
    putU32(d + 4, kNeuralFileVersion);
    putU32(d + 8, kNeuralFileHeaderBytes);
    putU32(d + 12, n);
    putU32(d + 16, c.inputDims);
    putU32(d + 20, static_cast<u32>(c.encoding));
    putU32(d + 24, c.frequencies);
    putU32(d + 28, c.levels);
    putU32(d + 32, c.featuresPerLevel);
    putU32(d + 36, c.log2TableSize);
    putU32(d + 40, c.baseResolution);
    putF32(d + 44, c.perLevelScale);
    putU32(d + 48, c.hiddenWidth);
    putU32(d + 52, c.hiddenLayers);
    putU32(d + 56, c.outputDims);
    putU32(d + 60, static_cast<u32>(c.hiddenActivation));
    putU32(d + 64, static_cast<u32>(c.outputActivation));
    putF32(d + 68, c.leakySlope);
    putU32(d + 72, static_cast<u32>(c.precision));
    u8* p = d + kNeuralFileHeaderBytes;
    const f32* params = net.params().data();
    for (u32 i = 0; i < n; ++i) {
        if (half) {
            const u16 hb = f32ToF16(params[i]);
            std::memcpy(p + static_cast<usize>(i) * 2u, &hb, 2);
        } else {
            std::memcpy(p + static_cast<usize>(i) * 4u, &params[i], 4);
        }
    }
    putU32(d + 76, fnv1a(p, payload));
}

bool readNeuralWeights(const u8* data, usize size, NeuralNet& net) {
    if (data == nullptr || size < kNeuralFileHeaderBytes || std::memcmp(data, "FNNW", 4) != 0 ||
        getU32(data + 4) != kNeuralFileVersion || getU32(data + 8) != kNeuralFileHeaderBytes) {
        return false;
    }
    NeuralNetConfig c{};
    c.inputDims = getU32(data + 16);
    c.encoding = static_cast<NeuralEncoding>(getU32(data + 20));
    c.frequencies = getU32(data + 24);
    c.levels = getU32(data + 28);
    c.featuresPerLevel = getU32(data + 32);
    c.log2TableSize = getU32(data + 36);
    c.baseResolution = getU32(data + 40);
    c.perLevelScale = getF32(data + 44);
    c.hiddenWidth = getU32(data + 48);
    c.hiddenLayers = getU32(data + 52);
    c.outputDims = getU32(data + 56);
    c.hiddenActivation = static_cast<NeuralActivation>(getU32(data + 60));
    c.outputActivation = static_cast<NeuralActivation>(getU32(data + 64));
    c.leakySlope = getF32(data + 68);
    c.precision = static_cast<NeuralPrecision>(getU32(data + 72));
    NeuralNet fresh;
    if (!fresh.build(c)) {
        return false;
    }
    const u32 n = getU32(data + 12);
    const bool half = c.precision == NeuralPrecision::F16;
    const usize payload = static_cast<usize>(n) * (half ? 2u : 4u);
    if (n != fresh.paramCount() || size != kNeuralFileHeaderBytes + payload) {
        return false;
    }
    const u8* p = data + kNeuralFileHeaderBytes;
    if (fnv1a(p, payload) != getU32(data + 76)) {
        return false;
    }
    for (u32 i = 0; i < n; ++i) {
        if (half) {
            u16 hb = 0;
            std::memcpy(&hb, p + static_cast<usize>(i) * 2u, 2);
            fresh.m_params[i] = f16ToF32(hb);
        } else {
            std::memcpy(&fresh.m_params[i], p + static_cast<usize>(i) * 4u, 4);
        }
    }
    net = std::move(fresh); // fresh.build() stamped a new version
    return true;
}

} // namespace fuse::renderer::neural
