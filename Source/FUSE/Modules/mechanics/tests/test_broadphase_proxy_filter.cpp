#include <fuse/mechanics/broadphase_proxy_filter.hpp>

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
    using fuse::mechanics::BroadphaseProxyFilter;
    using fuse::mechanics::broadphaseProxyFiltersCollide;

    expectTrue(broadphaseProxyFiltersCollide(BroadphaseProxyFilter::Character, BroadphaseProxyFilter::Trigger),
               "character collides with trigger proxy");
    expectTrue(!broadphaseProxyFiltersCollide(BroadphaseProxyFilter::StaticRigid, BroadphaseProxyFilter::Trigger),
               "static rigid ignores trigger proxy");
    expectTrue(broadphaseProxyFiltersCollide(BroadphaseProxyFilter::Default, BroadphaseProxyFilter::Trigger),
               "default filter collides with trigger");

    const fuse::mechanics::BroadphaseProxyGroupMask characterGroup =
        fuse::mechanics::broadphaseProxyGroupMask(BroadphaseProxyFilter::Character);
    const fuse::mechanics::BroadphaseProxyGroupMask triggerGroup =
        fuse::mechanics::broadphaseProxyGroupMask(BroadphaseProxyFilter::Trigger);
    expectTrue(fuse::mechanics::broadphaseProxyMasksCollide(characterGroup, triggerGroup, triggerGroup, characterGroup),
               "btBroadphaseProxy-style group/mask collision accepts character vs trigger");
    expectTrue(!fuse::mechanics::broadphaseProxyMasksCollide(
                   fuse::mechanics::broadphaseProxyGroupMask(BroadphaseProxyFilter::StaticRigid),
                   triggerGroup,
                   triggerGroup,
                   characterGroup),
               "static rigid group/mask rejects trigger");

    if (g_failures == 0) {
        std::printf("fuse_mechanics_broadphase_proxy_filter: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_broadphase_proxy_filter: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
