// CUDA backend of the partitioned FFT reverb: the __global__ trampolines from cuda_launch.cuh run the
// same FUSE_HOST_DEVICE bodies (fuse/audio/reverb_fft_kernel.hpp) the CPU backends run. This TU keeps a
// device mirror of the plan (twiddles, IR spectra) and of the streaming state (input ring, delay line,
// accumulator); the host resolves the schedule (stale windows, head) exactly as for the CPU path.
// Everything is allocated in reverb_fft_device_create, so steady-state blocks do not allocate.

#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/audio/reverb_fft_kernel.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>

#include <cuda_runtime.h>

namespace fuse::audio {

struct ReverbFftDevice {
    reverb_fft::Plan plan{}; ///< spans re-pointed at device memory
    cudaStream_t stream = nullptr;
    kernel::cuda::DeviceBuffer<reverb_fft::Complex> twiddles;
    kernel::cuda::DeviceBuffer<reverb_fft::Complex> irSpectra;
    kernel::cuda::DeviceBuffer<reverb_fft::Complex> spectra;
    kernel::cuda::DeviceBuffer<reverb_fft::Complex> accum;
    kernel::cuda::DeviceBuffer<f32> ring;
    kernel::cuda::DeviceBuffer<f32> output;
    kernel::cuda::DeviceBuffer<u32> staleStart;
    kernel::cuda::DeviceBuffer<u32> staleSlot;
};

ReverbFftDevice* reverb_fft_device_create(const reverb_fft::Plan& plan, void* stream) {
    ReverbFftDevice* device = new ReverbFftDevice{};
    device->stream = static_cast<cudaStream_t>(stream);
    const usize bins = static_cast<usize>(plan.partitions) * plan.n;
    bool ok = device->twiddles.allocate(plan.twiddles.size) && device->irSpectra.allocate(bins) &&
              device->spectra.allocate(bins) && device->accum.allocate(plan.n) &&
              device->ring.allocate(plan.ring_size) && device->output.allocate(plan.partition) &&
              device->staleStart.allocate(plan.partitions) && device->staleSlot.allocate(plan.partitions);
    ok = ok && device->twiddles.upload(plan.twiddles.data, plan.twiddles.size, device->stream) &&
         device->irSpectra.upload(plan.ir_spectra.data, bins, device->stream) &&
         cudaMemsetAsync(device->ring.data(), 0, plan.ring_size * sizeof(f32), device->stream) == cudaSuccess &&
         cudaStreamSynchronize(device->stream) == cudaSuccess;
    if (!ok) {
        delete device;
        return nullptr;
    }
    device->plan = plan;
    device->plan.twiddles = kernel::make_span<const reverb_fft::Complex>(device->twiddles.data(), plan.twiddles.size);
    device->plan.ir_spectra = kernel::make_span<const reverb_fft::Complex>(device->irSpectra.data(),
                                                                           static_cast<u32>(bins));
    return device;
}

void reverb_fft_device_destroy(ReverbFftDevice* device) {
    delete device;
}

void reverb_fft_device_reset(ReverbFftDevice* device) {
    (void)cudaMemsetAsync(device->ring.data(), 0, device->plan.ring_size * sizeof(f32), device->stream);
    (void)cudaStreamSynchronize(device->stream);
}

bool reverb_fft_device_block(ReverbFftDevice* device, const reverb_fft::Block& block) {
    namespace rf = reverb_fft;
    const rf::Plan& plan = device->plan;
    const cudaStream_t stream = device->stream;
    const u32 stale = block.stale_start.size;

    // Append the block to the device ring (at most two contiguous pieces).
    const u32 mask = plan.ring_size - 1u;
    const u32 first = static_cast<u32>(block.time) & mask;
    const u32 head = block.frames < plan.ring_size - first ? block.frames : plan.ring_size - first;
    bool ok = cudaMemcpyAsync(device->ring.data() + first, block.input, head * sizeof(f32), cudaMemcpyHostToDevice,
                              stream) == cudaSuccess;
    if (ok && head < block.frames) {
        ok = cudaMemcpyAsync(device->ring.data(), block.input + head, (block.frames - head) * sizeof(f32),
                             cudaMemcpyHostToDevice, stream) == cudaSuccess;
    }
    ok = ok && device->staleStart.upload(block.stale_start.data, stale, stream) &&
         device->staleSlot.upload(block.stale_slot.data, stale, stream);
    if (!ok) {
        return false;
    }

    kernel::LaunchOptions options{};
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = false; // one sync after the read-back

    const kernel::Span<rf::Complex> spectra =
        kernel::make_span(device->spectra.data(), static_cast<u32>(device->spectra.size()));
    const kernel::Span<rf::Complex> accum = kernel::make_span(device->accum.data(), plan.n);
    if (stale > 0u) {
        options.cuda = &kernel::cuda::entry<rf::FftKernel, rf::FftParams>;
        ok = kernel::launch(kernel::Backend::Cuda, rf::make_fft_launch(plan.n, stale), rf::FftKernel{},
                            rf::make_window_params(
                                plan, kernel::make_span<const f32>(device->ring.data(), plan.ring_size),
                                kernel::make_span<const u32>(device->staleStart.data(), stale),
                                kernel::make_span<const u32>(device->staleSlot.data(), stale), spectra),
                            options)
                 .ok;
    }
    options.cuda = &kernel::cuda::entry<rf::CmacKernel, rf::CmacParams>;
    ok = ok && kernel::launch(kernel::Backend::Cuda, rf::make_cmac_launch(plan.n), rf::CmacKernel{},
                              rf::make_cmac_params(plan, block,
                                                   kernel::make_span<const rf::Complex>(spectra.data, spectra.size),
                                                   accum),
                              options)
                   .ok;
    options.cuda = &kernel::cuda::entry<rf::FftKernel, rf::FftParams>;
    ok = ok && kernel::launch(kernel::Backend::Cuda, rf::make_fft_launch(plan.n, 1u), rf::FftKernel{},
                              rf::make_inverse_params(plan, block,
                                                      kernel::make_span<const rf::Complex>(accum.data, plan.n),
                                                      kernel::make_span(device->output.data(), block.frames)),
                              options)
                   .ok;
    ok = ok && device->output.download(block.output, block.frames, stream);
    return ok && cudaStreamSynchronize(stream) == cudaSuccess;
}

} // namespace fuse::audio
