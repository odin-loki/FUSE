# Track B — Core B1.6 Logging, Assert & Profiler (stubs)

**Status:** B1.6 deepen — async chrome flow begin/end stubs, int/float counter samples, expanded export tests on `fuse_core`  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B1.6  
**Related:** Editor `ProfilerPanel` ring buffer (B6.10) consumes frame summaries; this PR owns per-scope CPU events in Core.

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `assert.hpp` | `Source/FUSE/Core/include/fuse/` | `FUSE_ASSERT`, `FUSE_VERIFY`, contract helpers |
| `fatal_handler.cpp` | `Source/FUSE/Core/src/assert/` | Fatal log + callback hook + `std::abort()` |
| `profiler.hpp` | `Source/FUSE/Core/include/fuse/profiler/` | `ProfileScope`, async flow + counter stubs, chrome JSON export |
| `profiler.cpp` | `Source/FUSE/Core/src/profiler/` | 4096-event ring buffer, steady-clock timestamps |
| `test_profiler_assert.cpp` | `Source/FUSE/Core/tests/` | Scope, async flow, counter samples, export, fatal hook acceptance |

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

const fuse::u32 flowId = fuse::profiler::nextFlowId();
FUSE_PROFILE_ASYNC_FLOW_BEGIN("vfs_load", flowId);
// ... worker thread completes I/O ...
FUSE_PROFILE_ASYNC_FLOW_END("vfs_load", flowId);
FUSE_PROFILE_COUNTER("frame_alloc_bytes", frameAllocator.usedBytes());
FUSE_PROFILE_COUNTER("frame_time_ms", 16.667); // f64 overload for fractional budgets

const std::string trace = fuse::profiler::exportChromeTraceJson();
// Load in chrome://tracing — stub emits valid {"traceEvents":[...]} JSON
```

Ring buffer holds up to 4096 `ProfileEvent` records (name, timestamp ns, phase, chrome `tid`, paired `scopeId` / `flowId`, nesting depth, counter kind + int/float payload). `setEnabled(false)` makes `FUSE_PROFILE_SCOPE`, `FUSE_PROFILE_ASYNC_FLOW_*`, and `FUSE_PROFILE_COUNTER` no-ops without recompiling.

Nested scopes preserve stack order (`outer B → inner B → inner E → outer E`) and track `maxNestingDepth()` for flame-graph scaffolding.

### Thread-id stubs

Profiler events call `fuse::platform::chromeTraceThreadId()` (low 32 bits of `currentThreadId()`). Register the process main thread once at startup so `isMainThread()` resolves correctly in multi-threaded smoke:

```cpp
fuse::platform::registerMainThread();
// worker threads get distinct tid values automatically
```

### chrome://tracing export

`exportChromeTraceJson()` emits a minimal but valid trace document:

| Field | Value |
|-------|-------|
| `displayTimeUnit` | `"ns"` |
| `metadata.name` | `"FUSE CPU profiler"` |
| `traceEvents[].ph` | `"B"` / `"E"` scope begin/end; `"s"` / `"f"` async flow start/finish; `"C"` counter sample |
| `traceEvents[].cat` | `"cpu"` scopes, `"async"` flows, `"counter"` samples |
| `traceEvents[].ts` | microseconds (`timestampNs / 1000`) |
| `traceEvents[].pid` | `1` (single-process stub) |
| `traceEvents[].tid` | `chromeTraceThreadId()` |
| `traceEvents[].id` | paired `scopeId` per `FUSE_PROFILE_SCOPE`, or `flowId` for async flow pairs |
| `traceEvents[].bp` | `"e"` on flow finish — bind to enclosing slice end |
| `traceEvents[].args.depth` | 1-based nesting depth (scope events only) |
| `traceEvents[].args.value` | counter sample payload — integer (`s64`) or float (`f64`) via `FUSE_PROFILE_COUNTER` overload |

Load the JSON in `chrome://tracing` for offline inspection; Qt flame-graph panel (B6.10) will consume the same event buffer later. Async flow stubs correlate I/O and job handoff across threads; counter samples surface allocator and scheduler budgets until GPU markers land (B2+).

---

## Tests

| Test binary | CTest name | Coverage |
|-------------|------------|----------|
| `fuse_core_profiler_assert_tests` | `fuse_core_profiler_assert` | Scope begin/end, nested zone ordering, async flow begin/end (matching `id`), int/float counter samples, nesting depth in chrome export, mixed chrome JSON export, thread-id stubs, disable flag, frame index, fatal hook, `FUSE_VERIFY` |

---

## Gates (B1.6)

- [x] Frame profiler scopes (CPU) with ring buffer on FUSE APIs
- [x] chrome://tracing JSON export stub
- [x] Async flow begin/end stubs (`ph:"s"` / `ph:"f"`) + counter samples (`ph:"C"`)
- [x] `FUSE_ASSERT` / `FUSE_VERIFY` macros + fatal handler hooks
- [x] Unit tests + this doc
- [ ] Lock-free async logger ring (B1.6 logging follow-up)
- [ ] GPU / CUDA profile markers (B2+)
