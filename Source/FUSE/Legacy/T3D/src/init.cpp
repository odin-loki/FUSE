#include <fuse/legacy/t3d/api.hpp>

#include <cstdint>

extern "C" std::uint32_t fuse_t3d_StringTable_intern(const char* value);

namespace fuse::legacy::t3d {

namespace {
bool g_initialized = false;
}

bool initialize() {
    if (g_initialized) {
        return true;
    }

    // Explicit init order: string table before console (mirrors T3D startup constraints).
    fuse_t3d_StringTable_intern("FUSE_T3D_BOOT");
    Con::execute("legacyBoot();");
    g_initialized = true;
    return true;
}

void shutdown() {
    g_initialized = false;
}

bool isInitialized() {
    return g_initialized;
}

} // namespace fuse::legacy::t3d
