// CUDA side of the single-source compute kernel gate: the same test kernel bodies the CPU gate runs
// (compute_kernel_test_kernels.hpp) are instantiated through cuda_launch.cuh's __global__
// trampolines — item and workgroup (__shared__ scratch + __syncthreads phases) forms — so the
// device pass proves every body compiles for sm_XX. With a CUDA device present the executable also
// checks device vs CpuReference parity; without one it reports compile-only and passes.

#include "../compute_kernel_test_kernels.hpp"

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/parity.hpp>

#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace kernel = fuse::kernel;
namespace kt = fuse::kernel_test;
using fuse::f32;
using fuse::u32;

namespace {

// Device entries for every test kernel (explicit instantiation of the trampolines).
const kernel::DeviceEntryFn kHashEntry = &kernel::cuda::entry<kt::HashKernel, kt::HashParams>;
const kernel::DeviceEntryFn kWaveEntry = &kernel::cuda::entry<kt::WaveKernel, kt::WaveParams>;
const kernel::DeviceEntryFn kHistogramEntry = &kernel::cuda::entry<kt::TiledHistogramKernel, kt::HistogramParams>;
const kernel::DeviceEntryFn kScanEntry = &kernel::cuda::entry<kt::BlockScanKernel, kt::BlockScanParams>;
const kernel::DeviceEntryFn kAddOffsetsEntry = &kernel::cuda::entry<kt::AddOffsetsKernel, kt::AddOffsetsParams>;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

kernel::LaunchOptions cudaOptions(kernel::DeviceEntryFn entry) {
    kernel::LaunchOptions options{};
    options.cuda = entry;
    options.allow_fallback = false;
    return options;
}

void deviceHashParity() {
    constexpr u32 kW = 333;
    constexpr u32 kH = 77;
    const kernel::KernelLaunch launch{"test_hash_device", kernel::extent2(kW, kH), {16, 8, 1}};
    std::vector<u32> host(kW * kH, 0u);
    std::vector<u32> fromDevice(kW * kH, 0u);
    kernel::cuda::DeviceBuffer<u32> deviceOut(host.size());
    const kt::HashParams hostParams{kernel::make_span(host.data(), kW * kH), 5u};
    const kt::HashParams deviceParams{kernel::make_span(deviceOut.data(), kW * kH), 5u};
    const kernel::ParityReport report = kernel::run_parity(
        kernel::Backend::CpuReference, kernel::Backend::Cuda, launch, kt::HashKernel{}, hostParams, deviceParams,
        [&] {
            const bool copied = deviceOut.download(fromDevice.data(), fromDevice.size(), nullptr) &&
                                cudaDeviceSynchronize() == cudaSuccess;
            kernel::ParityReport r =
                kernel::compare_bitwise(std::span<const u32>(host), std::span<const u32>(fromDevice));
            r.launches_ok = r.launches_ok && copied;
            return r;
        },
        cudaOptions(kHashEntry));
    expectTrue(report.ok && report.backend_b == kernel::Backend::Cuda, "device hash kernel == CpuReference (bit-exact)");
}

void deviceHistogramParity() {
    constexpr u32 kCount = 256u * 21u + 100u;
    const u32 groups = kernel::div_up(kCount, kt::kHistogramBins);
    std::vector<u32> values(kCount);
    for (u32 i = 0; i < kCount; ++i) {
        values[i] = kt::hash_u32(i) % 911u;
    }
    std::vector<u32> hostGroups(groups * kt::kHistogramBins, 0u);
    std::vector<u32> hostHist(kt::kHistogramBins, 0u);
    std::vector<u32> devGroupsHost(hostGroups.size(), 0u);
    std::vector<u32> devHistHost(kt::kHistogramBins, 0u);
    kernel::cuda::DeviceBuffer<u32> dValues(kCount);
    kernel::cuda::DeviceBuffer<u32> dGroups(hostGroups.size());
    kernel::cuda::DeviceBuffer<u32> dHist(kt::kHistogramBins);
    bool staged = dValues.upload(values.data(), kCount, nullptr) &&
                  cudaMemset(dHist.data(), 0, kt::kHistogramBins * sizeof(u32)) == cudaSuccess;
    const kernel::KernelLaunch launch{"test_histogram_device", kernel::extent1(kCount), {kt::kHistogramBins, 1, 1}};
    const kt::HistogramParams hostParams{{values.data(), kCount},
                                         {hostGroups.data(), static_cast<u32>(hostGroups.size())},
                                         {hostHist.data(), kt::kHistogramBins}};
    const kt::HistogramParams deviceParams{{dValues.data(), kCount},
                                           {dGroups.data(), static_cast<u32>(hostGroups.size())},
                                           {dHist.data(), kt::kHistogramBins}};
    const kernel::ParityReport report = kernel::run_parity(
        kernel::Backend::CpuReference, kernel::Backend::Cuda, launch, kt::TiledHistogramKernel{}, hostParams,
        deviceParams,
        [&] {
            const bool copied = dGroups.download(devGroupsHost.data(), devGroupsHost.size(), nullptr) &&
                                dHist.download(devHistHost.data(), devHistHost.size(), nullptr) &&
                                cudaDeviceSynchronize() == cudaSuccess;
            kernel::ParityReport r =
                kernel::compare_bitwise(std::span<const u32>(hostGroups), std::span<const u32>(devGroupsHost));
            r.merge(kernel::compare_bitwise(std::span<const u32>(hostHist), std::span<const u32>(devHistHost)));
            r.launches_ok = r.launches_ok && copied;
            r.ok = r.launches_ok && r.mismatches == 0u;
            return r;
        },
        cudaOptions(kHistogramEntry));
    expectTrue(staged && report.ok, "device tiled histogram (__shared__ + __syncthreads phases) == CpuReference");
}

} // namespace

int main() {
    // Keep every instantiated entry referenced (the scan / wave entries are compile coverage only).
    const bool entriesPresent = kHashEntry != nullptr && kWaveEntry != nullptr && kHistogramEntry != nullptr &&
                                kScanEntry != nullptr && kAddOffsetsEntry != nullptr;
    expectTrue(entriesPresent, "device entries instantiated");
    if (!kernel::cuda::device_present()) {
        std::printf("fuse_compute_kernel_cuda_gate: no CUDA device — compile-only (device code built for every "
                    "test kernel)\n");
        return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    deviceHashParity();
    deviceHistogramParity();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }
    std::printf("fuse_compute_kernel_cuda_gate: device parity passed\n");
    return EXIT_SUCCESS;
}
