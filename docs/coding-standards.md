# Coding standards

These rules apply to product code under `Source/FUSE/` and `Tools/FUSE/`.

## Language

- Host: ISO C++23 is the product target. The umbrella currently compiles as C++17 while that lands.
- Allowed: concepts, `std::expected`, `std::optional`, `std::span`, `std::string_view`, `[[nodiscard]]`, aggressive `constexpr` on math and handles.
- `std::format` / `std::print` for tools. The engine logger stays custom so shipping builds can strip it.
- Ranges on non-hot paths. Coroutines only inside the job/fiber layer.
- CUDA device code may remain a C++20 dialect if the toolkit requires it.

## Ownership and memory

| Pattern | Rule |
|---------|------|
| Owning raw `T*` in public APIs | Ban — use `fuse::Handle`, allocator + size, or tooling-only `unique_ptr` |
| `new` / `delete` / `malloc` in engine libs | Ban after the allocator milestone; debug overload asserts |
| Exceptions across engine ↔ script ↔ Qt | No. `std::expected` / error codes. Qt may throw at the UI edge only |
| Unbounded C arrays of objects | Prefer `span` + sized buffers |
| Silent narrowing | `-Wconversion` on FUSE-owned code in CI |
| `#include <Q*>` in engine libs | Hard ban |

Prefer `Handle` / `HandleMap` for long-lived objects (scene, resources, GPU). Route allocations through `fuse/alloc/`.

## Threading

- Game thread owns mutation of `fuse::Object` and script callbacks that touch scene state.
- Workers use `fuse::Handle<T>` and snapshots. No raw scene pointers across job boundaries.
- No ad-hoc `std::thread` in product code. Submit to `JobScheduler`.
- `FUSE_JOBS_SINGLE_THREAD` must match parallel behaviour modulo timing.

## Names

- Namespace `fuse` (nested: `fuse::jobs`, `fuse::script`, …).
- Macros `FUSE_*` (`FUSE_ASSERT`, `FUSE_HOST_DEVICE`, …).
- CMake options and targets `FUSE_*` / `fuse_*`.
- Public headers under `include/fuse/`.

## Tests and honesty

- New behaviour gets a `ctest` target named `fuse_*`.
- If a GPU or renderer path is a stub, say so in the header and the PR. Do not pretend present is real.
- Do not merge if ASan/UBSan smoke is red on the paths you touched.

## Editor

Qt owns chrome. The engine owns the viewport. UI posts commands; the game thread drains them. See [editor.md](editor.md).
