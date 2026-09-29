// C++23 host side of the B1 shared-header CUDA gate. Compiled by the engine's host compiler at the
// engine dialect; evaluates the identical FUSE_HOST_DEVICE code and compares with nvcc's host pass
// (always) and the device kernel (only when a CUDA device is present — never claimed otherwise).
// The GRIA Alpha sweep (gria_alpha_gate.hpp) runs the same way: C++23 host vs nvcc host vs device.
// `--require-device` (ctest fuse_cuda_gria_alpha_device) exits 77 without a CUDA device instead of
// reporting compile-only, so a pass there always means the device kernel ran and matched.

#include "gria_alpha_gate.hpp"
#include "shared_layout_asserts.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static_assert(__cplusplus > 202002L, "engine host TU must be C++23");

extern "C" void fuse_cuda_gate_eval_nvcc_host(const fuse::cuda_gate::GateSample* samples, fuse::f32* results,
                                              fuse::u32 count);
extern "C" bool fuse_cuda_gate_eval_device(const fuse::cuda_gate::GateSample* samples, fuse::f32* results,
                                           fuse::u32 count);
extern "C" void fuse_cuda_gate_alpha_nvcc_host(const fuse::cuda_gate::AlphaInput* inputs,
                                               fuse::cuda_gate::AlphaRecord* records, fuse::u32 count);
extern "C" bool fuse_cuda_gate_alpha_device(const fuse::cuda_gate::AlphaInput* inputs,
                                            fuse::cuda_gate::AlphaRecord* records, fuse::u32 count);

namespace {

using namespace fuse::cuda_gate;

int compare(const char* label, const fuse::f32* expected, const fuse::f32* actual, fuse::f32 tolerance) {
    int failures = 0;
    for (fuse::u32 i = 0; i < kGateSampleCount * kGateResultFloats; ++i) {
        if (!(std::fabs(expected[i] - actual[i]) <= tolerance)) {
            std::fprintf(stderr, "FAIL [%s] value %u: C++23 host %.9g vs %.9g\n", label, i, expected[i], actual[i]);
            ++failures;
        }
    }
    return failures;
}

/// get / entropy / flags / default must match bit for bit (clamps, compares, one correctly rounded
/// division); blend is one multiply-add, which the device may contract into an FMA (<= 1e-6 relative).
int compareAlpha(const char* label, const std::vector<AlphaInput>& inputs, const std::vector<AlphaRecord>& expected,
                 const std::vector<AlphaRecord>& actual) {
    int failures = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const AlphaRecord& e = expected[i];
        const AlphaRecord& a = actual[i];
        const fuse::f32 blendTol = 1e-6f * std::fmax(1.f, std::fabs(e.blend));
        const bool ok = sameBits(e.get, a.get) && sameBits(e.entropy, a.entropy) && e.flags == a.flags &&
                        sameBits(e.default_get, a.default_get) && std::fabs(e.blend - a.blend) <= blendTol;
        if (!ok && failures < 16) {
            std::fprintf(stderr,
                         "FAIL [%s] Alpha input %zu (value %.9g): get %.9g/%.9g entropy %.9g/%.9g blend %.9g/%.9g "
                         "flags 0x%x/0x%x\n",
                         label, i, static_cast<double>(inputs[i].value), static_cast<double>(e.get),
                         static_cast<double>(a.get), static_cast<double>(e.entropy), static_cast<double>(a.entropy),
                         static_cast<double>(e.blend), static_cast<double>(a.blend), e.flags, a.flags);
        }
        failures += ok ? 0 : 1;
    }
    return failures;
}

/// The C++23 host results themselves follow the Alpha contract (so a device match is a match to truth).
int checkAlphaContract(const std::vector<AlphaInput>& inputs, const std::vector<AlphaRecord>& host) {
    int failures = 0;
    for (std::size_t i = 0; i < host.size(); ++i) {
        const fuse::f32 v = inputs[i].value;
        const fuse::f32 expected = v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; // NaN -> 0
        const AlphaRecord& r = host[i];
        const bool ok = sameBits(r.get, expected) && r.get >= 0.f && r.get <= 1.f && r.entropy >= 0.f &&
                        r.entropy <= 1.f && (inputs[i].h_input > 0.f || r.entropy == 0.f) && r.default_get == 0.f &&
                        ((r.flags & 1u) != 0u) == (r.get < 0.01f) && ((r.flags & 2u) != 0u) == (r.get > 0.99f) &&
                        ((r.flags & 4u) != 0u) == (r.get > 0.49f && r.get < 0.51f) && (r.flags & 8u) != 0u;
        if (!ok && failures < 16) {
            std::fprintf(stderr, "FAIL [Alpha contract] input %zu value %.9g -> get %.9g flags 0x%x\n", i,
                         static_cast<double>(v), static_cast<double>(r.get), r.flags);
        }
        failures += ok ? 0 : 1;
    }
    return failures;
}

} // namespace

int main(int argc, char** argv) {
    const bool requireDevice = argc > 1 && std::strcmp(argv[1], "--require-device") == 0;
    GateSample samples[kGateSampleCount];
    for (fuse::u32 i = 0; i < kGateSampleCount; ++i) {
        samples[i] = gateSample(i);
    }

    fuse::f32 cxx23[kGateSampleCount * kGateResultFloats]{};
    fuse::f32 nvccHost[kGateSampleCount * kGateResultFloats]{};
    fuse::f32 device[kGateSampleCount * kGateResultFloats]{};
    for (fuse::u32 i = 0; i < kGateSampleCount; ++i) {
        evaluateGateSample(samples[i], cxx23 + i * kGateResultFloats);
    }
    fuse_cuda_gate_eval_nvcc_host(samples, nvccHost, kGateSampleCount);

    // Same source, different compilers/dialects/flags: allow FMA-contraction-level differences.
    int failures = compare("nvcc host pass (C++20)", cxx23, nvccHost, 1e-5f);

    std::vector<AlphaInput> alphaInputs(kAlphaSweepCount);
    for (fuse::u32 i = 0; i < kAlphaSweepCount; ++i) {
        alphaInputs[i] = alphaSweepInput(i);
    }
    std::vector<AlphaRecord> alphaHost(kAlphaSweepCount);
    std::vector<AlphaRecord> alphaNvccHost(kAlphaSweepCount);
    std::vector<AlphaRecord> alphaDevice(kAlphaSweepCount);
    for (fuse::u32 i = 0; i < kAlphaSweepCount; ++i) {
        alphaHost[i] = evaluateAlpha(alphaInputs[i]);
    }
    fuse_cuda_gate_alpha_nvcc_host(alphaInputs.data(), alphaNvccHost.data(), kAlphaSweepCount);
    failures += checkAlphaContract(alphaInputs, alphaHost);
    failures += compareAlpha("Alpha nvcc host pass (C++20)", alphaInputs, alphaHost, alphaNvccHost);

    const bool deviceRan = fuse_cuda_gate_eval_device(samples, device, kGateSampleCount);
    if (deviceRan) {
        failures += compare("CUDA device kernel", cxx23, device, 1e-4f);
        const bool alphaRan = fuse_cuda_gate_alpha_device(alphaInputs.data(), alphaDevice.data(), kAlphaSweepCount);
        if (!alphaRan) {
            std::fprintf(stderr, "FAIL: GRIA Alpha device kernel did not run\n");
            ++failures;
        } else {
            const int alphaFailures = compareAlpha("GRIA Alpha CUDA device kernel", alphaInputs, alphaHost, alphaDevice);
            failures += alphaFailures;
            std::printf("fuse_cuda_shared_headers: GRIA Alpha device kernel: %u inputs, %d mismatch(es) vs C++23 host\n",
                        kAlphaSweepCount, alphaFailures);
        }
        std::printf("fuse_cuda_shared_headers: device kernel ran and matched host (%s)\n",
                    failures == 0 ? "ok" : "MISMATCH");
    } else if (requireDevice) {
        std::printf("SKIP fuse_cuda_gria_alpha_device: no CUDA device\n");
        return 77;
    } else {
        std::printf("fuse_cuda_shared_headers: no CUDA device — compile-only; device results NOT verified\n");
    }

    if (failures != 0) {
        std::fprintf(stderr, "fuse_cuda_shared_headers: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("fuse_cuda_shared_headers: shared FUSE headers compiled for C++23 host, nvcc host and device; "
                "layouts asserted identical\n");
    return 0;
}
