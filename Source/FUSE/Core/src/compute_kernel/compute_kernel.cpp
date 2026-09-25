#include <fuse/compute_kernel/load_scale.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/jobs/cuda_jobs.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace fuse::kernel {

// ---- backends ----------------------------------------------------------------------------------

const char* backend_name(Backend backend) {
    switch (backend) {
    case Backend::Auto:
        return "auto";
    case Backend::CpuReference:
        return "cpu_reference";
    case Backend::CpuParallel:
        return "cpu_parallel";
    case Backend::Cuda:
        return "cuda";
    case Backend::VulkanCompute:
        return "vulkan_compute";
    }
    return "unknown";
}

bool backend_available(Backend backend) {
    switch (backend) {
    case Backend::Auto:
    case Backend::CpuReference:
    case Backend::CpuParallel:
        return true;
    case Backend::Cuda:
#if defined(FUSE_HAS_CUDA)
        return jobs::cudaJobsAvailable(); // toolkit compiled in AND a device initialised
#else
        return false;
#endif
    case Backend::VulkanCompute:
        return false;
    }
    return false;
}

// ---- stats registry ----------------------------------------------------------------------------

namespace {

struct Registry {
    std::mutex mutex;
    KernelStats entries[kMaxTrackedKernels];
    u32 count = 0;
    u64 totalLaunches = 0;
    u64 dropped = 0;
    LaunchRecord last{};
};

Registry& registry() {
    static Registry instance;
    return instance;
}

bool sameName(const char* a, const char* b) {
    return a == b || (a != nullptr && b != nullptr && std::strcmp(a, b) == 0);
}

} // namespace

void record_launch(const LaunchRecord& record) {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    ++r.totalLaunches;
    r.last = record;
    if (record.name == nullptr) {
        ++r.dropped;
        return;
    }
    KernelStats* stats = nullptr;
    for (u32 i = 0; i < r.count; ++i) {
        if (sameName(r.entries[i].name, record.name)) {
            stats = &r.entries[i];
            break;
        }
    }
    if (stats == nullptr) {
        if (r.count == kMaxTrackedKernels) {
            ++r.dropped;
            return;
        }
        stats = &r.entries[r.count++];
        *stats = KernelStats{};
        stats->name = record.name;
        stats->min_ns = record.duration_ns;
        stats->max_ns = record.duration_ns;
    }
    ++stats->launches;
    if (!record.ok) {
        ++stats->failed_launches;
    }
    stats->items += record.items;
    stats->workgroups += record.workgroups;
    stats->total_ns += record.duration_ns;
    stats->last_ns = record.duration_ns;
    stats->min_ns = record.duration_ns < stats->min_ns ? record.duration_ns : stats->min_ns;
    stats->max_ns = record.duration_ns > stats->max_ns ? record.duration_ns : stats->max_ns;
    stats->last_backend = record.backend;
    const u32 backendIndex = static_cast<u32>(record.backend);
    if (backendIndex < kBackendCount) {
        ++stats->launches_by_backend[backendIndex];
    }
}

bool find_kernel_stats(const char* name, KernelStats& out) {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    for (u32 i = 0; i < r.count; ++i) {
        if (sameName(r.entries[i].name, name)) {
            out = r.entries[i];
            return true;
        }
    }
    return false;
}

u32 kernel_stats_count() {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    return r.count;
}

bool kernel_stats_at(u32 index, KernelStats& out) {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    if (index >= r.count) {
        return false;
    }
    out = r.entries[index];
    return true;
}

LaunchRecord last_launch() {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    return r.last;
}

u64 total_launch_count() {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    return r.totalLaunches;
}

u64 kernel_stats_dropped() {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    return r.dropped;
}

void reset_kernel_stats() {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    r.count = 0;
    r.totalLaunches = 0;
    r.dropped = 0;
    r.last = LaunchRecord{};
}

// ---- load scale --------------------------------------------------------------------------------

namespace {

std::mutex g_loadScaleMutex;
LoadScale g_loadScale{};

f32 sanitize(f32 v) { return std::isfinite(v) && v > 0.f ? v : 0.f; }

bool parseNumber(const char* begin, const char* end, f32& out) {
    char buffer[32];
    const std::size_t len = static_cast<std::size_t>(end - begin);
    if (len == 0 || len >= sizeof(buffer)) {
        return false;
    }
    std::memcpy(buffer, begin, len);
    buffer[len] = '\0';
    char* parsedEnd = nullptr;
    const double value = std::strtod(buffer, &parsedEnd);
    if (parsedEnd != buffer + len || !std::isfinite(value) || value < 0.0) {
        return false;
    }
    out = static_cast<f32>(value);
    return true;
}

bool keyIs(const char* begin, const char* end, const char* key) {
    const std::size_t len = static_cast<std::size_t>(end - begin);
    return std::strlen(key) == len && std::strncmp(begin, key, len) == 0;
}

} // namespace

LoadScale load_scale() {
    const std::lock_guard<std::mutex> lock(g_loadScaleMutex);
    return g_loadScale;
}

void set_load_scale(const LoadScale& scale) {
    const std::lock_guard<std::mutex> lock(g_loadScaleMutex);
    g_loadScale = LoadScale{sanitize(scale.resolution), sanitize(scale.objects), sanitize(scale.probes),
                            sanitize(scale.lights)};
}

u32 scaled_count(u32 base, f32 scale) {
    if (base == 0u || !(scale > 0.f) || !std::isfinite(scale)) {
        return 0u;
    }
    const double v = std::round(static_cast<double>(base) * static_cast<double>(scale));
    if (v >= 4294967295.0) {
        return 0xffffffffu;
    }
    return v < 1.0 ? 1u : static_cast<u32>(v);
}

u32 scaled_extent(u32 base, f32 scale) {
    const u32 v = scaled_count(base, scale);
    return v == 0u ? 1u : v;
}

bool parse_load_scale(const char* text, LoadScale& inOut) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    LoadScale parsed = inOut;
    const char* end = text + std::strlen(text);
    if (std::strchr(text, '=') == nullptr) {
        f32 all = 0.f;
        if (!parseNumber(text, end, all)) {
            return false;
        }
        inOut = LoadScale{all, all, all, all};
        return true;
    }
    const char* cursor = text;
    while (cursor < end) {
        const char* comma = cursor;
        while (comma < end && *comma != ',') {
            ++comma;
        }
        const char* eq = cursor;
        while (eq < comma && *eq != '=') {
            ++eq;
        }
        if (eq == comma) {
            return false;
        }
        f32 value = 0.f;
        if (!parseNumber(eq + 1, comma, value)) {
            return false;
        }
        if (keyIs(cursor, eq, "res") || keyIs(cursor, eq, "resolution")) {
            parsed.resolution = value;
        } else if (keyIs(cursor, eq, "objects")) {
            parsed.objects = value;
        } else if (keyIs(cursor, eq, "probes")) {
            parsed.probes = value;
        } else if (keyIs(cursor, eq, "lights")) {
            parsed.lights = value;
        } else {
            return false;
        }
        cursor = comma < end ? comma + 1 : end;
    }
    inOut = parsed;
    return true;
}

bool load_scale_from_env() {
    const char* text = std::getenv("FUSE_LOAD_SCALE");
    LoadScale scale = load_scale();
    if (text == nullptr || !parse_load_scale(text, scale)) {
        return false;
    }
    set_load_scale(scale);
    return true;
}

} // namespace fuse::kernel
