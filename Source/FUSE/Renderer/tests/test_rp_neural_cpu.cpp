// WP-9.1 neural runtime CPU gates (stub-safe; the Lavapipe gates are test_rp_neural.cpp).
//
//   layout        record sizes / offsets; the GLSL and Slang mirrors (nn_common.{glsl,slang}) of NeuralGpuHeader
//                 and NeuralPush declare the same fields in the same order at the same offsets; GPU blob layout
//   fp16          binary16 conversion: every half widens and narrows back to itself; fp32 -> fp16 is round to
//                 nearest even (checked against an exact double-precision neighbour search), overflow, subnormals
//   naive_parity  NeuralNet::infer == an independent naive implementation that parses the FNNW file itself, bit
//                 for bit (fp32), over the configuration matrix (identity / frequency / hash grid 2D + 3D, every
//                 activation, fp32 + fp16 parameters)
//   file          FNNW round trip (bytes and parameters identical), fp16 files are half the payload, corrupted /
//                 truncated / wrong-magic files are rejected and leave the net untouched
//   gradient      NeuralTrainer::computeGradient vs central finite differences (MLP weights, biases, hash tables)
//   train_fit     2D image fit (hash grid, Adam): loss falls >= 20x and PSNR >= 28 dB (fp32) / 27 dB (fp16
//                 parameters); frequency-encoded fit falls >= 5x
//   determinism   two trainings from one seed give identical parameters and weight files
//   coopmat_spirv the cooperative-matrix kernel is built (compile-only here): valid SPIR-V header, declares
//                 CooperativeMatrixKHR and issues OpCooperativeMatrixMulAddKHR (exit 77 when glslang lacked it)
//   zero_alloc    steady-state inference and training steps make no heap allocation (replaced operator new)
#include <fuse/renderer/neural/neural_gpu.hpp>
#include <fuse/renderer/neural/neural_mlp.hpp>
#include <fuse/renderer/neural/neural_types.hpp>

#include "test_rp_neural_common.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

// GCC may inline the replacement operators into callers and then report a false
// -Wmismatched-new-delete at -O2; replacement functions must not be inline anyway.
#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace fuse::renderer::neural;
using fuse::f32;
using fuse::f64;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using nn_test::Rng;

constexpr int kSkip = 77;
int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- layout ----------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define NN_FIELD(T, n) Field{#n, offsetof(T, n)}
const Field kHeaderFields[] = {
    NN_FIELD(NeuralGpuHeader, inputDims),        NN_FIELD(NeuralGpuHeader, encoding),
    NN_FIELD(NeuralGpuHeader, encodedDims),      NN_FIELD(NeuralGpuHeader, layerCount),
    NN_FIELD(NeuralGpuHeader, outputDims),       NN_FIELD(NeuralGpuHeader, paramPrecision),
    NN_FIELD(NeuralGpuHeader, frequencies),      NN_FIELD(NeuralGpuHeader, levels),
    NN_FIELD(NeuralGpuHeader, features),         NN_FIELD(NeuralGpuHeader, tableMask),
    NN_FIELD(NeuralGpuHeader, hiddenActivation), NN_FIELD(NeuralGpuHeader, outputActivation),
    NN_FIELD(NeuralGpuHeader, leakySlope),       NN_FIELD(NeuralGpuHeader, gridOffset),
    NN_FIELD(NeuralGpuHeader, denseMask),        NN_FIELD(NeuralGpuHeader, reserved),
    NN_FIELD(NeuralGpuHeader, layerIn),          NN_FIELD(NeuralGpuHeader, layerOut),
    NN_FIELD(NeuralGpuHeader, weightOffset),     NN_FIELD(NeuralGpuHeader, biasOffset),
    NN_FIELD(NeuralGpuHeader, levelResolution)};
const Field kPushFields[] = {NN_FIELD(NeuralPush, header),  NN_FIELD(NeuralPush, params),
                             NN_FIELD(NeuralPush, inputs),  NN_FIELD(NeuralPush, outputs),
                             NN_FIELD(NeuralPush, count),   NN_FIELD(NeuralPush, reserved)};
#undef NN_FIELD

std::string readText(const std::string& path) {
    std::ifstream file(path);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/// Parses `struct <name> {...};` of a shader source: (name, std430 / scalar offset) per field.
bool parseShaderStruct(const std::string& text, const std::string& name, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    const std::string key = "struct " + name + " {";
    const size_t begin = text.find(key);
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + key.size(), end - begin - key.size()));
    std::string line;
    size_t offset = 0;
    size_t align = 4;
    while (std::getline(body, line)) {
        std::istringstream ls(line);
        std::string type, decl;
        if (!(ls >> type >> decl) || decl.back() != ';') {
            continue;
        }
        decl.pop_back();
        size_t count = 1;
        const size_t bracket = decl.find('[');
        if (bracket != std::string::npos) {
            count = static_cast<size_t>(std::stoul(decl.substr(bracket + 1)));
            decl = decl.substr(0, bracket);
        }
        const size_t bytes = type == "uint64_t" ? 8u : 4u;
        align = std::max(align, bytes);
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes * count;
    }
    size = (offset + align - 1u) / align * align;
    return true;
}

template <size_t N>
void checkStruct(const std::string& text, const char* lang, const char* shaderName, const Field (&fields)[N],
                 size_t cppSize) {
    std::vector<size_t> offsets;
    std::vector<std::string> names;
    size_t size = 0;
    const bool parsed = parseShaderStruct(text, shaderName, offsets, size, names);
    expect(parsed, "shader struct parsed");
    if (!parsed) {
        std::fprintf(stderr, "  %s: struct %s missing\n", lang, shaderName);
        return;
    }
    bool same = offsets.size() == N && size == cppSize;
    for (size_t i = 0; same && i < N; ++i) {
        same = names[i] == fields[i].name && offsets[i] == fields[i].offset;
        if (!same) {
            std::fprintf(stderr, "  %s %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, shaderName, i,
                         names[i].c_str(), offsets[i], fields[i].name, fields[i].offset);
        }
    }
    std::printf("layout: %s %s %zu fields, %zu bytes\n", lang, shaderName, offsets.size(), size);
    expect(same, "shader struct == C++ record (names, order, offsets, size)");
}

void testLayout() {
    expect(sizeof(NeuralGpuHeader) == 256u && sizeof(NeuralPush) == 40u, "record sizes");
    const std::string dir = FUSE_RP_NEURAL_SHADER_DIR;
    const std::string glsl = readText(dir + "/nn_common.glsl");
    const std::string slang = readText(dir + "/nn_common.slang");
    expect(!glsl.empty() && !slang.empty(), "shader sources readable");
    checkStruct(glsl, "glsl", "NeuralGpuHeader", kHeaderFields, sizeof(NeuralGpuHeader));
    checkStruct(slang, "slang", "NeuralGpuHeader", kHeaderFields, sizeof(NeuralGpuHeader));
    checkStruct(glsl, "glsl", "NeuralPush", kPushFields, sizeof(NeuralPush));
    checkStruct(slang, "slang", "NeuralPush", kPushFields, sizeof(NeuralPush));
    // Shader constants mirror the C++ ones.
    for (const std::string* text : {&glsl, &slang}) {
        expect(text->find("2654435761u") != std::string::npos && text->find("805459861u") != std::string::npos &&
                   text->find("3.14159265358979") != std::string::npos,
               "hash primes and pi mirrored in the shader");
    }
    // GPU blob: header, then the parameters (fp16: packed halves, padded to 4 bytes).
    for (const nn_test::NamedConfig& nc : nn_test::configMatrix()) {
        NeuralNet net;
        expect(net.init(nc.config, 7), "init");
        const u64 bytes = net.gpuBlobBytes();
        const bool half = nc.config.precision == NeuralPrecision::F16;
        const u64 want = 256u + (half ? (u64{net.paramCount()} * 2u + 3u) / 4u * 4u : u64{net.paramCount()} * 4u);
        expect(bytes == want, "blob size");
        std::vector<u8> blob(bytes);
        net.writeGpuBlob(blob.data());
        expect(std::memcmp(blob.data(), &net.header(), 256u) == 0, "blob starts with the header");
        bool same = true;
        for (u32 i = 0; i < net.paramCount(); ++i) {
            f32 v = 0;
            if (half) {
                u16 h = 0;
                std::memcpy(&h, blob.data() + 256u + i * 2u, 2);
                v = f16ToF32(h);
            } else {
                std::memcpy(&v, blob.data() + 256u + i * 4u, 4);
            }
            same = same && std::bit_cast<u32>(v) == std::bit_cast<u32>(net.params()[i]);
        }
        expect(same, "blob parameters == NeuralNet::params()");
        // Offsets: the last layer's bias ends the parameter array.
        const NeuralGpuHeader& h = net.header();
        expect(h.biasOffset[h.layerCount - 1u] + h.layerOut[h.layerCount - 1u] == net.paramCount(), "offsets tile");
    }
    std::printf("layout: blob layout checked over %zu configurations\n", nn_test::configMatrix().size());
}

// --- fp16 ------------------------------------------------------------------------------------------
/// Exact reference: the nearest finite half (ties to even) by scanning neighbours in double precision.
u16 referenceF16(f32 v) {
    if (std::isnan(v)) {
        return 0x7E00u;
    }
    const f64 x = static_cast<f64>(v);
    const u16 sign = std::signbit(v) ? 0x8000u : 0u;
    const f64 ax = std::fabs(x);
    // Largest half 65504; the rounding boundary to infinity is 65520.
    if (ax >= 65520.0) {
        return static_cast<u16>(sign | 0x7C00u);
    }
    u16 best = 0;
    f64 bestErr = 1e300;
    // Binary search over the monotone positive halves, then check the neighbours.
    u32 lo = 0, hi = 0x7BFFu;
    while (lo < hi) {
        const u32 mid = (lo + hi + 1u) / 2u;
        if (static_cast<f64>(f16ToF32(static_cast<u16>(mid))) <= ax) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    for (u32 c = lo; c <= std::min<u32>(lo + 1u, 0x7BFFu); ++c) {
        const f64 err = std::fabs(static_cast<f64>(f16ToF32(static_cast<u16>(c))) - ax);
        if (err < bestErr || (err == bestErr && (c & 1u) == 0u)) {
            best = static_cast<u16>(c);
            bestErr = err;
        }
    }
    return static_cast<u16>(sign | best);
}

void testFp16() {
    u32 roundTrip = 0;
    for (u32 h = 0; h < 0x10000u; ++h) {
        const u32 exp = (h >> 10) & 0x1Fu;
        const u32 mant = h & 0x3FFu;
        if (exp == 31u && mant != 0u) {
            expect(std::isnan(f16ToF32(static_cast<u16>(h))), "NaN widens to NaN");
            continue;
        }
        if (f32ToF16(f16ToF32(static_cast<u16>(h))) != h) {
            ++roundTrip;
        }
    }
    expect(roundTrip == 0u, "every half widens and narrows back to itself");
    Rng rng(16);
    u32 mismatches = 0;
    const u32 kSamples = 400000u;
    for (u32 i = 0; i < kSamples; ++i) {
        f32 v = 0;
        switch (i % 4u) {
        case 0: // any finite float with a half-range exponent
            v = std::ldexp(rng.range(1.0f, 2.0f), static_cast<int>(rng.next() % 44u) - 28) * ((i & 4u) ? -1.f : 1.f);
            break;
        case 1: // exact ties between neighbouring halves
        {
            const u16 h = static_cast<u16>(rng.next() % 0x7BFFu);
            v = 0.5f * (f16ToF32(h) + f16ToF32(static_cast<u16>(h + 1u)));
            break;
        }
        case 2:
            v = rng.range(-1.0f, 1.0f);
            break;
        default:
            v = std::bit_cast<f32>(static_cast<u32>(rng.next()));
            if (!std::isfinite(v)) {
                v = 1.0f;
            }
            break;
        }
        if (f32ToF16(v) != referenceF16(v)) {
            if (mismatches < 5u) {
                std::fprintf(stderr, "  f32ToF16(%a) = %04x, reference %04x\n", static_cast<f64>(v), f32ToF16(v),
                             referenceF16(v));
            }
            ++mismatches;
        }
    }
    expect(mismatches == 0u, "fp32 -> fp16 rounds to nearest even");
    expect(f32ToF16(65504.0f) == 0x7BFFu && f32ToF16(65520.0f) == 0x7C00u && f32ToF16(1e9f) == 0x7C00u,
           "overflow to infinity");
    expect(f32ToF16(5.9604645e-8f) == 0x0001u && f32ToF16(2.9802322e-8f) == 0x0000u, "subnormal ends");
    expect(f32ToF16(-0.0f) == 0x8000u && f16ToF32(0x3C00u) == 1.0f, "signed zero, one");
    std::printf("fp16: 65536 halves round-trip, %u floats vs exact RNE reference: %u mismatches\n", kSamples,
                mismatches);
}

// --- naive oracle ------------------------------------------------------------------------------------
/// Independent implementation: parses the FNNW bytes itself and evaluates with plain loops (the documented order:
/// bias first, then inputs 0..n-1; corners in bit order; weights multiplied per axis in axis order).
struct Naive {
    u32 in = 0, enc = 0, freq = 0, levels = 0, feat = 0, log2T = 0, base = 0, hiddenW = 0, hiddenL = 0, outD = 0;
    u32 hidAct = 0, outAct = 0, prec = 0;
    f32 scale = 0, slope = 0;
    std::vector<f32> p;

    bool parse(const std::vector<u8>& f) {
        auto u = [&](usize at) {
            u32 v;
            std::memcpy(&v, f.data() + at, 4);
            return v;
        };
        auto fl = [&](usize at) {
            f32 v;
            std::memcpy(&v, f.data() + at, 4);
            return v;
        };
        if (f.size() < 80u || std::memcmp(f.data(), "FNNW", 4) != 0) {
            return false;
        }
        const u32 n = u(12);
        in = u(16);
        enc = u(20);
        freq = u(24);
        levels = u(28);
        feat = u(32);
        log2T = u(36);
        base = u(40);
        scale = fl(44);
        hiddenW = u(48);
        hiddenL = u(52);
        outD = u(56);
        hidAct = u(60);
        outAct = u(64);
        slope = fl(68);
        prec = u(72);
        p.resize(n);
        for (u32 i = 0; i < n; ++i) {
            if (prec == 1u) {
                u16 h;
                std::memcpy(&h, f.data() + 80u + i * 2u, 2);
                // Widen by hand (independent of f16ToF32).
                const u32 s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
                f64 v = e == 0u ? std::ldexp(static_cast<f64>(m), -24)
                                : std::ldexp(1.0 + static_cast<f64>(m) / 1024.0, static_cast<int>(e) - 15);
                p[i] = static_cast<f32>(s != 0u ? -v : v);
            } else {
                std::memcpy(&p[i], f.data() + 80u + i * 4u, 4);
            }
        }
        return true;
    }

    f32 act(u32 a, f32 v) const {
        if (a == 1u) {
            return v > 0.0f ? v : 0.0f;
        }
        if (a == 2u) {
            return v > 0.0f ? v : v * slope;
        }
        if (a == 3u) {
            return 1.0f / (1.0f + std::exp(-v));
        }
        return v;
    }

    void eval(const f32* x, f32* y) const {
        std::vector<f32> e;
        u32 cursor = 0;
        if (enc == 0u) {
            e.assign(x, x + in);
        } else if (enc == 1u) {
            for (u32 d = 0; d < in; ++d) {
                e.push_back(x[d]);
                f32 band = 3.14159265358979f;
                for (u32 l = 0; l < freq; ++l) {
                    e.push_back(std::sin(x[d] * band));
                    e.push_back(std::cos(x[d] * band));
                    band = band * 2.0f;
                }
            }
        } else {
            const u32 T = 1u << log2T;
            f64 s = 1.0;
            for (u32 l = 0; l < levels; ++l) {
                const u32 res = static_cast<u32>(std::floor(static_cast<f64>(base) * s));
                s *= static_cast<f64>(scale);
                u64 cells = 1;
                for (u32 d = 0; d < in; ++d) {
                    cells *= res + 1u;
                }
                const bool dense = cells <= T;
                u32 c0[3] = {};
                f32 fr[3] = {};
                for (u32 d = 0; d < in; ++d) {
                    f32 v = x[d] < 0.0f ? 0.0f : (x[d] > 1.0f ? 1.0f : x[d]);
                    const f32 pos = v * static_cast<f32>(res);
                    u32 c = static_cast<u32>(std::floor(pos));
                    if (c >= res) {
                        c = res - 1u;
                    }
                    c0[d] = c;
                    fr[d] = pos - static_cast<f32>(c);
                }
                for (u32 f = 0; f < feat; ++f) {
                    f32 acc = 0.0f;
                    for (u32 k = 0; k < (1u << in); ++k) {
                        f32 w = 1.0f;
                        u32 q[3] = {};
                        for (u32 d = 0; d < in; ++d) {
                            const bool hiBit = ((k >> d) & 1u) != 0u;
                            q[d] = c0[d] + (hiBit ? 1u : 0u);
                            w = w * (hiBit ? fr[d] : 1.0f - fr[d]);
                        }
                        u32 idx;
                        if (dense) {
                            idx = q[0] + q[1] * (res + 1u) + (in == 3u ? q[2] * (res + 1u) * (res + 1u) : 0u);
                        } else {
                            idx = q[0] ^ (q[1] * 2654435761u) ^ (in == 3u ? q[2] * 805459861u : 0u);
                        }
                        idx %= T;
                        acc = acc + w * p[(static_cast<usize>(l) * T + idx) * feat + f];
                    }
                    e.push_back(acc);
                }
            }
            cursor = levels * T * feat;
        }
        std::vector<f32> a = e;
        const u32 layers = hiddenL + 1u;
        for (u32 j = 0; j < layers; ++j) {
            const u32 ni = static_cast<u32>(a.size());
            const u32 no = j + 1u == layers ? outD : hiddenW;
            const u32 w0 = cursor;
            const u32 b0 = cursor + ni * no;
            cursor = b0 + no;
            std::vector<f32> b(no);
            for (u32 o = 0; o < no; ++o) {
                f32 acc = p[b0 + o];
                for (u32 i = 0; i < ni; ++i) {
                    acc = acc + p[w0 + o * ni + i] * a[i];
                }
                b[o] = act(j + 1u == layers ? outAct : hidAct, acc);
            }
            a = b;
        }
        for (u32 o = 0; o < outD; ++o) {
            y[o] = a[o];
        }
    }
};

void testNaiveParity() {
    u32 configs = 0;
    for (const nn_test::NamedConfig& nc : nn_test::configMatrix()) {
        NeuralNet net;
        expect(net.init(nc.config, 11), "init");
        nn_test::perturb(net, 12);
        std::vector<u8> file;
        writeNeuralWeights(net, file);
        Naive naive;
        expect(naive.parse(file), "naive parse");
        Rng rng(13 + configs);
        std::vector<f32> x;
        const u32 count = 2000;
        nn_test::randomInputs(rng, nc.config, count, x);
        // Exact grid vertices and the domain ends too.
        for (u32 d = 0; d < nc.config.inputDims; ++d) {
            x[d] = 0.0f;
            x[nc.config.inputDims + d] = 1.0f;
            x[2u * nc.config.inputDims + d] = 0.5f;
        }
        NeuralScratch scratch;
        std::vector<f32> y(static_cast<usize>(count) * nc.config.outputDims);
        net.inferBatch(x.data(), y.data(), count, scratch);
        u32 mismatches = 0;
        f32 ref[kNnMaxWidth];
        for (u32 s = 0; s < count; ++s) {
            naive.eval(x.data() + static_cast<usize>(s) * nc.config.inputDims, ref);
            for (u32 o = 0; o < nc.config.outputDims; ++o) {
                const f32 got = y[static_cast<usize>(s) * nc.config.outputDims + o];
                if (std::bit_cast<u32>(got) != std::bit_cast<u32>(ref[o])) {
                    if (mismatches < 3u) {
                        std::fprintf(stderr, "  %s sample %u out %u: %a vs naive %a\n", nc.name, s, o,
                                     static_cast<f64>(got), static_cast<f64>(ref[o]));
                    }
                    ++mismatches;
                }
            }
        }
        std::printf("naive_parity: %-36s %6u params, %u samples: %u mismatches\n", nc.name, net.paramCount(), count,
                    mismatches);
        expect(mismatches == 0u, "NeuralNet::infer == naive oracle bit for bit");
        ++configs;
    }
}

// --- file ------------------------------------------------------------------------------------------
void testFile() {
    for (const nn_test::NamedConfig& nc : nn_test::configMatrix()) {
        NeuralNet net;
        expect(net.init(nc.config, 21), "init");
        nn_test::perturb(net, 22);
        std::vector<u8> file;
        writeNeuralWeights(net, file);
        const bool half = nc.config.precision == NeuralPrecision::F16;
        expect(file.size() == kNeuralFileHeaderBytes + net.paramCount() * (half ? 2u : 4u), "file size");
        NeuralNet back;
        expect(readNeuralWeights(file.data(), file.size(), back), "read back");
        expect(back.paramCount() == net.paramCount() &&
                   std::memcmp(back.params().data(), net.params().data(), net.paramCount() * sizeof(f32)) == 0 &&
                   std::memcmp(&back.header(), &net.header(), sizeof(NeuralGpuHeader)) == 0,
               "parameters and header identical after the round trip");
        std::vector<u8> again;
        writeNeuralWeights(back, again);
        expect(again == file, "re-written file identical");
        const u64 version = back.version();
        std::vector<u8> bad = file;
        bad[kNeuralFileHeaderBytes + 3u] ^= 0x10u;
        expect(!readNeuralWeights(bad.data(), bad.size(), back), "payload corruption rejected (checksum)");
        bad = file;
        bad[0] = 'X';
        expect(!readNeuralWeights(bad.data(), bad.size(), back), "bad magic rejected");
        expect(!readNeuralWeights(file.data(), file.size() - 2u, back), "truncated file rejected");
        bad = file;
        bad[16] = 9; // inputDims 9
        expect(!readNeuralWeights(bad.data(), bad.size(), back), "invalid config rejected");
        expect(back.version() == version &&
                   std::memcmp(back.params().data(), net.params().data(), net.paramCount() * sizeof(f32)) == 0,
               "rejected reads leave the net untouched");
    }
    std::printf("file: FNNW round trip + 4 rejection cases over %zu configurations\n", nn_test::configMatrix().size());
}

// --- gradient ----------------------------------------------------------------------------------------
void testGradient() {
    struct Case {
        const char* name;
        NeuralNetConfig c;
    };
    std::vector<Case> cases;
    NeuralNetConfig c{};
    c.inputDims = 2;
    c.encoding = NeuralEncoding::Frequency;
    c.frequencies = 2;
    c.hiddenWidth = 8;
    c.hiddenLayers = 2;
    c.outputDims = 2;
    c.hiddenActivation = NeuralActivation::Sigmoid;
    c.outputActivation = NeuralActivation::Sigmoid;
    cases.push_back({"frequency sigmoid", c});
    c.outputActivation = NeuralActivation::None;
    c.hiddenActivation = NeuralActivation::LeakyReLU;
    c.leakySlope = 0.1f;
    cases.push_back({"frequency leaky", c});
    c = NeuralNetConfig{};
    c.inputDims = 2;
    c.encoding = NeuralEncoding::HashGrid;
    c.levels = 3;
    c.featuresPerLevel = 2;
    c.log2TableSize = 4;
    c.baseResolution = 2;
    c.perLevelScale = 2.0f;
    c.hiddenWidth = 8;
    c.hiddenLayers = 1;
    c.outputDims = 2;
    c.hiddenActivation = NeuralActivation::Sigmoid;
    c.outputActivation = NeuralActivation::None;
    cases.push_back({"hashgrid sigmoid (tables)", c});
    c.inputDims = 3;
    c.hiddenActivation = NeuralActivation::ReLU;
    cases.push_back({"hashgrid 3D relu", c});

    for (const Case& k : cases) {
        NeuralNet net;
        expect(net.init(k.c, 31), "init");
        nn_test::perturb(net, 32);
        if (k.c.encoding == NeuralEncoding::HashGrid) {
            std::vector<f32> p = net.params();
            Rng r(33);
            for (u32 i = 0; i < net.header().weightOffset[0]; ++i) {
                p[i] = r.range(-1.0f, 1.0f);
            }
            net.setParams(p.data(), static_cast<u32>(p.size()));
        }
        Rng rng(34);
        const u32 count = 16;
        std::vector<f32> x, t(count * k.c.outputDims);
        nn_test::randomInputs(rng, k.c, count, x);
        for (f32& v : t) {
            v = rng.range(-1.0f, 1.0f);
        }
        NeuralTrainer trainer;
        expect(trainer.init(net, NeuralAdamDesc{}, count), "trainer init");
        trainer.computeGradient(net, x.data(), t.data(), count);
        const std::vector<f32> g = trainer.gradient();
        // Central differences in double over the (float) forward: loss recomputed with the same trainer.
        std::vector<f32> p = net.params();
        f64 dot = 0, na = 0, nf = 0;
        u32 checked = 0;
        u32 outliers = 0;
        f64 worst = 0;
        for (u32 i = 0; i < net.paramCount(); ++i) {
            const f32 h = k.c.hiddenActivation == NeuralActivation::Sigmoid ? 1e-2f : 4e-4f;
            const f32 orig = p[i];
            p[i] = orig + h;
            net.setParams(p.data(), static_cast<u32>(p.size()));
            const f64 lp = trainer.computeGradient(net, x.data(), t.data(), count);
            p[i] = orig - h;
            net.setParams(p.data(), static_cast<u32>(p.size()));
            const f64 lm = trainer.computeGradient(net, x.data(), t.data(), count);
            p[i] = orig;
            const f64 fd = (lp - lm) / (2.0 * static_cast<f64>(h));
            dot += fd * g[i];
            na += static_cast<f64>(g[i]) * g[i];
            nf += fd * fd;
            if (std::fabs(fd) > 1e-3) {
                const f64 rel = std::fabs(fd - g[i]) / std::fabs(fd);
                worst = std::max(worst, rel);
                outliers += rel > 0.05 ? 1u : 0u;
                ++checked;
            }
        }
        net.setParams(p.data(), static_cast<u32>(p.size()));
        const f64 cosine = dot / std::sqrt(std::max(na * nf, 1e-300));
        std::printf("gradient: %-28s %4u params, cosine(analytic, FD) %.6f, worst rel err %.4f, %u > 5%% of %u\n",
                    k.name, net.paramCount(), cosine, worst, outliers, checked);
        expect(cosine > 0.9995, "analytic gradient parallel to finite differences");
        // Smooth networks must match everywhere; with (leaky) ReLU a unit can cross its kink inside +-h, which
        // spoils the central difference of the few parameters feeding it.
        const bool smooth = k.c.hiddenActivation == NeuralActivation::Sigmoid;
        if (smooth) {
            expect(worst < 0.02, "per-parameter gradient matches finite differences");
        } else {
            expect(outliers * 20u <= checked, "<= 5% of parameters off by > 5% (kinks)");
        }
    }
}

// --- training ----------------------------------------------------------------------------------------
struct FitResult {
    f32 firstLoss = 0;
    f32 lastLoss = 0;
    f64 psnr = 0;
};

FitResult fitImage(const NeuralNetConfig& c, u32 steps, u32 batch, f32 lr, u64 seed, NeuralNet* outNet = nullptr) {
    NeuralNet net;
    net.init(c, seed);
    NeuralAdamDesc adam{};
    adam.learningRate = lr;
    NeuralTrainer trainer;
    trainer.init(net, adam, batch);
    Rng rng(seed + 1u);
    std::vector<f32> x(batch * 2u), t(batch * 3u);
    FitResult r{};
    f32 tail = 0;
    for (u32 s = 0; s < steps; ++s) {
        for (u32 i = 0; i < batch; ++i) {
            x[i * 2u] = rng.uniform();
            x[i * 2u + 1u] = rng.uniform();
            nn_test::targetImage(x[i * 2u], x[i * 2u + 1u], &t[i * 3u]);
        }
        const f32 loss = trainer.step(net, x.data(), t.data(), batch);
        if (s < 5u) {
            r.firstLoss += loss / 5.0f;
        }
        if (s + 10u >= steps) {
            tail += loss / 10.0f;
        }
    }
    r.lastLoss = tail;
    // PSNR over a 128 x 128 pixel-centre grid.
    const u32 res = 128;
    f64 mse = 0;
    NeuralScratch scratch;
    for (u32 j = 0; j < res; ++j) {
        for (u32 i = 0; i < res; ++i) {
            const f32 p[2] = {(static_cast<f32>(i) + 0.5f) / res, (static_cast<f32>(j) + 0.5f) / res};
            f32 y[3], ref[3];
            net.infer(p, y, scratch);
            nn_test::targetImage(p[0], p[1], ref);
            for (u32 k = 0; k < 3u; ++k) {
                const f64 e = static_cast<f64>(std::min(std::max(y[k], 0.0f), 1.0f)) - ref[k];
                mse += e * e;
            }
        }
    }
    mse /= static_cast<f64>(res * res * 3u);
    r.psnr = 10.0 * std::log10(1.0 / std::max(mse, 1e-12));
    if (outNet != nullptr) {
        *outNet = net;
    }
    return r;
}

void testTrainFit() {
    const FitResult f32r = fitImage(nn_test::imageFitConfig(NeuralPrecision::F32), 600, 512, 1e-2f, 41);
    std::printf("train_fit: hash grid fp32: loss %.5f -> %.6f (x%.1f), PSNR %.2f dB\n", f32r.firstLoss, f32r.lastLoss,
                f32r.firstLoss / f32r.lastLoss, f32r.psnr);
    expect(f32r.lastLoss * 20.0f < f32r.firstLoss, "fp32 loss falls >= 20x");
    expect(f32r.psnr >= 28.0, "fp32 image fit PSNR >= 28 dB");
    const FitResult f16r = fitImage(nn_test::imageFitConfig(NeuralPrecision::F16), 600, 512, 1e-2f, 41);
    std::printf("train_fit: hash grid fp16 params: loss %.5f -> %.6f (x%.1f), PSNR %.2f dB\n", f16r.firstLoss,
                f16r.lastLoss, f16r.firstLoss / f16r.lastLoss, f16r.psnr);
    expect(f16r.lastLoss * 20.0f < f16r.firstLoss, "fp16 loss falls >= 20x");
    expect(f16r.psnr >= 27.0, "fp16 image fit PSNR >= 27 dB");
    NeuralNetConfig fc{};
    fc.inputDims = 2;
    fc.encoding = NeuralEncoding::Frequency;
    fc.frequencies = 4;
    fc.hiddenWidth = 32;
    fc.hiddenLayers = 2;
    fc.outputDims = 3;
    fc.hiddenActivation = NeuralActivation::ReLU;
    fc.outputActivation = NeuralActivation::Sigmoid;
    const FitResult fr = fitImage(fc, 600, 256, 5e-3f, 43);
    std::printf("train_fit: frequency relu/sigmoid: loss %.5f -> %.6f (x%.1f), PSNR %.2f dB\n", fr.firstLoss,
                fr.lastLoss, fr.firstLoss / fr.lastLoss, fr.psnr);
    expect(fr.lastLoss * 5.0f < fr.firstLoss, "frequency-encoded loss falls >= 5x");
}

void testDeterminism() {
    NeuralNet a, b;
    const NeuralNetConfig c = nn_test::imageFitConfig(NeuralPrecision::F16);
    fitImage(c, 50, 128, 1e-2f, 51, &a);
    fitImage(c, 50, 128, 1e-2f, 51, &b);
    std::vector<u8> fa, fb;
    writeNeuralWeights(a, fa);
    writeNeuralWeights(b, fb);
    expect(a.params() == b.params() && fa == fb, "identical trainings give identical parameters");
    std::printf("determinism: two 50-step trainings: %s\n", fa == fb ? "identical" : "DIFFERENT");
}

// --- coopmat_spirv -----------------------------------------------------------------------------------
int testCoopMatrixSpirv() {
    usize bytes = 0;
    const u32* words = neuralCoopMatrixSpirv(&bytes);
    if (words == nullptr) {
        std::printf("SKIP: glslang without GL_KHR_cooperative_matrix: the kernel was not built\n");
        return kSkip;
    }
    expect(bytes >= 20u && (bytes % 4u) == 0u && words[0] == 0x07230203u, "SPIR-V header");
    const usize count = bytes / 4u;
    bool capability = false, mulAdd = false, load = false, store = false, walked = true;
    constexpr u32 kOpCapability = 17u;
    constexpr u32 kCapCooperativeMatrixKHR = 6022u;
    constexpr u32 kOpCoopLoad = 4457u;
    constexpr u32 kOpCoopStore = 4458u;
    constexpr u32 kOpCoopMulAdd = 4459u;
    usize i = 5;
    while (i < count) {
        const u32 op = words[i] & 0xFFFFu;
        const u32 len = words[i] >> 16;
        if (len == 0u || i + len > count) {
            walked = false;
            break;
        }
        capability = capability || (op == kOpCapability && words[i + 1] == kCapCooperativeMatrixKHR);
        load = load || op == kOpCoopLoad;
        store = store || op == kOpCoopStore;
        mulAdd = mulAdd || op == kOpCoopMulAdd;
        i += len;
    }
    expect(walked, "instruction stream well formed");
    expect(capability && load && store && mulAdd,
           "declares CooperativeMatrixKHR and loads / multiplies / stores cooperative matrices");
    std::printf("coopmat_spirv: %zu words, capability %d, load %d, mul-add %d, store %d (compile-only; Lavapipe has "
                "no VK_KHR_cooperative_matrix)\n",
                count, capability, load, mulAdd, store);
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
void testZeroAlloc() {
    unsigned long long total = 0;
    for (const NeuralPrecision precision : {NeuralPrecision::F32, NeuralPrecision::F16}) {
        NeuralNet net;
        net.init(nn_test::imageFitConfig(precision), 61);
        NeuralTrainer trainer;
        const u32 batch = 256;
        trainer.init(net, NeuralAdamDesc{}, batch);
        NeuralScratch scratch;
        Rng rng(62);
        std::vector<f32> x(batch * 2u), t(batch * 3u), y(batch * 3u);
        f32 sink = 0;
        for (u32 frame = 0; frame < 24u; ++frame) {
            for (u32 i = 0; i < batch; ++i) {
                x[i * 2u] = rng.uniform();
                x[i * 2u + 1u] = rng.uniform();
                nn_test::targetImage(x[i * 2u], x[i * 2u + 1u], &t[i * 3u]);
            }
            t_allocations = 0;
            t_count = frame >= 4u;
            sink += trainer.step(net, x.data(), t.data(), batch);
            net.inferBatch(x.data(), y.data(), batch, scratch);
            t_count = false;
            total += t_allocations;
            sink += y[0];
        }
        std::printf("zero_alloc: %s, 20 steady-state frames (Adam step + %u inferences): %llu operator-new calls "
                    "(checksum %.3f)\n",
                    precision == NeuralPrecision::F16 ? "fp16" : "fp32", batch, total, static_cast<f64>(sink));
    }
    expect(total == 0u, "training steps and inference make no steady-state heap allocation");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    int rc = 0;
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "fp16") {
        testFp16();
    }
    if (all || suite == "naive_parity") {
        testNaiveParity();
    }
    if (all || suite == "file") {
        testFile();
    }
    if (all || suite == "gradient") {
        testGradient();
    }
    if (all || suite == "train_fit") {
        testTrainFit();
    }
    if (all || suite == "determinism") {
        testDeterminism();
    }
    if (all || suite == "coopmat_spirv") {
        rc = testCoopMatrixSpirv();
    }
    if (all || suite == "zero_alloc") {
        testZeroAlloc();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    if (rc == kSkip && !all) {
        return kSkip;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
