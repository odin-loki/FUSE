# Track B — Core B1.6 Logging, Assert & Profiler (stubs)

**Status:** B1.6 CPU profiler ring buffer, chrome-trace export stub, assert macros, and fatal handler hooks on `fuse_core`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B1.6  
**Related:** Editor `ProfilerPanel` ring buffer (B6.10) consumes frame summaries; this PR owns per-scope CPU events in Core.

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `assert.hpp` | `Source/FUSE/Core/include/fuse/` | `FUSE_ASSERT`, `FUSE_VERIFY`, contract helpers |
| `fatal_handler.cpp` | `Source/FUSE/Core/src/assert/` | Fatal log + callback hook + `std::abort()` |
| `profiler.hpp` | `Source/FUSE/Core/include/fuse/profiler/` | `ProfileScope`, frame ring buffer, chrome JSON stub |
| `profiler.cpp` | `Source/FUSE/Core/src/profiler/` | 4096-event ring buffer, steady-clock timestamps |
| `test_profiler_assert.cpp` | `Source/FUSE/Core/tests/` | Scope, export, fatal hook acceptance |

**Not in scope (follow-up PRs):** lock-free async logger ring, GPU/CUDA markers, Tracy backend, shipping strip presets (`FUSE_NO_ASSERT` / `FUSE_NO_PROFILER` macros exist for future CMake presets).

---

## Build

Profiler and assert ship inside `fuse_core`:

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build --target fuse_core_profiler_assert_tests
ctest --test-dir build -R fuse_core_profiler_assert
```

| Macro | When | Effect |
|-------|------|--------|
| `FUSE_DEBUG=1` | Debug config | `FUSE_ASSERT` active (alias of `FUSE_VERIFY`) |
| `FUSE_PROFILE=1` | RelWithDebInfo | Reserved for future Tracy / GPU correlation |
| `FUSE_NO_ASSERT=1` | Shipping preset (future) | Strips assert macros |
| `FUSE_NO_PROFILER=1` | Shipping preset (future) | Strips `FUSE_PROFILE_SCOPE` |

---

## Assert macros

```cpp
#include <fuse/assert.hpp>

FUSE_ASSERT(ptr != nullptr, "pointer must be valid");   // debug builds only
FUSE_VERIFY(index < count, "index out of range");        // always on (unless FUSE_NO_ASSERT)
FUSE_PRECONDITION(queue.empty());
FUSE_POSTCONDITION(result.isValid());
```

Fatal path:

1. `fuse::log::Logger` records `Level::Fatal`
2. Registered `FatalHandlerFn` runs (crash reporter, minidump stub, tests)
3. `std::abort()` unless `setSuppressAbortForTests(true)`

```cpp
fuse::assertion::setFatalHandler(
  [](const fuse::assertion::FatalContext& ctx, void*) {
    fuse::platform::submitCrashReport({ctx.message, ctx.file, ctx.line});
  });
```

Integrates with B7.8 `crash_report.hpp` — OS signal wiring remains a follow-up.

---

## CPU profiler

```cpp
#include <fuse/profiler/profiler.hpp>

void tick() {
  fuse::profiler::beginFrame();
  {
    FUSE_PROFILE_SCOPE("physics");
    // ...
  }
  fuse::profiler::endFrame();
}

const std::string trace = fuse::profiler::exportChromeTraceJson();
// Load in chrome://tracing — stub emits valid {"traceEvents":[...]} JSON
```

Ring buffer holds up to 4096 `ProfileEvent` records (name, timestamp ns, begin/end phase, thread id). `setEnabled(false)` makes `FUSE_PROFILE_SCOPE` a no-op without recompiling.

---

## Tests

| Test binary | CTest name | Coverage |
|-------------|------------|----------|
| `fuse_core_profiler_assert_tests` | `fuse_core_profiler_assert` | Scope begin/end, disable flag, frame index, chrome JSON, fatal hook, `FUSE_VERIFY` |

---

## Gates (B1.6)

- [x] Frame profiler scopes (CPU) with ring buffer on FUSE APIs
- [x] chrome://tracing JSON export stub
- [x] `FUSE_ASSERT` / `FUSE_VERIFY` macros + fatal handler hooks
- [x] Unit tests + this doc
- [ ] Lock-free async logger ring (B1.6 logging follow-up)
- [ ] GPU / CUDA profile markers (B2+)
