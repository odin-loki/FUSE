# Track B — Vulkan Bootstrap (B2.1–B2.10) + CUDA Ray March (B2.7)

**Status:** B2.1 bootstrap + B2.2 swapchain/frame ring + B2.3 resource/bindless scaffolding + B2.4 shader scaffold + B2.5 command buffer / render graph scaffolding + B2.6 CUDA/interop stubs + B2.7 SDF ray-march CUDA path scaffolding + B2.8 rasterisation pipeline scaffold + B2.9 composite pass scaffold + B2.10 renderer init & main-loop glue + **B2.11 Phase 2 deliverables & integration test suite**  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B2.1–B2.5, §B2.6, §B2.7, §B2.8, §B2.9, §B2.10  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §4.2, §4.4, §5.3  
**Hybrid integration:** [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `RenderCommandList` | `Source/FUSE/Renderer/` | Per-frame draws/clears merged on render thread |
| `VulkanInstance` / `VulkanDevice` | `Source/FUSE/Renderer/include/fuse/renderer/vk/` | Headless bootstrap; optional validation layers |
| `VulkanSurface` | same | Headless vs external `VkSurfaceKHR` abstraction |
| `PlatformWindow` | `Source/FUSE/Core/include/fuse/platform/window.hpp` | Null (CI) or optional GLFW desktop window stub |
| `VulkanPresentable` | `Source/FUSE/Hybrid/include/fuse/hybrid/vulkan_presentable.hpp` | Own Hybrid presentable path — wires platform window → External `VulkanSurface` |
| `VulkanSwapchain` | same | Real `VkSwapchainKHR` when External surface + WSI; headless stub otherwise |
| `FrameManager` | same | Triple-buffered fence/semaphore ring aligned with `FrameBarrier` |
| `ResourceManager` / `GpuAllocator` | `Source/FUSE/Renderer/` | Handle-based buffers/images; VMA when vendored, stub otherwise |
| `BindlessDescriptors` | `Source/FUSE/Renderer/include/fuse/renderer/vk/` | Index table scaffolding (descriptor pool deferred to B2.4 follow-up) |
| `HandleMap<T>` | `Source/FUSE/Core/include/fuse/` | Generation-checked slots for GPU resources |
| `ShaderCompiler` / `ShaderModule` | `Source/FUSE/Renderer/include/fuse/renderer/shader/` | Offline-first — loads checked-in `.spv` fixtures |
| `PipelineLayout` | `Source/FUSE/Renderer/include/fuse/renderer/vk/pipeline_layout.hpp` | Placeholder layout (push constants only; bindless sets deferred) |
| `CommandBufferRecorder` | `Source/FUSE/Renderer/` | Stub logical command recording (pass/barrier/clear/draw/present) |
| `RenderGraph` | `Source/FUSE/Renderer/` | Pass dependency sketch, barrier planning, culling |
| `RenderPass` / `GraphicsPipeline` | `Source/FUSE/Renderer/include/fuse/renderer/vk/` | B2.8 headless raster scaffold — `VkPipeline` from B2.4 fixtures |
| `RasterPath` | same | Offscreen clear + triangle stub wired through `RhiContext::submitFrame` |
| `CompositePass` | `composite_pass.hpp` | B2.9 stub — merges raster + CUDA textures into backbuffer before present |
| `RhiContext` | `Source/FUSE/Renderer/` | `beginFrame` / `submitFrame` on `renderThread()`; compiles graph per frame + optional `RasterPath` + `CompositePass` |
| `RendererBootstrap` | `Source/FUSE/Renderer/include/fuse/renderer/renderer_bootstrap.hpp` | B2.10 single init path for `RhiContext` + `FrameManager` |
| `HybridRendererBootstrap` | `Source/FUSE/Hybrid/include/fuse/hybrid/hybrid_renderer_bootstrap.hpp` | B2.10 runtime glue — wires `RendererBootstrap` into `HybridComposer` (`FUSE_BUILD_VULKAN` only) |
| `fuse::platform::gl_context.hpp` | `Source/FUSE/Core/` | Portable “may touch GPU” guard |
| `HybridComposer` wiring | `Source/FUSE/Hybrid/` | Dual path: software `PlaceholderRenderer` **and** RHI command mirror |
| `CUDAJobDesc` / `submit_cuda` | `Source/FUSE/Core/include/fuse/jobs/cuda_jobs.hpp` | Job-lane CUDA dispatch via `JobScheduler` |
| `import_vulkan_buffer` / `import_vulkan_image` | `Source/FUSE/Renderer/include/fuse/renderer/cuda/` | External-memory import deferred — returns `ok=false` until full B2.6 |
| `StreamManager` | `Source/FUSE/Renderer/include/fuse/renderer/cuda/` | Named streams (Render, Physics, AI, Particles, Upload) |
| `RayMarchParams` / `submit_ray_march_job` | `Source/FUSE/Compute/` | SDF ray-march pass wired through `submit_cuda` (`fuse_compute`) |
| `fuse::math::Vec3` / `SDF::sphere` | `Source/FUSE/Core/include/fuse/math/` | Host-side math for CPU reference tracer |

**Not in scope:** Engine marriage, real present in CI (no window surface), MoltenVK/Android surface wiring, full bindless descriptor pool, graphics pipeline cache, hot-reload watchers, full B2.6 shared-texture interop, render-graph CUDA node execution (B2.5 API only).

---

## Build flag — `FUSE_BUILD_VULKAN`

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_VULKAN=ON
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_VULKAN=OFF` | No `fuse_rhi` target; Hybrid stays software-only (U4 path unchanged) |
| `FUSE_BUILD_VULKAN=ON`, loader found | `FUSE_VULKAN_BACKEND=1` — real `VkInstance` / `VkDevice` when ICD present |
| `FUSE_BUILD_VULKAN=ON`, loader missing | Stub backend — tests assert graceful degradation |
| CI Linux | Installs `libvulkan-dev` + Mesa Lavapipe; validation layers optional |
| CI Android | `FUSE_BUILD_VULKAN=OFF` until NDK surface bootstrap lands |

CMake discovers Vulkan quietly in `Source/FUSE/CMakeLists.txt`; `fuse_rhi` always builds when the option is ON.

---

## Thread ownership (locked)

Only `fuse::platform::renderThread()` may:

- Create/destroy GPU contexts (v1 bootstrap on same thread as Hybrid `render()`)
- Call `RhiContext::beginFrame()` / `submitFrame()`

Workers produce snapshot SOA / staging data only. Job code never includes `<vulkan/vulkan.h>`.

`HybridComposer::render()`:

1. Assert render thread (`platform::isRenderThread()`)
2. Execute software placeholder (existing U4 tests)
3. When `FUSE_HAS_VULKAN_RHI`: reset `RenderCommandList`, mirror clears/sprites, `beginFrame(ctx.frameIndex)`, `submitFrame()` (builds + compiles `RenderGraph`, records stub commands)

**Frame barrier alignment:** `FrameBarrier` at end of tick ensures jobs for frame `N` complete before render record. `FrameManager::signalTickComplete()` + `beginFrame(frameIndex)` mirror that sync point on the GPU path.

---

## Backend modes

```cpp
enum class VulkanBackendMode : u8 { Stub, Headless };
```

| Mode | When | Instance | Device | Swapchain |
|------|------|----------|--------|-----------|
| **Stub** | No loader / no ICD | — | — | — |
| **Headless** | Loader + ICD (incl. Lavapipe), no surface | `VkInstance` | `VkDevice` + queues | stub (records desc, no `VkSwapchainKHR`) |
| **Presentable** | External `VkSurfaceKHR` + `VK_KHR_swapchain` | `VkInstance` + WSI ext* | `VkDevice` | real `VkSwapchainKHR` |

\*WSI instance extensions (`VK_KHR_surface`, platform surface) must be enabled by the platform module that creates the surface — not yet wired in CI.

`GpuAllocator` creates a real `VmaAllocator` when `third_party/VulkanMemoryAllocator/include/vk_mem_alloc.h` is present; otherwise stub bookkeeping handles are issued and `VulkanDeviceInfo::vmaAllocator` stays null until VMA is vendored.

---

## Surface abstraction (B2.2)

```cpp
enum class SurfaceKind : u8 { Headless, External };
```

| Kind | `nativeSurface` | Swapchain behaviour |
|------|-----------------|---------------------|
| **Headless** | `nullptr` | Records width/height/imageCount; **no** `VkSwapchainKHR` (CI / Lavapipe default) |
| **External** | opaque `VkSurfaceKHR*` | Creates real swapchain when device has `VK_KHR_swapchain` and queue supports present |

Headless is intentional for CI: Lavapipe provides an ICD but umbrella tests run without a window. Editor Qt viewport (`U6`) will pass `SurfaceKind::External`.

### Presentable path stubs (B2.2 follow-up)

| Component | Backend | Behaviour |
|-----------|---------|-----------|
| `PlatformWindow` | **Null** (default) | No OS window; CI / Lavapipe headless |
| `PlatformWindow` | **GLFW** (`FUSE_PLATFORM_WINDOW_GLFW=ON`) | Hidden desktop window; `glfwGetRequiredInstanceExtensions` + `glfwCreateWindowSurface` |
| `VulkanPresentable` | **Headless** | No window; `SurfaceKind::Headless` |
| `VulkanPresentable` | **PlatformWindow** | Defers swapchain until `VkSurfaceKHR` exists; wires `SurfaceKind::External` |
| `HybridRendererBootstrap` | either | Owns `VulkanPresentable`; populates `SwapchainDesc.surface` + WSI instance extensions |
| `PresentPath` | headless or WSI | Acquire / present / fence-wait state machine over `FrameManager` + `VulkanSwapchain` |
| `VsyncMode` | `Fifo` / `Mailbox` / `Immediate` | Maps to `VkPresentModeKHR`; default `Fifo` for CI |

#### Present path state machine (B2.2 deepen)

CPU-side lifecycle exercised without a real GPU window:

```
Idle → waitInFlightFence → FenceWaited → acquireImage → ImageAcquired
     → presentImage → Presented → Idle
ResizePending → (fence wait) → recreateSwapchain → Idle
```

| API | Headless CI behaviour |
|-----|----------------------|
| `FrameManager::waitInFlightFence` | Slot bookkeeping; real `vkWaitForFences` when backend active |
| `PresentPath::acquireImage` | Returns `UINT32_MAX`; advances state |
| `PresentPath::presentImage` | Succeeds without `vkQueuePresentKHR` |
| `PresentPath::requestResize` | Records dimensions; `rebuild()` on next fence wait |
| `VulkanPresentable::requestResize` | Queues resize; `HybridRendererBootstrap::render` forwards to `PresentPath` |

```bash
# Optional local GLFW window bootstrap (not used in CI):
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_VULKAN=ON -DFUSE_PLATFORM_WINDOW_GLFW=ON
```

Android CI keeps `FUSE_BUILD_VULKAN=OFF` — `VulkanPresentable` and `HybridRendererBootstrap` Vulkan sources are not compiled into `fuse_hybrid`.

---

## Frame ring (B2.2 sketch)

`kFramesInFlight = 3` — triple-buffered slot ring:

| Per-slot sync | Role |
|---------------|------|
| `image_available` | swapchain acquire signal (used when presentable) |
| `render_finished` | present wait |
| `in_flight_fence` | CPU wait before reusing slot |

Lifecycle per frame:

1. Tick completes → `FrameBarrier::signalTickJobsComplete()` (game thread)
2. Render thread → `FrameManager::signalTickComplete()` + `beginFrame(frameIndex)` (waits prior fence)
3. Record `RenderCommandList` + placeholder software path
4. `endFrame()` advances ring index

Per-slot command pools and primary command buffers are allocated when the Vulkan backend is active (B2.5). Descriptor pools and scratch allocators remain deferred.

---

## B2.5 — Command buffer & render graph scaffolding

**Status:** CPU-side graph compile + stub command recording landed; real `vkCmd*` wiring deferred to B2.8+.

| Component | Location | Notes |
|-----------|----------|-------|
| `CommandBufferRecorder` | `include/fuse/renderer/command_buffer.hpp` | Records logical pass/barrier/clear/draw/present commands for tests |
| `RenderGraph` | `include/fuse/renderer/render_graph.hpp` | Pass nodes declare texture/buffer accesses; `compile()` plans barriers + culls unused passes |
| `populateRenderGraphFromCommandList` | `render_graph.cpp` | Maps `RenderCommandList` clears/sprites → graph passes (clear → sprites2d → composite → present) |
| `FrameCommandData` | `vk/frame.hpp` | Per-slot `VkCommandPool` + primary `VkCommandBuffer` when backend active |
| `RhiContext` integration | `rhi_context.cpp` | `submitFrame()` populates graph, compiles, executes into recorder, advances frame ring |

### B2.5 follow-up — pass dependency edges & resource lifetimes (stub)

`RenderGraph::compile()` now builds a directed dependency graph before barrier planning:

| Piece | API | Behaviour |
|-------|-----|-----------|
| Explicit edges | `addPassDependency(from, to)` | Producer pass must finish before consumer pass |
| Resource edges | `RGPassDependencyKind::ResourceAccess` | Chains passes that touch the same texture/buffer in declaration order |
| Compile order | `compileOrder()` | Topological sort of non-culled passes; falls back to declaration order on cycles |
| Lifetime stubs | `resourceLifetimes()` | CPU-side `firstPassIndex` / `lastPassIndex` per resource (`Imported` vs `TransientCreated`) |

GPU release / alias reuse remains deferred — lifetimes are planning metadata only. `fuse_render_graph` covers explicit reordering, resource-edge derivation, and transient lifetime tagging.

### Render graph API (sketch)

```cpp
enum class RGResourceAccess : u32 {
  ColorAttachmentWrite, DepthAttachmentWrite, ShaderRead, ShaderWrite,
  TransferSrc, TransferDst, Present, CUDAWrite, CUDARead,
};

struct RGPassDesc {
  const char* name;
  RGPassExecuteFn execute;          // void(*)(void* cmd, void* userData)
  const RGTextureAccess* textureAccesses;
  u32 textureAccessCount;
  bool is_compute = false;
  bool is_cuda = false;
};

class RenderGraph {
  void beginFrame(u32 backbufferIndex);
  void addPass(const RGPassDesc& desc);
  void compile();                   // barrier planning + pass culling
  RenderGraphExecuteInfo execute(VulkanDevice&, FrameManager&, CommandBufferRecorder&);
};
```

CUDA nodes (`is_cuda`) are first-class in the API; graph execution remains a no-op stub until B2.6 timeline wiring lands.

### Per-frame flow (B2.5)

1. `HybridComposer::render()` mirrors draws into `RenderCommandList` (unchanged U4 software path).
2. `RhiContext::beginFrame(frameIndex)` → `FrameManager::beginFrame` + `RenderGraph::beginFrame`.
3. `populateRenderGraphFromCommandList()` builds passes from mirrored commands.
4. `RenderGraph::compile()` inserts layout-transition barriers (CPU plan only).
5. `RenderGraph::execute()` records logical commands via `CommandBufferRecorder` using the current slot's primary command buffer handle.
6. `FrameManager::endFrame()` advances the ring.

**Gates:** no per-frame heap allocs in the hot path (`kMaxPassesPerFrame` fixed storage for hybrid mirror); no manual `vkCmdPipelineBarrier` outside graph planning (barriers are graph-owned, even when recording is stubbed).

---

## B2.3 — Resources & bindless scaffolding

| Piece | Path | Behaviour |
|-------|------|-----------|
| Typed handles | `resources.hpp` | `TextureHandle`, `BufferHandle`, `SamplerHandle` |
| `GpuAllocator` | `vk/allocator.hpp` | VMA path when header vendored; stub IDs + byte bookkeeping otherwise |
| `GpuAllocStats` | `vk/gpu_alloc_stats.hpp` | Buffer/image counters; VMA pool snapshot via `refreshVmaPoolStats()` |
| `ResourceManager` | `resource_manager.hpp` | Create/destroy + bindless index assignment; ordered destroy-all |
| `BindlessDescriptors` | `vk/bindless.hpp` | Free-list indices only until descriptor pool lands |

**B2.3 deepen:** [TRACK-B-RHI.md](./TRACK-B-RHI.md) — stub alloc stats, destroy-order teardown, `fuse_rhi_resource_destroy_order` tests.

Optional VMA: place [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) at `third_party/VulkanMemoryAllocator/include/vk_mem_alloc.h` and reconfigure.

---

## B2.4 — Shader system & pipeline compiler (scaffold)

**Status:** Offline SPIR-V load + `VkShaderModule` stub + pipeline layout placeholders landed.

| Component | Location | Notes |
|-----------|----------|-------|
| `ShaderCompiler` | `Source/FUSE/Renderer/include/fuse/renderer/shader/` | Offline-first — loads checked-in `.spv` fixtures |
| `ShaderModule` | same | Creates `VkShaderModule` when `FUSE_VULKAN_BACKEND=1` and device ready |
| `PipelineLayout` | `Source/FUSE/Renderer/include/fuse/renderer/vk/pipeline_layout.hpp` | Placeholder layout (push constants only; bindless sets deferred) |
| Fixtures | `Source/FUSE/Renderer/shaders/fixtures/` | `minimal.vert` / `minimal.frag` + precompiled `.spv` for CI |

### Offline SPIR-V path (CI default)

CI does **not** require glslang. Tests load `minimal.vert.spv` / `minimal.frag.spv` checked into the repo (sibling naming: `source.glsl` → `source.glsl.spv`).

```bash
# Regenerate fixtures locally when GLSL changes (developer machine only):
glslangValidator -V Source/FUSE/Renderer/shaders/fixtures/minimal.vert \
  -o Source/FUSE/Renderer/shaders/fixtures/minimal.vert.spv
spirv-val Source/FUSE/Renderer/shaders/fixtures/minimal.vert.spv
```

Optional runtime glslang (off by default):

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_VULKAN=ON -DFUSE_SHADER_GLSLANG=ON
```

When `FUSE_SHADER_GLSLANG=ON` but glslang is missing, configure continues with offline SPIR-V only.

---

## B2.7 — SDF Ray Marcher (CUDA scaffold)

**Status:** API + job wiring landed in `fuse_compute`; full kernel and B2.6 interop deferred.

| Component | Location | Notes |
|-----------|----------|-------|
| `RayMarchParams` | `Compute/include/fuse/compute/ray_march.hpp` | Camera, march settings, GPU-resident `SdfObject` array |
| `launch_ray_march` | `Compute/src/ray_march_host.cpp` | Host launcher — CPU reference or CUDA stub kernel |
| `submit_ray_march_job` | `Compute/include/fuse/compute/ray_march_job.hpp` | Wraps `launch_ray_march` in `CUDAJobDesc` → `submit_cuda` |
| `ray_march.cu` | `Compute/kernels/` | Placeholder `__global__` kernel when toolkit present |
| `ray_march_center_hit_distance` | `Compute/src/ray_march_cpu.cpp` | CPU sphere-tracer for unit tests |

### Backend modes

```cpp
enum class RayMarcherMode : u8 { Stub, CpuReference, Cuda };
```

| Mode | When | Behaviour |
|------|------|-----------|
| **CpuReference** | `FUSE_BUILD_CUDA=ON`, no toolkit | Host sphere-tracing against `fuse::math::SDF` primitives |
| **Cuda** | Toolkit found (`FUSE_HAS_CUDA=1`) | Launches placeholder kernel via B2.6 managed stream |
| **Stub** | `FUSE_BUILD_CUDA=OFF` | `fuse_compute` not built |

Job workers call `submit_cuda` / `submit_ray_march_job`; they never include `<cuda_runtime.h>`.

---

## B2.8 — Rasterisation pipeline (scaffold)

**Status:** Headless `VkGraphicsPipeline` + clear/triangle path stub landed.

| Component | Location | Notes |
|-----------|----------|-------|
| `RenderPass` | `Source/FUSE/Renderer/include/fuse/renderer/vk/render_pass.hpp` | Single color attachment for headless targets |
| `GraphicsPipeline` | `graphics_pipeline.hpp` | Built from B2.4 `ShaderModule` + `PipelineLayout` + `RenderPass` |
| `RasterPath` | `raster_path.hpp` | Offscreen image/framebuffer; records clear + one triangle draw per frame |
| `RhiContext` wiring | `rhi_context.hpp` | Lazy `RasterPath` creation; `submitFrame` mirrors `RenderCommandList` clears |

CI exercises the path headlessly (no `VkSurfaceKHR`). Full G-buffer layout, draw lists, and CUDA depth handoff remain future B2.8+ / B2.6 work.

---

## B2.9 — Composite pass (scaffold)

**Status:** Logical composite pass + render-graph node landed; real bindless composite shader deferred.

| Component | Location | Notes |
|-----------|----------|-------|
| `CompositePass` | `include/fuse/renderer/composite_pass.hpp` | Stub records GRIA blend; lazy-created via `RhiContext` |
| `addCompositePassToGraph` | `composite_pass.cpp` | Inserts `composite` pass before `present` — reads raster + CUDA transients, writes backbuffer |
| `CommandBufferRecorder::composite` | `command_buffer.hpp` | Logical composite command for tests |
| `RhiContext` wiring | `rhi_context.cpp` | `enableCompositePass` + `CompositePassDesc::defaultBlend`; stats via `lastCompositeStats()` |

### Hybrid ordering alignment

`HybridComposer::render()` mirrors **3D clear → 2D sprites → UI overlay** into `RenderCommandList`. The render graph maps that to:

1. `clear3d` — 3D background
2. `sprites2d` — 2D scene + UI overlay draws
3. `composite` — blend raster (B2.8) + CUDA (B2.7) into backbuffer (`mix(cuda, raster, blend)` per P2 §2.9)
4. `present` — swapchain / headless sink

No Hybrid code changes required: `submitFrame()` owns graph population and composite recording.

---

## B2.10 — Renderer init & main-loop glue

**Status:** Single init path for `fuse_rhi` + `FrameManager` + `HybridComposer`; lifecycle documented; init/shutdown order tests landed.

| Component | Location | Notes |
|-----------|----------|-------|
| `RendererBootstrap` | `include/fuse/renderer/renderer_bootstrap.hpp` | Creates `RhiContext` (which owns `VulkanBootstrap` + `FrameManager`) on the render thread |
| `HybridRendererBootstrap` | `include/fuse/hybrid/hybrid_renderer_bootstrap.hpp` | Owns `RendererBootstrap` + `HybridComposer`; shares one `RhiContext` via `setSharedRhiContext` |
| `HybridComposer::setSharedRhiContext` | `hybrid_composer.hpp` | Injected context from bootstrap; lazy owned context remains for legacy tests |

### Lifecycle (init order)

1. `fuse::core::initialize()` — job scheduler + `platform::registerRenderThread()`
2. `RendererBootstrap::create(desc)` — allocates `RhiContext` → `VulkanBootstrap` → `FrameManager` (+ optional `RasterPath` on first submit)
3. `HybridRendererBootstrap::create(desc)` — step 2 plus `HybridComposer::setSharedRhiContext(shared RhiContext)`
4. Per frame: `tick(ctx)` → (optional modules) → `render(ctx)` or `runFrame(ctx)`
   - `HybridComposer::render()` mirrors software + RHI command list, then `RhiContext::beginFrame` / `submitFrame`
5. `HybridRendererBootstrap::shutdown()` — detach shared context, destroy `RhiContext` / `FrameManager`
6. `RendererBootstrap::shutdown()` — same RHI tear-down when used without hybrid glue
7. `fuse::core::shutdown()` — job scheduler last

All shutdown steps are idempotent. GPU init and submit require the registered render thread (`platform::mayTouchGpuContext()`).

`HybridRendererBootstrap` sources and renderer headers are compiled into `fuse_hybrid` only when `FUSE_BUILD_VULKAN=ON`. Android CI keeps `FUSE_BUILD_VULKAN=OFF`, so `fuse_hybrid` stays software-only with no `fuse/renderer/*` includes.

### Consumers

| Binary / test | Uses B2.10 path |
|---------------|-----------------|
| `demo_hybrid_hud` | `HybridRendererBootstrap` when `FUSE_HAS_VULKAN_RHI`; software-only `HybridComposer` otherwise |
| `fuse_runtime_smoke` | Optional `RendererBootstrap` init/shutdown when `FUSE_HAS_VULKAN_RHI` |
| `fuse_renderer_bootstrap` | Init/shutdown order + frame submit after bootstrap |
| `fuse_hybrid_renderer_bootstrap` | Hybrid glue + shared `RhiContext` wiring |

---

## B2.11 — Phase 2 deliverables & integration test suite

**Status:** Headless integration test exercises `RendererBootstrap` → `RhiContext` → `RenderGraph` + `RasterPath` + `CompositePass` together; full production gates from P2 §2.11 remain deferred.

| Deliverable | Location | B2.11 status |
|-------------|----------|--------------|
| Phase 2 integration test | `Source/FUSE/Renderer/tests/test_vulkan_phase2_integration.cpp` | **Done** — `fuse_vulkan_phase2_integration` (headless, `FUSE_BUILD_VULKAN` gated) |
| Per-component unit tests | `Source/FUSE/Renderer/tests/`, `Source/FUSE/Hybrid/tests/` | **Done** — B2.1–B2.10 targets listed below |
| CI umbrella run | `.github/workflows/fuse-umbrella-linux.yml` | **Done** — Lavapipe headless ICD; no window surface |

### Checklist — scaffold landed (B2.1–B2.10) vs deferred (full P2 gates)

#### Vulkan infrastructure

| Item | Status | Notes |
|------|--------|-------|
| Instance/device bootstrap (headless) | **Done** | `VulkanBootstrap`, optional validation layers; stub when loader missing |
| Physical device selection (discrete over integrated) | **Deferred** | No RTX-specific policy yet |
| Graphics/compute/transfer queue families | **Deferred** | Single graphics queue path today |
| Swapchain 1920×1080 triple-buffered + resize | **Deferred** | Headless stub + frame ring sketch only (`SurfaceKind::Headless`) |
| Frame-in-flight (3 slots) | **Done** | `FrameManager` + fences/semaphores; timeline values not asserted |
| `vkSetDebugUtilsObjectNameEXT` on all objects | **Deferred** | Debug naming not wired |

#### Resource system

| Item | Status | Notes |
|------|--------|-------|
| VMA buffer/texture create/destroy | **Done (scaffold)** | Real VMA when vendored; stub handles otherwise (`fuse_vulkan_resources`) |
| Bindless descriptor table | **Deferred** | Index free-list only; no descriptor pool |
| Staging ring wrap / large upload stress | **Deferred** | 64 MiB ring scaffold; no 256 MiB corruption test |
| Async upload fence timeout | **Deferred** | — |
| Win32 external memory + `cudaImportExternalMemory` | **Deferred** | `import_vulkan_*` returns `ok=false` (B2.6 stub) |

#### Shader & pipeline system

| Item | Status | Notes |
|------|--------|-------|
| Offline SPIR-V fixtures (`spirv-val` clean) | **Done** | Checked-in `.spv`; `fuse_shader_pipeline` |
| Pipeline cache serialize/restore | **Deferred** | — |
| Hot-reload < 200 ms | **Deferred** | — |
| Push constants per-draw (RenderDoc) | **Deferred** | Placeholder layout only |

#### CUDA–Vulkan interop

| Item | Status | Notes |
|------|--------|-------|
| `SharedTimeline` 10k-frame race-free | **Deferred** | Stub semaphore wrapper |
| Vulkan buffer readback via CUDA pointer | **Deferred** | Import API surface only |
| CUDA texture visible in composite pass | **Deferred** | Composite is logical stub |
| `cuda-memcheck` / `compute-sanitizer` clean | **Deferred** | No toolkit on CI |

#### Rendering correctness

| Item | Status | Notes |
|------|--------|-------|
| White triangle, black background (Week 1 gate) | **Done (headless)** | `RasterPath` clear + triangle; no on-screen present in CI |
| G-buffer attachments (RenderDoc) | **Deferred** | Deferred renderer scaffold separate from B2.8 path |
| CUDA ray march vs reference | **Done (CPU ref)** | `fuse_ray_march_stub`; full kernel deferred |
| SDF normals smooth at surface | **Deferred** | — |
| Composite blend at all GRIA α | **Done (stub)** | `CompositePass` + graph ordering; no real bindless shader |
| 60 fps @ 1080p, 10-object SDF (Week 5 gate) | **Deferred** | No present path / perf gate in CI |

#### Performance baselines (RTX 3090)

| Item | Status | Notes |
|------|--------|-------|
| GPU frame time < 8 ms @ 1080p | **Deferred** | — |
| CUDA kernel > 60% occupancy | **Deferred** | — |
| Zero per-frame heap allocs | **Done (scaffold)** | Fixed `kMaxPassesPerFrame` storage; not profiled under load |
| Render graph compile < 1 ms CPU | **Deferred** | Not timed in CI |

### Integration test flow (headless)

`fuse_vulkan_phase2_integration` validates the full B2.5–B2.10 wiring in one executable:

1. `fuse::core::initialize()` + render-thread registration
2. `RendererBootstrap::create()` — `RhiContext` + `FrameManager` + `VulkanBootstrap`
3. Per frame (3× to exercise triple-buffer ring): `beginFrame` → `submitFrame` with clears + 2D sprites
4. Asserts: `RenderGraph` compiles (≥ 4 passes, barriers planned), `CommandBufferRecorder` non-empty, `RasterPath` triangle draw, `CompositePass` stats, frame ring slot < 3

Gated by `FUSE_BUILD_VULKAN=ON` (builds `fuse_rhi` and renderer tests). When the Vulkan loader is absent, configure still succeeds; the test asserts stub-backend rejection paths where applicable.

---

## Desktop vs mobile (design notes)

| Platform | B2.2 stance | Later |
|----------|-------------|-------|
| Linux/Windows desktop | Headless bootstrap + Lavapipe/discrete ICD; real swapchain via External surface | Editor Qt viewport |
| macOS | Same API surface; MoltenVK behind platform module | Dedicated `FUSE_PLATFORM_MACOS` RHI backend |
| iOS / Android | Stub compile (`FUSE_BUILD_VULKAN=OFF` in mobile CI) | GLES/Metal/Vulkan queue ownership per §4.4 |

Portable invariant unchanged: job code emits `RenderCommandList`; platform module selects Vulkan/Metal/GLES backend and supplies `SurfaceDesc`.

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_vulkan_bootstrap` | Instance/device or stub path; surface abstraction; render-thread submit |
| `fuse_vulkan_swapchain` | Headless swapchain desc; frame ring advance; external surface graceful failure |
| `fuse_vulkan_resources` | `HandleMap`, bindless index recycle, buffer/texture create/destroy |
| `fuse_rhi_resource_destroy_order` | B2.3 deepen — destroy-order teardown, staging ring guard, GPU alloc stats + hook |
| `fuse_shader_pipeline` | SPIR-V I/O, offline compiler, shader module + pipeline layout (stub or Vulkan) |
| `fuse_graphics_pipeline` | `VkGraphicsPipeline`, headless `RasterPath` clear + triangle, `RhiContext` wiring |
| `fuse_render_command_list` | Hybrid mirrors commands without breaking placeholder pixels |
| `fuse_render_graph` | Barrier planning, pass culling, command recorder, RHI graph submit |
| `fuse_composite_pass` | Composite pass scaffold, graph ordering (composite before present), RHI stats |
| `fuse_renderer_bootstrap` | B2.10 init/shutdown order, FrameManager availability, post-init submit |
| `fuse_vulkan_phase2_integration` | **B2.11** — `RendererBootstrap` + `RenderGraph` + `RasterPath` + `CompositePass` headless multi-frame |
| `fuse_hybrid_renderer_bootstrap` | Hybrid glue, shared RhiContext, runFrame lifecycle |
| `fuse_hybrid_vulkan_presentable` | Headless presentable stubs — vsync mode, resize recreate, present-path state machine via hybrid bootstrap |
| `fuse_rhi_present_path_stub` | RHI `PresentPath` acquire/present/fence-wait/resize stubs without GPU window |
| `fuse_hybrid_tests` | Existing U4 software renderer regressions |
| `fuse_cuda_jobs` | `submit_cuda` hook signals counter without CUDA toolkit |
| `fuse_cuda_interop` | Vulkan/CUDA import + timeline stubs degrade on CI |
| `fuse_ray_march_stub` | CPU sphere hit distance, `submit_cuda` counter signal, ray-march job wiring |
| `fuse_screen_space_effects_stub` | SSAO/SSR/SSGI CPU reference samples, launches, `submit_*_job` counter signal — see [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) |

Run:

```bash
ctest --test-dir build --output-on-failure -R 'fuse_vulkan|fuse_shader_pipeline|fuse_graphics_pipeline|fuse_render_command|fuse_render_graph|fuse_composite_pass|fuse_renderer_bootstrap|fuse_vulkan_phase2|fuse_hybrid_renderer|fuse_hybrid_vulkan_presentable|fuse_rhi_present_path_stub|fuse_hybrid|fuse_cuda|fuse_ray_march|fuse_screen_space_effects'
```

---

## CI story (honest)

1. **Linux umbrella** — `FUSE_BUILD_VULKAN=ON`, Mesa Lavapipe for headless ICD; Khronos validation layers used when installed, otherwise stub message (non-fatal). Swapchain stays **headless** (no `VkSurfaceKHR`); frame ring exercises real fences/semaphores. `fuse_hybrid_vulkan_presentable` exercises null-window + External-surface wiring only — no GPU window on runner.
2. **Android NDK** — `FUSE_BUILD_VULKAN=OFF`; `fuse_core` + `fuse_hybrid` unchanged.
3. **iOS stub workflow** — unchanged; Vulkan deferred.
4. **CUDA** — umbrella Linux enables `FUSE_BUILD_CUDA=ON`; no NVIDIA toolkit required. `fuse_cuda_jobs`, `fuse_cuda_interop`, and `fuse_ray_march_stub` exercise stub paths.

No GPU window on runner is OK: stub backend keeps configure/build green; when Lavapipe is present, tests exercise real instance/device + frame sync objects.

---

## B2.6 — CUDA job lane & Vulkan interop (stubs)

**Status:** Optional stubs — `submit_cuda()` hook, external-memory import API surface, timeline sync placeholders  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B2.6, §B1.5 CUDA-Job Integration  
**Architecture:** [architecture-parallel.md](./architecture-parallel.md) §3.2 (`submit_cuda` decision gate)

### Build flag — `FUSE_BUILD_CUDA`

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_CUDA=ON
# With CUDA toolkit + GPU: defines FUSE_HAS_CUDA=1
# Without toolkit (CI default): stub path — configure succeeds, APIs no-op gracefully
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_CUDA=OFF` (default) | No `fuse_compute`; `cuda_jobs` / interop headers not linked into tests |
| `FUSE_BUILD_CUDA=ON`, toolkit found | `FUSE_HAS_CUDA=1` — managed stream + named `StreamManager` streams + `ray_march.cu` |
| `FUSE_BUILD_CUDA=ON`, toolkit missing | CPU reference ray marcher + `submit_cuda` stub — CI stays green without `nvcc` |
| CI Linux umbrella | `FUSE_BUILD_CUDA=ON` — stub path; no NVIDIA toolkit on runners required |

### API surface

| Component | Location | Notes |
|-----------|----------|-------|
| `CUDAJobDesc` / `submit_cuda()` | `Source/FUSE/Core/include/fuse/jobs/cuda_jobs.hpp` | Job-scheduler dispatch; signals `JobCounter` on completion |
| `fuse_compute` / `submit_ray_march_job` | `Source/FUSE/Compute/` | B2.7 SDF ray-march pass wired through `submit_cuda` |
| `import_vulkan_buffer` / `import_vulkan_image` | `Source/FUSE/Renderer/include/fuse/renderer/cuda/interop.hpp` | External-memory import deferred — returns `ok=false` until full B2.6 |
| `SharedTimeline` | `Source/FUSE/Renderer/include/fuse/renderer/cuda/vk_sync.hpp` | Timeline semaphore wrapper stub |
| `StreamManager` | `Source/FUSE/Renderer/include/fuse/renderer/cuda/stream_manager.hpp` | Named streams (Render, Physics, AI, Particles, Upload) |
| `TextureDesc::cudaInterop` / `BufferDesc::cudaInterop` | `resources.hpp` | Flag reserved for shared allocations (B2.3+) |

Thread ownership unchanged: CUDA launch jobs run on worker threads; Vulkan record/submit stays on `renderThread()`. Full timeline-semaphore frame flow (vk→cuda→vk) lands after render graph wiring (separate workstream).

---

## Next

- [x] `VkSwapchainKHR` path behind surface abstraction (External surface; headless stub documented)
- [x] Triple-buffered frame ring sketch aligned with `FrameBarrier`
- [x] B2.3 resource handles + stub/VMA allocator + bindless index table
- [x] B2.4 shader scaffold — offline SPIR-V, shader module, pipeline layout placeholder
- [x] B2.5 command buffer recording stubs + render graph compile/execute scaffolding
- [x] B2.6 CUDA job/interop stubs — `FUSE_BUILD_CUDA` gated, CI passes without toolkit
- [x] B2.7 SDF ray-march scaffold — `fuse_compute`, CPU reference tracer, placeholder `.cu` kernel
- [x] B2.8 rasterisation pipeline scaffold — `GraphicsPipeline`, headless clear/triangle `RasterPath`
- [x] B2.9 composite pass scaffold — `CompositePass`, graph node before present, GRIA blend stub
- [x] B2.10 renderer init & main-loop glue — `RendererBootstrap`, `HybridRendererBootstrap`, lifecycle tests
- [x] B2.11 Phase 2 deliverables & integration test suite — checklist in this doc; `fuse_vulkan_phase2_integration`
- [ ] B2.4 follow-up: bindless descriptor pool + graphics pipeline cache
- [ ] B2.5 follow-up: real `vkCmdBeginRenderPass` / queue submit wiring (B2.8 draw list)
- [ ] B2.6 follow-up: `cudaImportExternalMemory`, timeline semaphores, real shared textures
- [ ] Replace `PlaceholderRenderer` present path incrementally — keep software fallback for headless CI
- [x] Own Hybrid presentable path stubs — `PlatformWindow` (null/GLFW), `VulkanPresentable`, `HybridRendererBootstrap` wiring
- [x] B2.2 present path deepen — `PresentPath`, `VsyncMode`, acquire/present/fence-wait/resize recreate stubs + CI state-machine tests
- [x] B2.3 resource deepen — stub/VMA alloc stats, destroy-order teardown, `fuse_rhi_resource_destroy_order` (see [TRACK-B-RHI.md](./TRACK-B-RHI.md))
- [x] B2.5 render graph deepen — pass dependency edges, resource lifetime stubs, compile-order tests
- [ ] Editor Qt native surface (`U6` viewport) → `SwapchainDesc.surface`
- [ ] Android Vulkan WSI + MoltenVK macOS module

---

## Related docs

- [TRACK-B-RHI.md](./TRACK-B-RHI.md) — B2.3 resource allocation deepen (stats, destroy order)
- [work-plan.md](./work-plan.md) — Track B kickoff entry
- [BUILD.md](./BUILD.md) — umbrella CMake options
- [risk-register.md](./risk-register.md) R21 — render thread invariant
