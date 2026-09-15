# fuse_rhi — Renderer module

Vulkan RHI and deferred rendering scaffolds from [FUSE Master Plan](../../docs/plans/FUSE_MASTER_PLAN.md).

## B5.6 — DDGI (stub)

CPU-first Dynamic Diffuse Global Illumination scaffolding (P5 §5.6). Probe grid types, irradiance cache allocation, and update/sample stub APIs are in place; CUDA probe trace kernels are deferred.

| Header | Role |
|--------|------|
| `gi/ddgi.hpp` | `DDGIDesc`, `ProbeVolume`, `IrradianceCacheEntry`, `DDGI` lifecycle + `sampleIrradiance` |
| `gi/ddgi_kernels.hpp` | CUDA kernel launch stubs (`probe_trace_kernel`, `probe_blend_kernel`) |

### Defaults

- 16×8×16 probe grid (2048 probes)
- 64 probes updated per frame (round-robin)
- Octahedral irradiance atlas + depth variance atlas via `ResourceManager`

## Tests (`ctest`)

| Test | Coverage |
|------|----------|
| `fuse_ddgi` | Grid math, hysteresis blend, probe scheduling, init/update/sample, pipeline slot |
