# Track B — Vulkan Bootstrap (B2.1–B2.10) + CUDA Ray March (B2.7)

**Status:** WP-06b ✅ B2.1 bootstrap + B2.2 swapchain/frame ring + **WP-06c ✅ real `vkQueueSubmit` + honest headless present sink** + **WP-06d ✅ `vkCmdBeginRenderPass` graph encode + bindless pool + null/GLFW WSI scaffold** + **WP-06e ✅ graph `vkCmdPipelineBarrier` + bindless `vkUpdateDescriptorSets` + pipeline cache disk I/O + swapchain FB present pass scaffold** + **WP-06f ✅ bindless composite GPU blit + CUDA interop/timeline honest stubs** + **WP-06g ✅ CUDA interop import deepen + composite CUDA texture path + GLFW present gate + U6 `SwapchainDesc` handoff** + **WP-06h ✅ CUDA interop fill kernel + frame-sync progress + Qt surface stub + composite SPIR-V regen docs** + **WP-06i ✅ Qt `QVulkanInstance` bootstrap + load-stress stubs** + **WP-06j ✅ Lavapipe teardown hardening + Qt embed/timeline stress** + **WP-06k ✅ Lavapipe tune + composite SPIR-V regen + viewport swapchain recreate stubs** + **WP-06l ✅ spirv-val regen gate + consumed swapchain present after recreate + PlaceholderRenderer toggle** + **WP-06m ✅ Qt `vkQueuePresentKHR` gate + viewport present eligibility + PlaceholderRenderer embed deepen + Android/MoltenVK cmake stubs** + **WP-06n ✅ Qt present path readiness + combined CUDA/timeline stress** + **WP-06o ✅ Qt present path eligible diagnostics + scoped PlaceholderRenderer retirement deepen** + **WP-06p ✅ mobile Vulkan stubs deepen + PlaceholderRenderer retirement tests + Lavapipe destroy-order ICD lock** + B2.3 resource/bindless scaffolding + B2.4 shader scaffold + B2.5 command buffer / render graph scaffolding + B2.6 CUDA/interop stubs + B2.7 SDF ray-march CUDA path scaffolding + B2.8 rasterisation pipeline scaffold + B2.9 composite pass scaffold + B2.10 renderer init & main-loop glue + **B2.11 Phase 2 deliverables & integration test suite**  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B2.1–B2.5, §B2.6, §B2.7, §B2.8, §B2.9, §B2.10  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §4.2, §4.4, §5.3  
**Hybrid integration:** [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md)

**Execution order from here:** [EXECUTION-PLAN.md](./EXECUTION-PLAN.md)

### B2.11 — validation gate ✅

`fuse_vulkan_validation_gate` re-runs the whole CTest suite with `VK_LAYER_KHRONOS_validation`
forced on (`VK_INSTANCE_LAYERS`) and fails on any validation message. It runs serially and
reports *skipped* when the layer manifest is not installed. Linux CI installs
`vulkan-validationlayers`, so the gate is live there.

```bash
ctest --test-dir build/fuse-debug -R fuse_vulkan_validation_gate --output-on-failure
```

Per-image layout tracking (raster colour/depth, swapchain images, CUDA interop image) backs the
graph barriers: `VkFrameEncodeContext::{barrierImageLayout, depthImageLayout,
presentImageLayout, cudaImageLayout}` point at state owned by the image owner, barriers use the
tracked value as `oldLayout`, and render passes update it to their final layout. Composite renders
into its own offscreen target (never the raster framebuffer it samples).

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `RenderCommandList` | `Source/FUSE/Renderer/` | Per-frame draws/clears merged on render thread |
| `VulkanInstance` / `VulkanDevice` | `Source/FUSE/Renderer/include/fuse/renderer/vk/` | Headless bootstrap; optional validation layers |
| `VulkanSurface` | same | Headless vs external `VkSurfaceKHR` abstraction |
| `PlatformWindow` | `Source/FUSE/Core/include/fuse/platform/window.hpp` | Null WSI (CI default) or optional GLFW hidden window (`FUSE_PLATFORM_WINDOW_GLFW=ON`) |
| `VulkanPresentable` | `Source/FUSE/Hybrid/include/fuse/hybrid/vulkan_presentable.hpp` | Own Hybrid presentable path — wires platform window → External `VulkanSurface` |
| `VulkanSwapchain` | same | Real `VkSwapchainKHR` when External surface + WSI; headless stub otherwise |
| `FrameManager` | same | Triple-buffered fence/semaphore ring aligned with `FrameBarrier` |
| `ResourceManager` / `GpuAllocator` | `Source/FUSE/Renderer/` | Handle-based buffers/images; VMA when vendored, stub otherwise |
| `BindlessDescriptors` | `Source/FUSE/Renderer/include/fuse/renderer/vk/` | CPU heap + **UPDATE_AFTER_BIND `VkDescriptorPool`/set/layout scaffold** (WP-06d) |
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

`HybridRendererBootstrap::render()` (preferred entry):

1. Forward resize → `PresentPath::requestResize` when `VulkanPresentable` needs recreate
2. `PresentPath::waitInFlightFence` → `acquireImage` (headless: `UINT32_MAX`; WSI: real index)
3. `RhiContext::setAcquiredSwapchainImage` — wires acquire result into queue submit
4. `HybridComposer::render()` — software placeholder + RHI mirror + `submitGraphicsQueue` (below)
5. `PresentPath::markReadyToPresent` → `presentImage` (`vkQueuePresentKHR` when WSI; headless honest sink otherwise)

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
| `submitGraphicsQueue` | `queue_submit.hpp` | Real `vkQueueSubmit` on frame-slot CB + fence; WSI semaphores when External surface acquires |
| `VsyncMode` | `Fifo` / `Mailbox` / `Immediate` | Maps to `VkPresentModeKHR`; default `Fifo` for CI |

#### Present path state machine (B2.2 deepen)

CPU-side lifecycle exercised without a real GPU window:

```
Idle → waitInFlightFence → FenceWaited → acquireImage → ImageAcquired
     → markReadyToPresent → ReadyToPresent → presentImage → Presented → Idle
ResizePending → (waitAllInFlightFences) → recreateSwapchain → Idle
```

| API | Headless CI behaviour |
|-----|----------------------|
| `FrameManager::waitInFlightFence` | Slot bookkeeping; real `vkWaitForFences` when backend active |
| `waitInFlightFenceForSlot` / `waitCurrentInFlightFence` / `waitAllInFlightFences` | `fence_wait.hpp` helpers — per-slot acquire wait; OOB slot rejection; `countPendingInFlightFences` for diagnostics |
| `PresentPath::acquireImage` | Returns `UINT32_MAX`; advances state |
| `PresentPath::markReadyToPresent` | `ImageAcquired` → `ReadyToPresent` after render record |
| `submitGraphicsQueue` | Real `vkQueueSubmit` on slot primary CB + `inFlightFence`; no WSI wait semaphores |
| `PresentPath::presentImage` | Succeeds without `vkQueuePresentKHR` — honest headless present sink after queue submit |
| `PresentPath::requestResize` | Coalesces to latest dimensions; `rebuild()` on next fence wait or `recreateSwapchain()` |
| `PresentPath::recreateSwapchain` | Explicit resize apply; no-op when nothing pending |
| `PresentPath::fenceWaitCount` / `swapchainRecreateCount` | Diagnostics counters for CI acceptance |
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

Per-slot command pools and primary command buffers are allocated when the Vulkan backend is active (B2.5). Bindless `VkDescriptorPool` scaffold landed in B2.4 follow-up (WP-06d); scratch allocators remain deferred.

---

## B2.5 — Command buffer & render graph scaffolding

**Status:** CPU-side graph compile + **real `vkCmdBeginRenderPass` / draw encoding on frame-slot CB** when `RasterPath` targets are ready (WP-06d); barriers/composite GPU nodes still stubbed.

| Component | Location | Notes |
|-----------|----------|-------|
| `CommandBufferRecorder` | `include/fuse/renderer/command_buffer.hpp` | Logical records for tests + optional `VkFrameEncodeContext` → real `vkCmdBeginRenderPass` / draw |
| `RenderGraph` | `include/fuse/renderer/render_graph.hpp` | Pass nodes declare texture/buffer accesses; `compile()` plans barriers + culls unused passes |
| `populateRenderGraphFromCommandList` | `render_graph.cpp` | Maps `RenderCommandList` clears/sprites → graph passes (clear → sprites2d → composite → present) |
| `FrameCommandData` | `vk/frame.hpp` | Per-slot `VkCommandPool` + primary `VkCommandBuffer` when backend active |
| `RhiContext` integration | `rhi_context.cpp` | `submitFrame()` resets slot pool, executes graph into recorder, `submitGraphicsQueue` with `commandsAlreadyRecorded` |

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
| `BindlessDescriptors` | `vk/bindless.hpp` | CPU heap: generation `BindlessSlotHandle`, binding-from-handle, handle pack/unpack, heap counts, sparse resize stub; VkDescriptorPool deferred |

**B2.3 deepen:** [TRACK-B-RHI.md](./TRACK-B-RHI.md) — stub alloc stats, destroy-order teardown, `fuse_rhi_resource_destroy_order` tests.

Optional VMA: place [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) at `third_party/VulkanMemoryAllocator/include/vk_mem_alloc.h` and reconfigure.

---

## B2.4 — Shader system & pipeline compiler (scaffold)

**Status:** Offline SPIR-V load + `VkShaderModule` + **`PipelineCache` in-memory scaffold** + bindless descriptor pool (WP-06d).

| Component | Location | Notes |
|-----------|----------|-------|
| `ShaderCompiler` | `Source/FUSE/Renderer/include/fuse/renderer/shader/` | Offline-first — loads checked-in `.spv` fixtures |
| `ShaderModule` | same | Creates `VkShaderModule` when `FUSE_VULKAN_BACKEND=1` and device ready |
| `PipelineLayout` | `Source/FUSE/Renderer/include/fuse/renderer/vk/pipeline_layout.hpp` | Placeholder layout (push constants only; bindless sets deferred) |
| `PipelineCache` | `include/fuse/renderer/vk/pipeline_cache.hpp` | `vkCreatePipelineCache` + snapshot API; disk serialize/restore deferred |
| `BindlessDescriptors` | `include/fuse/renderer/vk/bindless.hpp` | CPU heap + `VkDescriptorPool`/layout/set when device ready |
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
| Physical device selection (discrete over integrated) | **Measured** | 2026-09-25 workstation: the only adapter is the RTX 3090 and `VulkanDevice` selected it. No integrated GPU was present, so discrete-over-integrated was not exercised on this PC. Policy coverage remains `fuse_b2_physical_device_selection` |
| Graphics/compute/transfer queue families | **Measured** | RTX 3090: 6 families, graphics 0, dedicated compute 2, dedicated transfer 1 |
| Swapchain 1920×1080 triple-buffered + resize | **Deferred** | Headless stub + frame ring sketch only (`SurfaceKind::Headless`) |
| Frame-in-flight (3 slots) | **Done** | `FrameManager` + fences/semaphores; timeline values not asserted |
| `vkSetDebugUtilsObjectNameEXT` on all objects | **Deferred** | Debug naming not wired |

#### Resource system

| Item | Status | Notes |
|------|--------|-------|
| VMA buffer/texture create/destroy | **Done (scaffold)** | Real VMA when vendored; stub handles otherwise (`fuse_vulkan_resources`) |
| Bindless descriptor table | **Done (WP-06e)** | UPDATE_AFTER_BIND pool + `vkUpdateDescriptorSets` on register/unregister |
| Staging ring wrap / large upload stress | **Done** | `fuse_b2_staging_wrap` on the RTX 3090: 256 × 1 MB chunks through a 16 MB ring (15 wraps), 256/256 read back intact, 0 fence timeouts |
| Async upload fence timeout | **Deferred** | — |
| Win32 external memory + `cudaImportExternalMemory` | **Deferred** | `import_vulkan_*` returns `ok=false` (B2.6 stub) |

#### Shader & pipeline system

| Item | Status | Notes |
|------|--------|-------|
| Offline SPIR-V fixtures (`spirv-val` clean) | **Done** | Checked-in `.spv`; `fuse_shader_pipeline` |
| Pipeline cache serialize/restore | **Done (WP-06e)** | `snapshotData` / `restoreFromData` / disk I/O; `fuse_pipeline_cache` |
| Hot-reload < 200 ms | **Deferred** | — |
| Push constants per-draw (RenderDoc) | **Deferred** | Placeholder layout only |

#### CUDA–Vulkan interop

| Item | Status | Notes |
|------|--------|-------|
| `SharedTimeline` 10k-frame race-free | **Deferred** | Stub semaphore wrapper |
| Vulkan buffer readback via CUDA pointer | **Deferred** | Import API surface only |
| CUDA texture visible in composite pass | **Deferred** | Composite is logical stub |
| `cuda-memcheck` / `compute-sanitizer` clean | **Partial** | 2026-09-25: `compute-sanitizer --tool memcheck` reported 0 errors on the ray-march, particle, and physics kernel gates. Not a full renderer frame loop |

#### Rendering correctness

| Item | Status | Notes |
|------|--------|-------|
| White triangle, black background (Week 1 gate) | **Done (headless)** | `RasterPath` clear + triangle; no on-screen present in CI |
| G-buffer attachments (RenderDoc) | **Deferred** | Deferred renderer scaffold separate from B2.8 path |
| CUDA ray march vs reference | **Done (device parity)** | `fuse_ray_march_kernel_parity` passed on the RTX 3090 (CUDA vs CPU kernel). 1920×1080, 10 objects: launch 3.35 ms, host wall 15.88 ms including copies |
| SDF normals smooth at surface | **Deferred** | — |
| Composite blend at all GRIA α | **Done (WP-06f)** | `CompositeGpuPath` bindless shader + graph ordering; CUDA texture still placeholder colour |
| 60 fps @ 1080p, 10-object SDF (Week 5 gate) | **Measured, present not run** | SDF march only, no swapchain. Launch 3.35 ms; copy-inclusive wall 15.88 ms. On-screen present stays behind `FUSE_TRACK_B_UNLOCK` |

#### Performance baselines (RTX 3090)

| Item | Status | Notes |
|------|--------|-------|
| GPU frame time < 8 ms @ 1080p | **Partial** | SDF march launch 3.35 ms (under 8 ms). Copy-inclusive wall 15.88 ms. Not a presented frame |
| CUDA kernel > 60% occupancy | **Measured** | Elevated `ncu` after `RmProfilingAdminOnly=0`. SDF march at 1920×1080, 16×8 blocks: achieved **64%** (theoretical 75%). Particle update at 262144 slots: achieved **69%** (theoretical 83%); compact **73%** |
| Zero per-frame heap allocs | **Done (scaffold)** | Fixed `kMaxPassesPerFrame` storage; not profiled under load |
| Render graph compile < 1 ms CPU | **Done** | Release, this workstation: hybrid graph median 0.90 µs, 32-pass chain median 16.20 µs (`fuse_b2_render_graph_budget`) |

### Hardware run (2026-09-25, this workstation)

Dual Intel Xeon Gold 6242, NVIDIA GeForce RTX 3090 (24576 MiB, driver 595.79), CUDA 13.2.51. Release. CPU and Vulkan numbers are MinGW GCC 13.2 (`build-hw`). CUDA numbers are MSVC 19.51 (`build-cuda`, `sm_86`). This PC has no integrated GPU. The Vulkan SDK is not installed, so the MSVC tree did not link Vulkan. RenderDoc is not installed. Production present was not unlocked.

| Gate | Result |
|------|--------|
| Device pick | `VulkanDevice` selected **NVIDIA GeForce RTX 3090**: 6 families, graphics 0, dedicated compute 2, dedicated transfer 1 |
| ECS 100k Transform+Mesh+RigidBody | `each_chunk` median **291.1 M components/s** (24.5 GB/s). 500M/s target missed. 200M/s floor passed |
| Render-graph compile | hybrid median **0.90 µs**, 32-pass median **16.20 µs** |
| 1M alloc/free | `fuse_core_b1_alloc_million_cycles` passed (0 corruption) |
| Staging 256 MB | 256/256 chunks intact, 15 wraps, 0 fence timeouts |
| SDF ray march 1920×1080, 10 objects | launch **3.35 ms**, host wall **15.88 ms** (upload + launch + download). Parity vs the CPU kernel passed. Not a swapchain present |
| SVO 1M rays | CPU parallel 184 ms. CUDA wall **150 ms**, launch **130 ms**. 10 ms target missed. Voxel, face and hit/miss match the CPU path; distance is within 1e-5 relative (nvcc fma contraction) |
| Broadphase, 10k bodies | CUDA wall **33.6 ms** per call, copies included. 2 ms target missed. CPU parallel on the same step was 26–35 ms |
| Narrowphase | 1,956 contacts: CUDA wall **2.06 ms**. 20,639 contacts: **20.9 ms**. Host clock around the call, not a resident CUDA-event batch of 1k pairs |
| Solver step | 9,610 bodies, existing step (4 substeps × 8 iterations): CUDA wall **42.2 ms**. Not “10 iterations × 10k contacts”. 5 ms target missed |
| `compute-sanitizer` memcheck | **0 errors** on the ray-march, particle (5,000 slots), and physics kernel gates |
| Nsight occupancy (60% / 70%) | Elevated `ncu` after allowing counters. SDF march 1920×1080 achieved **64%**. Particle compact **73%**, particle update **69%** on 262144 particles |
| Win32 `cudaImportExternalMemory` | Not run. CUDA and Vulkan were not linked in one binary |
| On-screen present / RenderDoc | Not run |

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
| `fuse_pipeline_cache` | Pipeline cache snapshot/restore + disk round-trip (`fuse_pipeline_cache`) |
| `fuse_bindless_descriptors` | Generation slot handles, alloc/free reuse, binding-from-handle, handle pack/unpack, heap counts, cap exhaustion, **descriptor update/clear counts** |
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
| `fuse_rhi_queue_submit` | Real `vkQueueSubmit` on frame ring; headless semaphores-off path; RHI + present-path mirror |
| `fuse_hybrid_tests` | Existing U4 software renderer regressions |
| `fuse_cuda_jobs` | `submit_cuda` hook signals counter without CUDA toolkit |
| `fuse_cuda_interop` | Vulkan/CUDA import + timeline/frame-sync progress + interop fill stub tests |
| `fuse_ray_march_stub` | CPU sphere hit distance, `submit_cuda` counter signal, ray-march job wiring |
| `fuse_screen_space_effects_stub` | SSAO/SSR/SSGI CPU reference samples, launches, `submit_*_job` counter signal — see [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) |

Run:

```bash
ctest --test-dir build --output-on-failure -R 'fuse_vulkan|fuse_shader_pipeline|fuse_graphics_pipeline|fuse_render_command|fuse_render_graph|fuse_composite_pass|fuse_renderer_bootstrap|fuse_vulkan_phase2|fuse_hybrid_renderer|fuse_hybrid_vulkan_presentable|fuse_rhi_present_path_stub|fuse_rhi_queue_submit|fuse_pipeline_cache|fuse_hybrid|fuse_cuda|fuse_ray_march|fuse_screen_space_effects'
```

---

## WP-06b deliverables (B2.1–B2.2)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `VulkanInstance` / `VulkanDevice` headless bootstrap | **Done** | Real `VkInstance`/`VkDevice` when Lavapipe ICD present; stub when loader missing |
| `VulkanSwapchain` headless desc + External `VkSwapchainKHR` path | **Done** | CI uses `SurfaceKind::Headless`; `acquireNextImage`/`present` no-op safely |
| Triple-buffered `FrameManager` ring | **Done** | Fences + semaphores per slot; aligned with `FrameBarrier` |
| `PresentPath` acquire/present/fence-wait/resize stubs | **Done** | `fuse_rhi_present_path_stub` + hybrid bootstrap wiring |
| `RenderCommandList` + `RhiContext::submitFrame` on render thread | **Done** | `platform::requireGpuContextThread()` guard; hybrid mirrors software draws |
| `HybridRendererBootstrap` dual path | **Done** | Software `PlaceholderRenderer` + RHI mirror + `PresentPath` per frame |
| Headless bootstrap tests | **Done** | `fuse_vulkan_bootstrap`, `fuse_vulkan_swapchain`, `fuse_hybrid_renderer_bootstrap`, `fuse_hybrid_vulkan_presentable` |

## WP-06c deliverables (B2.2 follow-up — queue submit + honest present)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `submitGraphicsQueue` / `recordFrameSlotCommands` | **Done** | Real `vkQueueSubmit` on triple-buffered frame slot; Lavapipe/headless CI |
| `RhiContext::submitFrame` queue submit | **Done** | Graph execute → raster/composite → `vkQueueSubmit` → `endFrame` |
| WSI semaphore wiring | **Done (API)** | `imageAvailable` wait + `renderFinished` signal when External surface + valid acquire |
| Headless honest present sink | **Done** | No `vkQueuePresentKHR`; `presentSkippedNoWsiCount` diagnostics |
| `HybridRendererBootstrap` present ordering | **Done** | Acquire → render/submit → mark ready → present |
| `fuse_rhi_queue_submit` tests | **Done** | Direct submit + RHI integration + present-path mirror |
| Lavapipe CTest `VK_ICD_FILENAMES` | **Done** | Renderer/Hybrid Vulkan tests set `lvp_icd.json` when ICD needed |

**Deferred (post–WP-06c):** ~~real `vkCmdBeginRenderPass` in graph execute~~ → landed WP-06d; ~~bindless descriptor pool~~ → pool scaffold WP-06d.

---

## WP-06d deliverables (B2.4–B2.5 follow-up + null WSI)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `vkCmdBeginRenderPass` in graph execute | **Done** | `CommandBufferRecorder` + `VkFrameEncodeContext` from `RasterPath`; unified frame-slot submit |
| Bindless `VkDescriptorPool`/layout/set | **Done (scaffold)** | UPDATE_AFTER_BIND pool; `vkUpdateDescriptorSets` on register deferred |
| `PipelineCache` | **Done (scaffold)** | Passed to `vkCreateGraphicsPipelines`; disk serialize deferred |
| Null/GLFW WSI scaffold | **Done** | `window_wsi.hpp`, `FUSE_PLATFORM_WINDOW_GLFW=OFF` default; CI headless |
| `HybridRendererBootstrap` + `demo_hybrid_hud` | **Done** | Software path unchanged; Lavapipe tests green |

**Deferred (post–WP-06d):** ~~graph barrier → `vkCmdPipelineBarrier`~~ → landed WP-06e; ~~bindless descriptor updates~~ → WP-06e; ~~pipeline cache disk I/O~~ → WP-06e; ~~swapchain FB present pass~~ → WP-06e scaffold; `vkQueuePresentKHR` on desktop with display + GLFW; Editor Qt surface (`U6`).

---

## WP-06e deliverables (graph barriers + bindless updates + cache I/O + swapchain present FB)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| Graph-planned `vkCmdPipelineBarrier` | **Done** | `CommandBufferRecorder::encodeVulkanPipelineBarrier`; offscreen `barrierImage` from `RasterPath` |
| Bindless `vkUpdateDescriptorSets` | **Done** | Register/unregister via `ResourceManager`; stub samplers skip invalid handles |
| Pipeline cache disk serialize/restore | **Done** | `snapshotData` / `restoreFromData` / `writeCacheFile` / `readCacheFile`; `fuse_pipeline_cache` |
| Swapchain FB present pass | **Done (scaffold)** | Per-image framebuffers + present render pass on real `VkSwapchainKHR`; `encodePresentSwapchainPass` when acquire valid |
| Null WSI + software demo | **Done** | Headless CI unchanged — present FB inactive without WSI acquire; `fuse_vulkan_phase2_integration` + hybrid paths green |

**Deferred (post–WP-06e):** ~~real composite blit into swapchain image~~ → landed WP-06f (headless offscreen + swapchain when WSI acquire valid); ~~bindless descriptor use in graphics pipeline~~ → WP-06f composite shader path; `vkQueuePresentKHR` on desktop with display + GLFW; Editor Qt surface (`U6`).

---

## WP-06f deliverables (bindless composite GPU blit + CUDA interop stubs)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `CompositeGpuPath` bindless composite shader | **Done** | `composite.vert` / `composite.frag` SPIR-V fixtures; `GL_EXT_nonuniform_qualifier` sampled-image array |
| Bindless set bound in composite pipeline | **Done** | `PipelineLayout` accepts `bindlessSetLayout`; `vkCmdBindDescriptorSets` + push constants in `encodeCompositePass` |
| GPU blit into swapchain backbuffer | **Done (when WSI)** | `fillEncodeContext` targets present FB when acquire valid; headless composites into offscreen raster FB |
| Headless-honest composite (no WSI) | **Done** | No swapchain pretend — offscreen raster target; `fuse_vulkan_phase2_integration` asserts `vulkanCompositeDrawCount` |
| B2.6 CUDA interop/timeline stubs | **Done (honest)** | `InteropUnavailableReason` + `reason` strings on import; `SharedTimeline::signalVulkan` / `waitCuda` stubs; `fuse_cuda_interop` skips cleanly |
| Lavapipe + `demo_hybrid_hud` | **Done** | Software path unchanged; headless ICD tests green |

**Deferred (post–WP-06f):** ~~`cudaImportExternalMemory` + real timeline semaphore pair~~ → WP-06g driver import when exported handles present; ~~`vkQueuePresentKHR` on desktop GLFW window~~ → WP-06g `FUSE_ENABLE_GLFW_PRESENT` gate; ~~Editor Qt surface (`U6`)~~ → WP-06g `RuntimeViewportHook` handoff stub.

---

## WP-06g deliverables (CUDA interop deepen + GLFW present + U6 surface handoff)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `cudaImportExternalMemory` / image surface import | **Done (when handles)** | `exportedHandle` + `allocationSize` on import desc; real CUDA APIs when toolkit + `cudaDevAttrExternalMemorySupport` |
| `SharedTimeline` / `FrameSyncPair` driver wiring | **Done (when device)** | Timeline export/import attempt; `signalVulkan` / `waitCuda` call real APIs when `driverWired` |
| Composite CUDA texture bindless path | **Done (honest)** | `registerCudaSource` + `ensureCudaInteropTexture`; shader samples when `cudaTexIndex != UINT32_MAX`, else placeholder colour |
| `FUSE_ENABLE_GLFW_PRESENT` desktop gate | **Done (OFF default)** | `desktopGlfwPresentRuntimeReady()` + `PresentPath` `realPresentCallCount`; headless CI unchanged |
| `RuntimeViewportHook` → `SwapchainDesc` handoff | **Done (stub)** | `setExternalSurfaceHandle` + `buildSwapchainDescHandoff`; consumed on game thread (`fuse_editor_host`) |
| Lavapipe + `demo_hybrid_hud` | **Done** | Headless present sink unchanged when gate OFF |

**Deferred (post–WP-06g):** ~~CUDA kernel fill into interop texture~~ → WP-06h `interop_fill.cu` + job-lane path; ~~Editor Qt native `VkSurfaceKHR` creation~~ → WP-06h winId stub handoff; ~~full frame sync pair exercised across job lane + render thread~~ → WP-06h `FrameSyncProgress` counters.

**Regenerate locally when glslang available:** `composite.frag` → `composite.frag.spv` (CUDA texture sampling branch). WP-06h regen:

```bash
sudo apt install glslang-tools   # when missing
glslangValidator -V Source/FUSE/Renderer/shaders/fixtures/composite.frag \
  -o Source/FUSE/Renderer/shaders/fixtures/composite.frag.spv
spirv-val Source/FUSE/Renderer/shaders/fixtures/composite.frag.spv
```

---

## WP-06h deliverables (CUDA interop fill + frame-sync progress + Qt surface stub)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `interop_fill.cu` CUDA kernel | **Done (when toolkit)** | `surf2Dwrite` solid fill into imported interop surface; stub path when no toolkit |
| `fillInteropTexture` / `submitInteropFillJob` | **Done** | Sync + job-lane paths; honest `InteropFillResult::stubPath` on CI |
| `CompositeGpuPath::fillCudaInteropTexture` | **Done** | Exports Vulkan memory handle; fills before composite encode |
| `FrameSyncPair` + `FrameSyncProgress` | **Done** | Render/job lane signal/wait counters; driver-wired timelines when device supports |
| `RhiContext` frame-sync wiring | **Done** | `signalRenderLane` → job-lane fill → `waitRenderLane` on submit |
| Qt `viewport.vk_surface_handle` handoff | **Done (stub)** | `ViewportPlaceholderWidget::showEvent` posts winId; headless-safe opaque handle |
| `composite.frag.spv` regen | **Done (local)** | Regenerated with `glslangValidator` when tooling available; checked-in fixture updated |
| Lavapipe + `demo_hybrid_hud` | **Done** | Headless ICD tests unchanged |

**Deferred (post–WP-06h):** ~~real Qt `QVulkanInstance` / `VkSurfaceKHR` creation~~ → WP-06i `QVulkanInstance` bootstrap with winId fallback; composite SPIR-V regen on CI runner; ~~full timeline frame flow profiled under load~~ → WP-06i `stressFrameSyncUnderLoad` / `stressInteropFillUnderLoad` stubs.

---

## WP-06i deliverables (Qt QVulkanInstance progress + CUDA/timeline load stubs)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `ViewportVulkanSurfaceResult` + `createViewportVulkanSurfaceFromWinId` | **Done** | Qt6 Gui path via `QVulkanInstance::createSurface`; headless CI falls back to winId stub |
| `ViewportSwapchainHandoff::qtRealSurface` | **Done** | Distinguishes real Qt surface from WP-06h winId stub |
| `stressFrameSyncUnderLoad` | **Done** | Multi-frame `FrameSyncProgress` counters; driver ops when wired |
| `stressInteropFillUnderLoad` | **Done** | Batch interop-fill stub-path exercise for CI |
| Lavapipe `RUN_SERIAL` ICD tests | **Done** | `ctest -j` no longer races parallel `VkInstance` creation |
| Build hygiene (`test_profiler_assert`, `fuse_cinematics`) | **Done** | Restored corrupted deepen merges; `cue_preview` API fix |

**Deferred (post–WP-06i):** ~~Lavapipe ICD teardown flake on rapid rerun~~ → WP-06j GPU drain + ICD lock quiesce; ~~Qt embed teardown stress~~ → WP-06j `stressViewportVulkanBootstrapTeardown`; driver-wired timeline stress on NVIDIA CI; composite SPIR-V regen on CI runner.

---

## WP-06j deliverables (Lavapipe teardown hardening + Qt embed/timeline stress)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| GPU drain before teardown | **Done** | `waitAllInFlightFences` + `vkDeviceWaitIdle` in `RendererBootstrap::shutdown` and `HybridRendererBootstrap::shutdown` (explicit paths — avoids Lavapipe destructor flake) |
| `fuse_hybrid_vulkan_presentable` rapid rerun | **Done** | 8-cycle create → render → shutdown in one process (`testHybridBootstrapRapidTeardownRerun`) |
| ICD lock post-test quiesce | **Done** | `run_vulkan_icd_locked.sh` runs test inside flock, then `sync` + brief sleep before releasing lock |
| Lavapipe `RUN_SERIAL` on all ICD tests | **Done** | Renderer + Hybrid + Editor Vulkan ICD targets marked `RUN_SERIAL` |
| `stressViewportVulkanBootstrapTeardown` | **Done** | Headless-safe Qt bootstrap create/destroy cycles; `fuse_editor_host` + `fuse_editor_runtime_embed` |
| `stressFrameSyncTeardownCycle` | **Done** | Multi-cycle timeline bookkeeping + destroy; deepened `stressFrameSyncUnderLoad` to 16 frames |
| Lavapipe serial `ctest -j1` | **Done** | Vulkan ICD targets green under serial ctest after teardown hardening |

**Deferred (post–WP-06j):** ~~full Qt viewport embed with consumed swapchain recreate on real display~~ → WP-06k headless-safe recreate stubs; driver-wired timeline stress on NVIDIA CI; ~~composite SPIR-V regen on CI runner~~ → WP-06k `FuseShaderSpirvRegen.cmake`.

---

## WP-06k deliverables (Lavapipe tune + composite SPIR-V regen + Qt viewport swapchain recreate stubs)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| ICD lock quiesce tune | **Done** | Pre-test `sleep 0.10` + post-test `sleep 0.35`; single retry on SIGSEGV/SIGABRT in `run_vulkan_icd_locked.sh` |
| Viewport GPU drain on teardown | **Done** | `drainViewportGpuContext` in `RuntimeViewportHook` destructor (Lavapipe flake reduction) |
| `FuseShaderSpirvRegen.cmake` | **Done** | Configure-time + `fuse_regen_shader_fixtures` target when `glslangValidator` on runner |
| `viewport_swapchain_recreate.*` | **Done** | `PresentPath` resize/recreate stubs consumed from `RuntimeViewportHook::requestResize` |
| `RuntimeEmbedSession` recreate counters | **Done** | `swapchainRecreateAttempts` / `swapchainRecreateCount` for U6 embed diagnostics |
| Lavapipe serial `ctest -j1` | **Done** | Vulkan ICD targets remain green after viewport recreate wiring |

**Deferred (post–WP-06k):** real Qt viewport embed with consumed swapchain on display + `vkQueuePresentKHR`; driver-wired timeline stress on NVIDIA CI; ~~composite SPIR-V `spirv-val` gate on regen target~~ → landed WP-06l.

---

## WP-06l deliverables (spirv-val regen gate + consumed swapchain present + PlaceholderRenderer toggle)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `spirv-val` gate on `fuse_regen_shader_fixtures` | **Done** | When `spirv-val` on runner, regen target + configure-time regen validate `composite.frag.spv` |
| Consumed swapchain present after recreate | **Done** | `presentViewportSwapchainFrame` + `applyViewportPendingSwapchainRecreateAndPresent`; embed counters `consumedSwapchainPresentTicks` / `swapchainPresentAfterRecreateCount` |
| Hybrid bootstrap sync from consumed handoff | **Done** | `syncHybridBootstrapFromConsumedHandoff` mirrors external swapchain into hybrid `RhiContext` |
| `PlaceholderRenderer` replacement progress | **Done (scoped)** | `HybridComposer::setSoftwarePlaceholderEnabled(false)` when embed present path active; RHI mirror unchanged |
| Lavapipe serial `ctest -j1` | **Done** | Vulkan ICD targets remain green after viewport present-after-recreate wiring |

**Deferred (post–WP-06l):** ~~real Qt viewport embed with `vkQueuePresentKHR` on display~~ → landed WP-06m `FUSE_ENABLE_QT_PRESENT` gate; driver-wired timeline stress on NVIDIA CI; full software placeholder removal (keep fallback for headless CI).

---

## WP-06m deliverables (Qt vkQueuePresentKHR gate + PlaceholderRenderer embed deepen + mobile stubs)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `FUSE_ENABLE_QT_PRESENT` desktop gate | **Done (OFF default)** | `desktopQtPresentRuntimeReady()` + unified `desktopPresentRuntimeReady()` / `realPresentEligible()` |
| `PresentPath` Qt present diagnostics | **Done** | `qtPresentEnabled`, `desktopPresentRuntimeReady`, `realPresentCallCount` when gate + swapchain ready |
| `viewport_present_gate.hpp` | **Done** | `viewportQtPresentEligible` + `shouldDisableSoftwarePlaceholderForEmbed` for U6 embed |
| Consumed viewport present deepen | **Done** | `ViewportSwapchainPresentResult` tracks Qt gate + real-present eligibility; embed session counters |
| PlaceholderRenderer embed retirement | **Done (scoped)** | Disable software RGBA when external swapchain wired + real Qt surface consumed |
| `cmake/FuseVulkanMobile.cmake` | **Done** | Android WSI + MoltenVK macOS stub options (docs-only, honest OFF defaults) |
| Lavapipe serial `ctest -j1` | **Done** | Vulkan ICD targets remain green after Qt present gate wiring |

**Deferred (post–WP-06m):** ~~Qt present path readiness without gate~~ → WP-06n `viewportQtPresentPathReady`; ~~combined frame-sync + interop-fill stress~~ → WP-06n `stressFrameSyncAndInteropFillUnderLoad`; exercise real `vkQueuePresentKHR` on display with `FUSE_ENABLE_QT_PRESENT=ON` + Qt6 Gui; driver-wired timeline stress on NVIDIA CI; full software placeholder removal (keep fallback for headless CI).

---

## WP-06n deliverables (Qt present path readiness + timeline/CUDA combined stress)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `realQtPresentEligible()` | **Done** | Qt-specific `vkQueuePresentKHR` gate separate from GLFW; `PresentPath` tracks `qtRealPresentCallCount` |
| `viewportQtPresentPathReady()` / `viewportQtPresentPathEligible()` | **Done** | Handoff + swapchain preconditions without requiring gate ON (headless-safe diagnostics) |
| Viewport present diagnostics deepen | **Done** | `ViewportSwapchainPresentResult` + `RuntimeEmbedSession` counters (`qtPresentPathReadyTicks`, `qtRealPresentCallCount`) |
| `stressFrameSyncAndInteropFillUnderLoad()` | **Done** | Combined frame-sync bookkeeping + interop fill + job-lane fill stub path |
| Timeline/CUDA stress iteration deepen | **Done** | Frame-sync load 24 frames; teardown 6×16; interop fill 24 iterations |
| `RuntimeEmbedSession::reset()` hygiene | **Done** | Restores Qt present counters dropped in prior deepen |
| Lavapipe serial `ctest -j1` | **Done** | Vulkan ICD targets remain green after Qt present + CUDA stress wiring |

**Deferred (post–WP-06n):** ~~Qt present path eligible diagnostics + scoped PlaceholderRenderer retirement deepen~~ → landed WP-06o; exercise real `vkQueuePresentKHR` on display with `FUSE_ENABLE_QT_PRESENT=ON` + Qt6 Gui; driver-wired timeline stress on NVIDIA CI; full software placeholder removal (keep fallback for headless CI).

---

## WP-06o deliverables (Qt present path eligible diagnostics + scoped PlaceholderRenderer retirement deepen)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `viewportQtPresentPathEligible` in present result | **Done** | `ViewportSwapchainPresentResult` tracks full gate+swapchain eligibility |
| Embed session eligible/retirement counters | **Done** | `qtPresentPathEligibleTicks`, `softwarePlaceholderRetiredTicks` |
| `shouldDisableSoftwarePlaceholderForEmbed` deepen | **Done** | Retires software RGBA when `viewportQtPresentPathReady` (gate may still be OFF) |
| `HybridComposer::softwarePlaceholderSkippedFrames` | **Done** | Per-frame counter when software path disabled; RHI mirror unchanged |
| Present diagnostics helper consolidation | **Done** | `recordViewportPresentDiagnostics` / `maybeRetireSoftwarePlaceholder` in `runtime_viewport.cpp` |
| Lavapipe serial `ctest -j1` | **Done** | Vulkan ICD targets remain green after Qt present + placeholder retirement wiring |

**Deferred (post–WP-06o):** ~~Android/MoltenVK cmake/docs deepen~~ → landed WP-06p; ~~PlaceholderRenderer retirement tests~~ → landed WP-06p; ~~Lavapipe ICD lock on destroy-order test~~ → landed WP-06p; exercise real `vkQueuePresentKHR` on display with `FUSE_ENABLE_QT_PRESENT=ON` + Qt6 Gui; driver-wired timeline stress on NVIDIA CI; full software placeholder removal (keep fallback for headless CI).

---

## WP-06p deliverables (mobile Vulkan stubs deepen + PlaceholderRenderer retirement tests + Lavapipe ICD lock tune)

| Deliverable | Status | Notes |
|-------------|--------|-------|
| `mobile_vulkan_stub.hpp` | **Done** | Android WSI + MoltenVK macOS honest stub status helpers |
| `FuseVulkanMobile.cmake` deepen | **Done** | `fuse_report_mobile_vulkan_status()` + configure-time `FUSE_MOBILE_VULKAN_STUB_DOCUMENTED` |
| `fuse_mobile_vulkan_stub` test | **Done** | Headless-safe stub status matrix |
| `test_viewport_present_gate.cpp` | **Done** | `viewportQtPresentPathReady` / `shouldDisableSoftwarePlaceholderForEmbed` matrix |
| `fuse_placeholder_renderer_retirement` test | **Done** | Multi-frame `softwarePlaceholderSkippedFrames` + re-enable path |
| `fuse_rhi_resource_destroy_order` ICD lock | **Done** | Serial Lavapipe quiesce for destroy-order teardown |
| Lavapipe serial `ctest -j1` | **Done** | Vulkan ICD targets remain green after WP-06p wiring |

**Deferred (post–WP-06p):** exercise real `vkQueuePresentKHR` on display with `FUSE_ENABLE_QT_PRESENT=ON` + Qt6 Gui; driver-wired timeline stress on NVIDIA CI; full software placeholder removal (keep fallback for headless CI).

---

**Deferred (historical WP-06b):** ~~real `vkQueueSubmit`~~ → landed WP-06c; WSI present on desktop window still deferred until display + GLFW path exercised.

---

## CI story (honest)

> **Note:** Umbrella GitHub Actions may be paused on `main` — do **not** re-enable CI as part of Track B landings. Validation is local/`ctest` with Lavapipe or stub backend.

1. **Linux umbrella (when CI runs)** — `FUSE_BUILD_VULKAN=ON`, Mesa Lavapipe for headless ICD; Khronos validation layers used when installed, otherwise stub message (non-fatal). Swapchain stays **headless** (no `VkSurfaceKHR`); frame ring exercises real fences/semaphores. `fuse_hybrid_vulkan_presentable` exercises null-window + External-surface wiring only — no GPU window on runner.
2. **Local / agent validation** — `cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_VULKAN=ON -DFUSE_BUILD_T3D=OFF -DFUSE_BUILD_T2D=OFF` then `ctest -R 'fuse_vulkan|fuse_hybrid_renderer|fuse_hybrid_vulkan_presentable|fuse_rhi_present_path_stub|fuse_render_command'`.
3. **Android NDK** — `FUSE_BUILD_VULKAN=OFF`; `fuse_core` + `fuse_hybrid` unchanged.
4. **iOS stub workflow** — unchanged; Vulkan deferred.
5. **CUDA** — umbrella Linux enables `FUSE_BUILD_CUDA=ON`; no NVIDIA toolkit required. `fuse_cuda_jobs`, `fuse_cuda_interop`, and `fuse_ray_march_stub` exercise stub paths.

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
| `import_vulkan_buffer` / `import_vulkan_image` | `Source/FUSE/Renderer/include/fuse/renderer/cuda/interop.hpp` | Real `cudaImportExternalMemory` when exported platform handle present; skip-clean on CI |
| `SharedTimeline` / `FrameSyncPair` | `Source/FUSE/Renderer/include/fuse/renderer/cuda/vk_sync.hpp` | Timeline export/import when device supports; `driverWired` flag for honest tests |
| `FUSE_ENABLE_GLFW_PRESENT` | root `CMakeLists.txt` | OFF default — enables `vkQueuePresentKHR` when display + GLFW WSI available |
| `FUSE_ENABLE_QT_PRESENT` | root `CMakeLists.txt` | OFF default — enables `vkQueuePresentKHR` when display + Qt Vulkan surface available |
| `TrackBHostFeature::EditorViewportPresent` | `fuse/core/track_b.hpp` | Host-scoped runtime unlock: `fuse_editor` sets it once its Qt Vulkan surface is wired; satisfies `productionPresentAllowed()` + `desktopQtPresentRuntimeReady()` for the editor process only (global `FUSE_TRACK_B_UNLOCK` unchanged; ignored under `FUSE_SHIPPING`) |
| `FuseVulkanMobile.cmake` | `cmake/` | Android WSI + MoltenVK macOS stub options (honest OFF defaults) |
| `RuntimeViewportHook` handoff | `Source/FUSE/Editor/` | `ViewportSwapchainHandoff` → `SwapchainDesc.surface` stub for U6 Qt embed |
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
- [x] B2.4 follow-up: bindless descriptor pool + graphics pipeline cache (**pool + in-memory cache scaffold**)
- [x] WP-06c: real `vkQueueSubmit` on frame ring + honest headless present sink (`fuse_rhi_queue_submit`)
- [x] B2.5 follow-up: real `vkCmdBeginRenderPass` in graph execute (WP-06d)
- [x] B2.5 follow-up: graph-planned `vkCmdPipelineBarrier`; present pass targets swapchain FB (WP-06e)
- [x] B2.4 follow-up: `vkUpdateDescriptorSets` on bindless register; pipeline cache disk serialize/restore (WP-06e)
- [x] B2.5 follow-up: real composite GPU blit into swapchain backbuffer (WP-06f — bindless shader; headless offscreen when no WSI)
- [x] B2.6 follow-up: interop/timeline honest stubs + tests that skip cleanly (WP-06f)
- [x] B2.6 follow-up: `cudaImportExternalMemory` + timeline export/import when handles/device present (WP-06g)
- [x] B2.6 follow-up: composite CUDA texture bindless path with honest placeholder (WP-06g)
- [x] B2.6 follow-up: CUDA interop fill kernel + job-lane path (WP-06h)
- [x] B2.6 follow-up: `FrameSyncPair` progress across render + job lanes (WP-06h)
- [x] Editor viewport → Qt winId stub `VkSurfaceKHR` handoff (WP-06h)
- [ ] Replace `PlaceholderRenderer` present path incrementally — keep software fallback for headless CI (**WP-06l/m:** `setSoftwarePlaceholderEnabled` + `shouldDisableSoftwarePlaceholderForEmbed`; full removal deferred)
- [x] Own Hybrid presentable path stubs — `PlatformWindow` (null/GLFW), `VulkanPresentable`, `HybridRendererBootstrap` wiring
- [x] Null/GLFW desktop WSI scaffold — `window_wsi.hpp`, `FUSE_PLATFORM_WINDOW_GLFW` (OFF in CI; headless Lavapipe stays green)
- [x] B2.2 present path deepen — `PresentPath`, `VsyncMode`, acquire/present/fence-wait/resize recreate stubs + CI state-machine tests
- [x] B2.3 resource deepen — stub/VMA alloc stats, destroy-order teardown, `fuse_rhi_resource_destroy_order` (see [TRACK-B-RHI.md](./TRACK-B-RHI.md))
- [x] B2.5 render graph deepen — pass dependency edges, resource lifetime stubs, compile-order tests
- [x] Editor viewport → `SwapchainDesc.surface` handoff stub (`RuntimeViewportHook`, WP-06g)
- [x] Editor viewport swapchain resize/recreate stubs (`viewport_swapchain_recreate`, WP-06k)
- [x] Composite SPIR-V configure-time regen when `glslangValidator` on runner (`FuseShaderSpirvRegen.cmake`, WP-06k)
- [x] Composite SPIR-V `spirv-val` gate on regen target when available (WP-06l)
- [x] Editor viewport consumed swapchain present after recreate (`presentViewportSwapchainFrame`, WP-06l)
- [x] Qt `vkQueuePresentKHR` gate + viewport present eligibility (`FUSE_ENABLE_QT_PRESENT`, WP-06m)
- [x] Qt present path readiness + combined timeline/CUDA stress (`realQtPresentEligible`, `stressFrameSyncAndInteropFillUnderLoad`, WP-06n)
- [x] Qt present path eligible diagnostics + scoped PlaceholderRenderer retirement deepen (`viewportQtPresentPathEligible`, `softwarePlaceholderRetiredTicks`, WP-06o)
- [x] Android Vulkan WSI + MoltenVK macOS cmake stubs (`FuseVulkanMobile.cmake`, WP-06m)
- [x] Mobile Vulkan stub status helpers + retirement gate tests (`mobile_vulkan_stub.hpp`, `fuse_placeholder_renderer_retirement`, WP-06p)
- [x] Lavapipe ICD lock on `fuse_rhi_resource_destroy_order` (WP-06p)
- [x] Editor Qt native surface (`U6` viewport) → real `vkQueuePresentKHR` on display (editor-scoped unlock `TrackBHostFeature::EditorViewportPresent`; `fuse_editor_qt_live_present`, see [editor.md](../editor.md#embedded-vulkan-viewport))

---

## Related docs

- [TRACK-B-RHI.md](./TRACK-B-RHI.md) — B2.3 resource allocation deepen (stats, destroy order)
- [work-plan.md](./work-plan.md) — Track B kickoff entry
- [BUILD.md](./BUILD.md) — umbrella CMake options
- [risk-register.md](./risk-register.md) R21 — render thread invariant
