#include <fuse/core/init.hpp>
#include <fuse/mechanics/bt_dbvt_bridge.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

} // namespace

int main() {
    fuse::core::initialize();

    fuse::mechanics::BtDbvtBridge dbvt;
    fuse::mechanics::BtDbvtProxy character{};
    character.objectId = 1;
    character.proxy = fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Character);
    character.minX = -0.5f;
    character.maxX = 0.5f;
    character.minY = -0.5f;
    character.maxY = 0.5f;
    dbvt.insertProxy(character);

    fuse::mechanics::BtDbvtProxy trigger{};
    trigger.objectId = 2;
    trigger.proxy = fuse::mechanics::makeBroadphaseProxyDesc(fuse::mechanics::BroadphaseProxyFilter::Trigger);
    trigger.minX = 0.f;
    trigger.maxX = 1.f;
    trigger.minY = 0.f;
    trigger.maxY = 1.f;
    dbvt.insertProxy(trigger);

    expectTrue(dbvt.queryOverlaps(fuse::mechanics::BroadphaseProxyFilter::Character,
                                  fuse::mechanics::BroadphaseProxyFilter::Trigger) >= 1u,
               "btDbvt bridge finds character/trigger overlap");
    expectTrue(dbvt.queryAabbOverlaps(-1.f, -1.f, -1.f, 2.f, 2.f, 2.f) >= 1u,
               "btDbvt bridge aabb query finds proxy");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_bt_dbvt_bridge: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_bt_dbvt_bridge: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
