# B5.9 — Temporal Anti-Aliasing (stub)

CPU-first TAA scaffolding for Track B5.9. Implements Halton sub-pixel jitter, ping-pong history buffers, and a resolve pass facade. The CUDA `taa_resolve_kernel` (variance clamp, Catmull-Rom history, velocity rejection) is deferred until B2.6 interop and G-buffer velocity wiring land.

## Layout

| Header | Role |
|--------|------|
| `taa_types.hpp` | `TAAParams`, history/jitter/resolve descriptor types |
| `taa_jitter.hpp` | 8-frame Halton (2,3) sequence and NDC projection offsets |
| `taa_history.hpp` | Ping-pong `TextureHandle` history targets |
| `taa_resolve.hpp` | Resolve stub — records inputs and swaps history |
| `taa_pass.hpp` | `TaaPass` facade + render-graph hook |

## Pipeline (stub)

`jitter → gbuffer (velocity) → resolve → history swap`

- **Jitter** — `TaaJitter::currentNdcOffset()` feeds the projection matrix each frame
- **History** — two RGBA16F targets allocated via `ResourceManager`
- **Resolve** — validates surfaces, updates stats, swaps history; kernel deferred
- **Graph** — `addTaaPassToGraph()` inserts the `taa_resolve` pass (also scheduled by `DeferredFramePipeline`)

## Tests

`fuse_taa_pass` (`ctest` name `fuse_taa_pass`) covers Halton jitter, history ping-pong, resolve validation, `TaaPass` lifecycle, and render-graph registration.

## Build

Part of `fuse_rhi`. Built with `FUSE_BUILD_CORE=ON`. Tests run when `FUSE_BUILD_CORE_TESTS=ON`.
