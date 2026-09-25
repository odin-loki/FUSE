// WP-0.6: Tracy backend of the FUSE profiler macros (see <fuse/profiler/tracy_adapter.hpp>).
// Compiled only into fuse_profiler_tracy (cmake/FuseTracy.cmake), which defines FUSE_TRACY=1 and
// TRACY_ENABLE; a default build never compiles this file.
#include <fuse/profiler/profiler.hpp>

#if !FUSE_PROFILER_TRACY_ACTIVE
#error "tracy_adapter.cpp is built only with FUSE_TRACY=1 (fuse_profiler_tracy)"
#endif

#include <common/TracyVersion.hpp>
#include <fuse_tracy_version.h>

#include <cstring>

static_assert(tracy::Version::Major == FUSE_TRACY_VERSION_MAJOR && tracy::Version::Minor == FUSE_TRACY_VERSION_MINOR &&
                  tracy::Version::Patch == FUSE_TRACY_VERSION_PATCH,
              "Engine/lib/tracy: public/common/TracyVersion.hpp disagrees with fuse_tracy_version.h (VERSION pin)");

namespace fuse::profiler::tracy_adapter {

namespace {

#define FUSE_TRACY_STR2(x) #x
#define FUSE_TRACY_STR(x) FUSE_TRACY_STR2(x)
constexpr const char kClientVersion[] = FUSE_TRACY_STR(FUSE_TRACY_VERSION_MAJOR) "." FUSE_TRACY_STR(
    FUSE_TRACY_VERSION_MINOR) "." FUSE_TRACY_STR(FUSE_TRACY_VERSION_PATCH);
#undef FUSE_TRACY_STR
#undef FUSE_TRACY_STR2

} // namespace

Zone::Zone(const ___tracy_source_location_data* location, const char* name) noexcept
    : m_ctx(___tracy_emit_zone_begin(location, 1)) {
    // The call site's location carries the first name it saw; later, different names (a scope fed
    // a runtime string) are attached to this zone instance.
    if (m_ctx.active != 0 && name != nullptr && name != location->name &&
        (location->name == nullptr || std::strcmp(name, location->name) != 0)) {
        ___tracy_emit_zone_name(m_ctx, name, std::strlen(name));
    }
}

Zone::~Zone() {
    ___tracy_emit_zone_end(m_ctx);
}

void frameMark() noexcept {
    ___tracy_emit_frame_mark(nullptr);
}

void frameMarkNamed(const char* name) noexcept {
    ___tracy_emit_frame_mark(name);
}

void plot(const char* track, f64 value) noexcept {
    if (track != nullptr && track[0] != '\0') {
        ___tracy_emit_plot(track, value);
    }
}

void plot(const char* track, s64 value) noexcept {
    if (track != nullptr && track[0] != '\0') {
        ___tracy_emit_plot_int(track, value);
    }
}

void message(const char* text) noexcept {
    if (text != nullptr) {
        ___tracy_emit_logString(TracyMessageSeverityInfo, 0, 0, std::strlen(text), text);
    }
}

void setThreadName(const char* name) noexcept {
    if (name != nullptr) {
        ___tracy_set_thread_name(name);
    }
}

bool connected() noexcept {
    return ___tracy_connected() != 0;
}

const char* clientVersion() noexcept {
    return kClientVersion;
}

} // namespace fuse::profiler::tracy_adapter
