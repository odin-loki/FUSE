#include <fuse/legacy/t2d/api.hpp>
#include <fuse/log/logger.hpp>

#include <cstdint>

extern "C" std::uint32_t fuse_t2d_StringTable_intern(const char* value);

namespace fuse::legacy::t2d {

namespace {
bool g_initialized = false;
}

bool initialize() {
    if (g_initialized) {
        return true;
    }

    fuse_t2d_StringTable_intern("FUSE_T2D_BOOT");
    Con::init();
    Con::addPathExpando("game", "/game");
    Con::setVariable("$FuseT2D", "1");
    Con::execute("legacyBoot();");
    fuse::log::info("[t2d] dimension initialized");
    g_initialized = true;
    return true;
}

void shutdown() {
    g_initialized = false;
}

bool isInitialized() {
    return g_initialized;
}

} // namespace fuse::legacy::t2d
