// C++23 host side of the B1 shared-header CUDA gate. Compiled by the engine's host compiler at the
// engine dialect; evaluates the identical FUSE_HOST_DEVICE code and compares with nvcc's host pass
// (always) and the device kernel (only when a CUDA device is present — never claimed otherwise).

#include "shared_layout_asserts.hpp"

#include <cmath>
#include <cstdio>

static_assert(__cplusplus > 202002L, "engine host TU must be C++23");

extern "C" void fuse_cuda_gate_eval_nvcc_host(const fuse::cuda_gate::GateSample* samples, fuse::f32* results,
                                              fuse::u32 count);
extern "C" bool fuse_cuda_gate_eval_device(const fuse::cuda_gate::GateSample* samples, fuse::f32* results,
                                           fuse::u32 count);

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

} // namespace

int main() {
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

    if (fuse_cuda_gate_eval_device(samples, device, kGateSampleCount)) {
        failures += compare("CUDA device kernel", cxx23, device, 1e-4f);
        std::printf("fuse_cuda_shared_headers: device kernel ran and matched host (%s)\n",
                    failures == 0 ? "ok" : "MISMATCH");
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
