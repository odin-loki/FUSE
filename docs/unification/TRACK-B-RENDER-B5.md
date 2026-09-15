# Track B — Deferred Rendering (B5.1–B5.4)

**Status:** B5.1 deferred frame pipeline + B5.2 G-buffer layout + B5.3 PBR material system + **B5.4 clustered deferred shading stubs**  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B5.1–B5.4  
**Phase source:** [P5.md](../sources/P5.md) §5.1–5.4

---

## B5.4 — Clustered Deferred Shading (stubs)

**Status:** CPU cluster grid SoA + `ClusteredLightCuller` stub landed; CUDA kernels deferred.

| Component | Location | Notes |
|-----------|----------|-------|
| `ClusterDesc` / `ClusterAABB` / `ClusterGridSoA` | `include/fuse/renderer/lighting/clustered.hpp` | 3D screen cluster grid types |
| `ClusterBuffers` | same | GPU buffer handles for AABBs, light grid, light list, light SSBO |
| `ClusteredLightCuller` | `src/lighting/clustered_light_culler.cpp` | CPU stub — builds cluster AABBs and sphere-culls point/spot lights |
| `DeferredFramePipeline` | `deferred/frame_pipeline.cpp` | `ClusteredLightCull` pass invokes culler when `DeferredRenderer` wires it |
| `DeferredRenderer` | `deferred/deferred_renderer.cpp` | Owns `ClusteredLightCuller`; passes culler into frame graph build |

CUDA `build_cluster_aabbs_kernel` / `cull_lights_kernel` / `deferred_shade_kernel` remain future work (B5.4 follow-up + B2.6 interop).

### Tests

| Target | Validates |
|--------|-----------|
| `fuse_clustered_light_culler` | Cluster count/index, culler init, CPU cull assignment, deferred pipeline wiring |
| `fuse_deferred_pipeline` | Full B5.1 schedule (includes `clustered_light_cull` pass name) |
| `fuse_gbuffer` / `fuse_material_system` | B5.2–B5.3 scaffolding |

```bash
ctest --test-dir build --output-on-failure -R 'fuse_clustered_light_culler|fuse_deferred_pipeline|fuse_gbuffer|fuse_material_system'
```
