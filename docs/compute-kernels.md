# Compute kernels

FUSE writes GPU-style work **once**, as single-source kernels. The same kernel body runs serially on the CPU
now (reference), fanned out over the job system (production CPU path) and as a CUDA kernel when a device
exists. A Vulkan-compute backend has a reserved seam. Every launch is profiled and counted under the kernel's name.

The framework lives in `fuse_core` (`Source/FUSE/Core/include/fuse/compute_kernel/`), so every module can use it.
The reference port is the SDF ray march in `fuse_compute`
(`Source/FUSE/Compute/include/fuse/compute/ray_march_kernel.hpp`).

| Header | What it has |
| --- | --- |
| `kernel.hpp` | `Dim3`, `LaunchIndex`, `KernelLaunch`, `Backend`, `Span<T>`, `WorkgroupContext<T>`, traits, `DeviceEntryFn`. Safe to include from device code. |
| `atomics.hpp` | `scratch_atomic_add` and `global_atomic_add` (u32). Single-source and safe to include from device code. |
| `launch.hpp` | `kernel::launch(backend, launch, body, params, options)`, `LaunchOptions`, `LaunchResult`, `resolve_backend`. Host only. |
| `stats.hpp` | Per-kernel `KernelStats` registry, `LaunchRecord`, `last_launch()`, `backend_available()`, `backend_name()`. |
| `load_scale.hpp` | The `LoadScale` knob (resolution, objects, probes, lights), `FUSE_LOAD_SCALE`, `ScopedLoadScale`. |
| `parity.hpp` | `run_parity`, `compare_bitwise`, `compare_floats`, `ParityReport`, `Tolerance`. Host only. |
| `cuda_launch.cuh` | `__global__` trampolines, `cuda::entry<Body, Params>`, `cuda::DeviceBuffer<T>`. Include it from `.cu` files only. |

## The model

A **kernel body** is a trivially copyable functor. Its call operator is `FUSE_HOST_DEVICE`, and it takes a
`LaunchIndex` and a **params** struct. Params are POD: scalars, `kernel::Span<T>` and raw non-owning pointers.
Laying the data out as SoA spans works well on every backend.

```cpp
struct ScaleParams { kernel::Span<const f32> in; kernel::Span<f32> out; f32 k = 1.f; };

struct ScaleKernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const ScaleParams& p) const {
        p.out[idx.linear] = p.in[idx.linear] * p.k;
    }
};

const kernel::KernelLaunch desc{"scale", kernel::extent1(n), {64, 1, 1}};   // name, grid (items), workgroup
kernel::launch(kernel::Backend::CpuParallel, desc, ScaleKernel{}, ScaleParams{in, out, 2.f});
```

- `grid` counts **items**, not workgroups. `group_count(grid, workgroup)` workgroups cover it. Item kernels are
  never called for out-of-grid items.
- `LaunchIndex` gives `global`, `local`, `group` (all `Dim3`), `linear` (x fastest, then y, then z),
  `local_linear`, `grid`, `workgroup` and `active`.
- `name` must be a string literal. It is the profiler scope, the stats key and the name the GPU timestamp query
  uses for the same pass (for example `"sdf_ray_march"`, which matches `DeferredFramePipeline::passName`).
- Workgroups have at most `kMaxWorkgroupSize` (1024) threads. Keep kernels meant for the GPU at 256 or fewer,
  because Vulkan only guarantees 128.

### Backends

| Backend | Behaviour |
| --- | --- |
| `CpuReference` | Serial, in deterministic order. Item kernels visit items in linear order. Workgroup kernels visit workgroups in linear order. This is the ground truth for tests. |
| `CpuParallel` | `JobScheduler::parallel_for` over workgroups. The grain is whole workgroups, about 4 chunks per thread by default (`LaunchOptions::grain_workgroups`). It makes no heap allocations in steady state. |
| `Cuda` | `LaunchOptions::cuda`, which is a `cuda::entry<Body, Params>` instantiated in a `.cu` file. It is compiled only with `FUSE_HAS_CUDA` and used only when a device is present. Params must point at device memory. |
| `VulkanCompute` | A seam. It dispatches through `LaunchOptions::vulkan` when the caller supplies one. No backend ships yet. |
| `Auto` | `Cuda` if an entry was supplied and a device is available. Otherwise `CpuParallel`. |

A GPU backend that was requested but cannot run falls back to `CpuParallel`. If `allow_fallback = false`, the
launch fails instead. `LaunchResult::backend` and the stats both record the backend that actually ran.

The two CPU backends produce **bit-identical** results for any kernel whose items do not race. The same body is
compiled once per instantiation, with no reductions whose order depends on the backend. CUDA results match
within a float tolerance, because device `sqrt`, `sin` and similar functions differ from host libm by a few ulps.

### Workgroup kernels (shared memory and barriers)

For tiled algorithms, declare the workgroup form:

```cpp
struct TiledHistogramKernel {
    using Scratch = u32;                      // scratch element type (POD, alignment <= 16)
    static constexpr u32 kScratchCount = 256; // elements; total <= kMaxWorkgroupScratchBytes (16 KiB)
    static constexpr u32 kPhases = 3;         // bulk-synchronous phases
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx,
                                     const kernel::WorkgroupContext<u32>& wg, const HistogramParams& p) const {
        switch (wg.phase) { case 0: /* clear wg.scratch */; case 1: /* accumulate */; default: /* publish */; }
    }
};
```

Every local thread runs phase 0, then every thread runs phase 1, and so on. On CUDA the scratch is `__shared__`
memory, and phases are separated by `__syncthreads()`. On the CPU, the workgroup's scratch lives on the stack
and its threads run serially in phase-major order.

Follow these rules:

- **Locals do not survive a phase.** Keep anything that must carry over in scratch.
- **Scratch starts undefined.** Initialize it in phase 0.
- **Padding threads run too.** The threads of a partial edge workgroup are called with `idx.active == false`.
  They must take part in scratch protocols, for example by writing identity values, but must not touch
  grid-indexed memory.
- **Atomics.** Use `scratch_atomic_add` for scratch and `global_atomic_add` for memory shared between
  workgroups. Integer atomics are order-independent, so results stay bit-exact. There are no float atomics:
  reduce in scratch instead.
- **Multi-pass algorithms** (a scan larger than one workgroup, for example) are a sequence of launches, just as
  on the GPU. `test_compute_kernel_gates.cpp` does block scan, then a scan of the block sums, then adds the
  offsets.

### Profiling and load scale

- Each `launch()` opens `FUSE_PROFILE_SCOPE(launch.name)` and records a `LaunchRecord` with the name, the
  requested and executed backend, items, workgroups, duration and success.
- Records aggregate per name into `KernelStats`. Query them with `find_kernel_stats`, `kernel_stats_at`,
  `last_launch` or `total_launch_count`. The registry is fixed-capacity and never allocates.
- `kernel::load_scale()` returns the global knob `{resolution, objects, probes, lights}`. Demo, benchmark and
  kernel host code multiplies its workload with `scaled_extent` / `scaled_count`. Set the knob with
  `set_load_scale`, with `ScopedLoadScale`, or with the environment (`FUSE_LOAD_SCALE="res=0.5,objects=4"` or
  `FUSE_LOAD_SCALE=2`) plus `load_scale_from_env()`.

### Parity harness

```cpp
const kernel::ParityReport r = kernel::run_parity(kernel::Backend::CpuReference, kernel::Backend::CpuParallel,
    desc, MyKernel{}, paramsA, paramsB,   // same params except the output pointers
    [&] { return kernel::compare_bitwise(std::span<const u32>(outA), std::span<const u32>(outB)); });
```

Use `compare_floats(a, b, {abs, rel})` for float outputs against a GPU. `T` can be `f32` or an aggregate of
floats such as `math::Vec4`. `ParityReport::merge` folds together several output surfaces.

## Porting a kernel

This is the checklist the ray march followed:

1. **Move the math into a device-safe header** (`<module>/include/fuse/<module>/<kernel>_kernel.hpp`). Mark
   every helper `FUSE_HOST_DEVICE inline`. Allowed: `fuse/math/*`, `fuse/types.hpp`, `std::min`, `std::max`,
   `std::clamp` (through `--expt-relaxed-constexpr`), and the `<cmath>` float functions. Not allowed:
   allocation, virtual calls, `std::vector`, `std::function`, exceptions, or globals.
2. **Define `Params`.** Use POD with `Span`s and pointers, and resolve per-launch constants on the host (the
   camera basis and `tan(fov/2)` in the ray march). Add `params_valid()` and `make_launch()`, and add
   `inline constexpr const char* kName` using the render-graph pass name.
3. **Write the body** as a `struct Kernel { FUSE_HOST_DEVICE void operator()(const LaunchIndex&, const Params&) const; }`.
   For shared memory, use the workgroup form.
4. **Replace the CPU implementation.** The old CPU loop becomes
   `kernel::launch(backend, make_launch(...), Kernel{}, make_params(...))`. Keep any public scalar helpers as
   thin wrappers over the header functions, and **delete the duplicated logic**.
5. **Write the CUDA wrapper** in the module's `.cu` file: stage inputs and outputs with `cuda::DeviceBuffer`,
   launch with `LaunchOptions{.cuda = &kernel::cuda::entry<Kernel, Params>, .allow_fallback = false}`, then read
   back. Host code calls it only when `kernel::backend_available(Backend::Cuda)`. Otherwise it goes through
   `kernel::launch` so the fallback is recorded. Add every new `.cu` file to `fuse_cuda_compile_gate` in
   `Source/FUSE/CMakeLists.txt`.
6. **Add tests** (with a `gate` label): a CpuReference vs CpuParallel bit-exact parity check at 0, 2 and 4
   workers on a grid with partial edge workgroups; a check that the stats name and item count are correct; and
   a check that a GPU request without a device falls back. Keep the existing tests green.

The ray march is split across these files:

- `Compute/include/fuse/compute/ray_march_kernel.hpp` holds the body and all the math.
- `Compute/src/ray_march_cpu.cpp` holds the public wrappers and the CPU launches.
- `Compute/src/ray_march_host.cpp` does the Auto/Cuda routing.
- `Compute/kernels/ray_march.cu` stages data on the device and launches.
- `Compute/tests/test_ray_march_kernel_parity.cpp` is the gate.

### Notes on the remaining kernels

| Kernel | Form | Notes |
| --- | --- | --- |
| SSAO, SSR, SSGI (ported: `ScreenSpace/include/fuse/ssfx/{hbao,ssr,ssgi}_kernel.hpp`, `Compute/include/fuse/compute/screen_space_kernels.hpp`) | Item, 8x8 | One pixel per item: `screen_space_ao`, then `screen_space_ao_blur` as a separate launch; `screen_space_reflections`; `screen_space_gi` once per bounce (radiance ping-pong driven by `ssgi_kernel::run_bounces`, shared by CPU and CUDA). Noise is an integer hash, so CPU parity is bit-exact. Gate: `fuse_screen_space_kernel_parity`. |
| DDGI probe update | Workgroup | One workgroup per probe. Rays go into scratch in phase 0, then irradiance and depth texels are blended in phase 1 (one thread per texel), then border texels are copied in phase 2. Scale probe counts with `LoadScale::probes`. |
| Clustered light cull and shade | Item (cull), item (shade) | Cull: one item per cluster writes a fixed-capacity light list, with no atomics. For a compacted list, use `global_atomic_add` on a u32 counter and sort for determinism, or a scan (block-scan pattern). Scale with `LoadScale::lights`. |
| SDF shadows | Item | Reuse `ray_march_kernel::scene_eval` for soft-shadow cone tracing. Don't copy the SDF code. |
| Skinning | Item | **Ported:** `Animation/include/fuse/animation/skinning_kernel.hpp` (`skinning_lbs`, 128-wide), `Animation/kernels/skinning.cu`, gate `fuse_skinning_kernel_parity`. One vertex per item; the bone palette is a `Span<const mat4>`. |
| Particles | Item (update), workgroup (compaction or sort) | **Ported:** `VFX/include/fuse/vfx/particle_sim_kernel.hpp` (`particle_update` item kernel with `global_atomic_add` per-workgroup dead counts, then `particle_compact` workgroup kernel: count, scan, write a descending dead list), `VFX/kernels/particle_sim.cu`, gate `fuse_particle_kernel_parity`. The AFX pool integrate is `Modules/fx/include/fuse/fx/particle_pool_kernel.hpp` (gate `fuse_fx_particle_pool_kernel_parity`). |
| Physics broadphase (ported) | Item + workgroup | `Physics/include/fuse/physics/broadphase/broadphase_kernel.hpp`: cell keys per shape (count, scan, write), a stable 4-bit LSD radix sort (tile histograms, reduce-then-scan, stable raked scatter; also a `BroadphaseKeyValueSorter` via `KernelRadixSorter`), cell runs, pair emission (count, scan, write), dedupe. Bit-identical to `runBroadphaseIntoBuffer`. CUDA entries in `Physics/kernels/physics_broadphase.cu`. |
| Physics narrowphase (ported) | Item | `narrowphase_kernels.hpp`: one pair per item into its staging slot, scan of the contact counts, then a copy into the scanned contact slot. Bit-identical to `runNarrowphaseIntoBuffer`. CPU backends only until the shape-pair math is `FUSE_HOST_DEVICE`. |
| Physics solver (ported, opt-in) | Item per color | `ConstraintSolveMode::ColoredKernel`: greedy colouring on the host over dynamic bodies, one `physics_solve_color` launch per colour and iteration, with no float atomics. It is deterministic on every CPU backend, but it uses a different Gauss-Seidel order from the default island solver, so it stays opt-in. |
| SVO ray cast | Item | **Ported** (`Scene/include/fuse/scene/svo_ray_kernel.hpp`, `"svo_ray_cast"`, `SVO::rayCastBatch`). One ray per item over an `SvoView` of flat node / brick / payload spans. A fixed-size `TraversalStack` (no recursion) restarts each octree walk from the deepest node that still contains the voxel. `SVO::get` and `SVO::rayCast` call the same header functions. |
| FFT reverb | Workgroup + item | **Ported** (`Audio/include/fuse/audio/reverb_fft_kernel.hpp`, `"reverb_fft"`, `"reverb_cmac"`). A uniformly partitioned overlap-save with a frequency-domain delay line. `reverb_fft` runs one workgroup per transform: phase 0 loads the bit-reversed window, then one butterfly stage per phase, then a store phase (`kPhases = kMaxLog2N + 2`, and stages past `log2(N)` idle). `reverb_cmac` runs one bin per item. N is at most 2048 (16 KiB scratch). |

The GLSL/SPIR-V side of `VulkanCompute` is not generated. A future backend supplies a `DeviceEntryFn` that
records a dispatch of a hand-written or translated pipeline named `launch.name`, with the params in an SSBO or
push constants. The C++ body stays the reference that its output is checked against with `run_parity`.
