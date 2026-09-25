// Shipping strip probe for the B7.10 row "Shipping build compiles with zero warnings, zero debug
// code included — verified by binary inspection". Compiled with the FUSE_SHIPPING definitions and
// warnings as errors after every fuse_core public header (b7_shipping_headers.cpp, generated);
// check_b7_shipping_strip.cmake then inspects the object: no assert/profiler calls, no stripped log
// format strings or assert messages, while the fatal log path is kept.
#include <fuse/assert.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/profiler/profiler.hpp>

extern "C" int fuse_b7_shipping_probe(int value) {
    FUSE_PROFILE_SCOPE("b7.shipping.scope");
    FUSE_ASSERT(value > 0, "b7 shipping assert message");
    FUSE_VERIFY(value < 1000000, "b7 shipping verify message");
    FUSE_LOG_TRACE("b7 shipping trace %d", value);
    FUSE_LOG_DEBUG("b7 shipping debug %d", value);
    FUSE_LOG_INFO("b7 shipping info %d", value);
    FUSE_LOG_WARN("b7 shipping warn %d", value);
    FUSE_LOG_ERROR("b7 shipping error %d", value);
    const int doubled = value * 2;
    if (doubled < 0) {
        FUSE_LOG_FATAL("b7 shipping fatal kept %d", doubled);
    }
    return doubled;
}
