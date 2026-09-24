#pragma once

// WP-0.6 (docs/unification/RENDERER-EXECUTION.md): optional Tracy backend for the FUSE profiler
// macros. Included at the end of <fuse/profiler/profiler.hpp>; do not include it directly.
//
//   FUSE_TRACY=1 (cmake -DFUSE_TRACY=ON, cmake/FuseTracy.cmake; ON in the fuse-profile preset)
//     FUSE_PROFILE_SCOPE(name)            Chrome-trace ring scope + a Tracy CPU zone (static source
//                                         location per call site; a name that differs from the
//                                         call site's first one is sent as the zone name)
//     FUSE_PROFILE_COUNTER[_SNAPSHOT_AT_FRAME](track, value)
//                                         ring counter sample + a Tracy plot
//     FUSE_PROFILE_FRAME_MARK() / FUSE_PROFILE_FRAME_MARK_NAMED(name)
//                                         Tracy frame mark (the renderer's GpuProfiler::endFrame
//                                         emits one per frame)
//     Async flows and FUSE_PROFILE_GPU/CUDA_* keep their Chrome-trace-only behaviour; GPU zones
//     come from fuse::renderer::GpuProfiler (TracyVk).
//   otherwise (default, and always under FUSE_NO_PROFILER)
//     every macro expands exactly as before: no Tracy header, symbol, call or storage. The new
//     frame-mark macros expand to nothing. Checked by fuse_core_profiler_tracy_zero_overhead
//     (compile-time macro expansion + nm) and fuse_core_profiler_tracy_on (the enabled build).
//
// Tracy is built with TRACY_ON_DEMAND: without a connected server, zones cost one connection check
// and nothing is buffered. Names follow the Chrome-trace ring's contract (static storage).

#include <fuse/profiler/profiler.hpp> // no-op when included from profiler.hpp (#pragma once)
#include <fuse/types.hpp>

namespace fuse::profiler {

#if defined(FUSE_TRACY) && FUSE_TRACY && !(defined(FUSE_NO_PROFILER) && FUSE_NO_PROFILER)
#define FUSE_PROFILER_TRACY_ACTIVE 1
inline constexpr bool kTracyEnabled = true;
#else
#define FUSE_PROFILER_TRACY_ACTIVE 0
inline constexpr bool kTracyEnabled = false;
#endif

} // namespace fuse::profiler

#if FUSE_PROFILER_TRACY_ACTIVE

#include <tracy/TracyC.h>

#include <type_traits>

namespace fuse::profiler::tracy_adapter {

/// One Tracy CPU zone. `location` must have static storage (the macro's per-call-site static).
class Zone {
public:
    Zone(const ___tracy_source_location_data* location, const char* name) noexcept;
    ~Zone();

    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;

private:
    TracyCZoneCtx m_ctx;
};

void frameMark() noexcept;
/// `name` must have static storage (Tracy identifies frame sets by pointer).
void frameMarkNamed(const char* name) noexcept;
/// `track` must have static storage (Tracy identifies plots by pointer).
void plot(const char* track, f64 value) noexcept;
void plot(const char* track, s64 value) noexcept;
/// Copied; any lifetime.
void message(const char* text) noexcept;
void setThreadName(const char* name) noexcept;
/// True while a Tracy server is connected (zones are only recorded then: TRACY_ON_DEMAND).
bool connected() noexcept;
/// "0.14.1": the vendored client version (Engine/lib/tracy/VERSION).
const char* clientVersion() noexcept;

template <typename T>
inline void counterDispatch(const char* track, T value) {
    ::fuse::profiler::sampleCounterDispatch(track, value);
    if constexpr (std::is_floating_point_v<T>) {
        plot(track, static_cast<f64>(value));
    } else {
        plot(track, static_cast<s64>(value));
    }
}

template <typename T>
inline void counterSnapshotAtFrameDispatch(const char* track, T value) {
    ::fuse::profiler::sampleCounterSnapshotAtFrameDispatch(track, value);
    if constexpr (std::is_floating_point_v<T>) {
        plot(track, static_cast<f64>(value));
    } else {
        plot(track, static_cast<s64>(value));
    }
}

} // namespace fuse::profiler::tracy_adapter

// `name` is evaluated once. The call site's static source location keeps the first name it saw.
#undef FUSE_PROFILE_SCOPE_IMPL2
#define FUSE_PROFILE_SCOPE_IMPL2(line, name)                                                             \
    const char* const _fuse_profile_name_##line = (name);                                                \
    ::fuse::profiler::ProfileScope _fuse_profile_scope_##line(_fuse_profile_name_##line);                 \
    static const ___tracy_source_location_data _fuse_tracy_location_##line{                              \
        _fuse_profile_name_##line, __func__, __FILE__, static_cast<::fuse::u32>(line), 0u};               \
    const ::fuse::profiler::tracy_adapter::Zone _fuse_tracy_zone_##line(&_fuse_tracy_location_##line,     \
                                                                        _fuse_profile_name_##line)
#undef FUSE_PROFILE_COUNTER
#define FUSE_PROFILE_COUNTER(track, value) ::fuse::profiler::tracy_adapter::counterDispatch(track, value)
#undef FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME
#define FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME(track, value) \
    ::fuse::profiler::tracy_adapter::counterSnapshotAtFrameDispatch(track, value)
#define FUSE_PROFILE_FRAME_MARK() ::fuse::profiler::tracy_adapter::frameMark()
#define FUSE_PROFILE_FRAME_MARK_NAMED(name) ::fuse::profiler::tracy_adapter::frameMarkNamed(name)

#else

#define FUSE_PROFILE_FRAME_MARK() static_cast<void>(0)
#define FUSE_PROFILE_FRAME_MARK_NAMED(name) static_cast<void>(sizeof(name))

#endif
