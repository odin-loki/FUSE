#include <fuse/mechanics/broadphase_proxy_filter.hpp>

namespace fuse::mechanics {

bool broadphaseProxyFiltersCollide(BroadphaseProxyFilter a, BroadphaseProxyFilter b) {
    if (a == BroadphaseProxyFilter::Default || b == BroadphaseProxyFilter::Default) {
        return true;
    }
    if (a == BroadphaseProxyFilter::Trigger || b == BroadphaseProxyFilter::Trigger) {
        return a == BroadphaseProxyFilter::Character || b == BroadphaseProxyFilter::Character;
    }
    return a == b;
}

} // namespace fuse::mechanics
