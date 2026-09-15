# Track B — RHI resource allocation (B2.3 deepen)

**Status:** B2.3 buffer/image stub bookkeeping, VMA pool stats hooks, destroy-order teardown  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B2.3  
**Vulkan track:** [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — bootstrap, frame ring, render graph  
**Related:** [TRACK-B-CORE-B13.md](./TRACK-B-CORE-B13.md) — CPU `AllocStats` hooks (mirrored pattern)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `GpuAllocStats` | `include/fuse/renderer/vk/gpu_alloc_stats.hpp` | Buffer/image byte counters, VMA pool snapshot fields |
| `GpuAllocator` | `include/fuse/renderer/vk/allocator.hpp` | Stub + VMA paths record alloc/free; `refreshVmaPoolStats()` |
| `ResourceManager` | `include/fuse/renderer/resource_manager.hpp` | Ordered teardown: samplers → textures → user buffers → staging ring |
| `HandleMap::forEachOccupied` | `include/fuse/handle_map.hpp` | Safe bulk destroy without mutating during iteration |
| `BindlessDescriptors` | `include/fuse/renderer/vk/bindless.hpp` | Generation slot handles, binding-index helpers, handle pack/unpack, heap counts, sparse resize stub |
| `test_rhi_resource_destroy_order` | `Source/FUSE/Renderer/tests/` | Headless destroy-order + stats hook acceptance |
| `test_bindless_descriptors` | `Source/FUSE/Renderer/tests/` | Alloc/free reuse, generation bump, OOB reject, binding pack, handle pack, cap exhaustion |

**B2.3 bindless deepen:** `BindlessDescriptors` CPU heap — generation `BindlessSlotHandle`, alloc/free reuse, `bindingIndexForHandle`, `packBindlessSlotHandle` / `unpackBindlessSlotHandle`, `heapLiveCount` / `heapFreeCount`, sparse `resizeHeap` stub. See `fuse_bindless_descriptors` tests.

**Not in scope (follow-up PRs):** Real `VkDescriptorPool` updates (B2.4), async upload fences, CUDA shared allocations, real GPU memory budgets.

### Present / swapchain path (B2.2 cross-ref)

CPU-side acquire/present lifecycle lives alongside resource teardown in `fuse_rhi`:

| Component | Location | Notes |
|-----------|----------|-------|
| `PresentPath` | `include/fuse/renderer/vk/present_path.hpp` | Acquire/present/recreate state machine; `fenceWaitCount`, resize coalescing |
| `fence_wait.hpp` | `include/fuse/renderer/vk/fence_wait.hpp` | `waitCurrentInFlightFence`, `countPendingInFlightFences`, OOB slot guard |
| `test_rhi_present_path_stub` | `Source/FUSE/Renderer/tests/` | Headless state transitions, recreate path, fence-wait edge cases |

See [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) §B2.2 for the full state diagram and WSI wiring.

---

## Build

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_VULKAN=ON -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build --target fuse_rhi_resource_destroy_order fuse_vulkan_resources fuse_bindless_descriptors
ctest --test-dir build -R 'fuse_rhi_resource_destroy_order|fuse_vulkan_resources|fuse_bindless_descriptors'
```

Optional VMA (developer machines):

```bash
# vendor VulkanMemoryAllocator at third_party/VulkanMemoryAllocator/include/vk_mem_alloc.h
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_VULKAN=ON
```

When VMA is present, `GpuAllocator::refreshVmaPoolStats()` fills `vmaPoolCount` / `vmaPoolUsedBytes` via `vmaCalculateStatistics`.

---

## Stub allocator bookkeeping

Without VMA (CI default), `GpuAllocator` tracks:

- `bufferBytes` / `imageBytes` from `BufferDesc::size` and estimated texture footprint
- `bufferCount` / `imageCount` live resource counts
- `peakUsedBytes`, `allocCount`, `freeCount`, `failedAllocs`
- Stub mapped pointer for `CpuToGpu` / `GpuToCpu` buffers (non-null sentinel; no host memory)

---

## Stats hooks

```cpp
#include <fuse/renderer/vk/gpu_alloc_stats.hpp>

fuse::renderer::setGlobalGpuStatsHook(
  [](const char* name, const fuse::renderer::GpuAllocStats& stats, void*) {
    // debug overlay / GPU budget stub
  });
```

`GpuAllocator` calls `notifyGpuStats` after successful/failed create/destroy. `ResourceManager` names the allocator `"fuse_rhi_gpu"` on init.

---

## Destroy order

`ResourceManager::destroy()` and `destroyAllResources()` tear down in this order:

1. **Samplers** — bindless unregister, no GPU image/buffer deps  
2. **Textures** — unregister bindless, destroy image + view via VMA/stub  
3. **User buffers** — explicit `destroyBuffer` skips the staging ring handle  
4. **Staging ring** — last buffer destroyed before allocator shutdown  
5. **GpuAllocator** — VMA destroy or stub reset  

Individual `destroyTexture` / `destroyBuffer` / `destroySampler` calls follow the same bindless-unregister-then-GPU-free sequence.

---

## Tests

| Test binary | CTest name | Coverage |
|-------------|------------|----------|
| `fuse_rhi_resource_destroy_order` | `fuse_rhi_resource_destroy_order` | Explicit destroy order, destroy-all, staging ring guard, stats + global hook |
| `fuse_vulkan_resources` | `fuse_vulkan_resources` | Handle map generation, bindless recycle, create/destroy smoke |
| `fuse_bindless_descriptors` | `fuse_bindless_descriptors` | Slot handle generations, binding helpers, handle pack/unpack, heap counts, cap exhaustion, legacy register API |

All tests run headless — stub allocator path when VMA is absent; Lavapipe path when Vulkan ICD is available.

---

## Gates (B2.3 deepen)

- [x] Expanded buffer/image stub allocation bookkeeping on FUSE APIs
- [x] VMA pool stats hook surface + stub counters
- [x] ResourceManager ordered destroy-all + CPU/headless tests
- [x] Track doc (this file) + `TRACK-B-VULKAN.md` checklist update
- [x] Bindless CPU heap deepen — generation handles, binding helpers, sparse resize stub
- [ ] Bindless descriptor pool + real descriptor updates (B2.4 follow-up)
- [ ] Async upload + fence completion (B2.3+)
