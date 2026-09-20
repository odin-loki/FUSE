#include <fuse/alloc/new_ban.hpp>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct BanHit {
    int count = 0;
    std::string lastKind;
};

void recordBan(const char* kind, void* userData) {
    auto* hit = static_cast<BanHit*>(userData);
    hit->count += 1;
    hit->lastKind = kind != nullptr ? kind : "";
}

void testHeapGuardOffAllowsVector() {
    fuse::alloc::clearBanHandler();

    BanHit hit;
    fuse::alloc::setBanHandler(recordBan, &hit);

    std::vector<int> values;
    values.resize(32);
    values[0] = 7;
    expectTrue(values.size() == 32u, "std::vector works with heap guard off");
    expectTrue(values[0] == 7, "vector storage is usable");
    expectTrue(hit.count == 0, "std::vector does not trip the engine ban when guard is off");
    expectTrue(!fuse::alloc::isEngineHeapGuardArmed(), "guard is disarmed by default");

    fuse::alloc::clearBanHandler();
}

void testArmedGuardFiresBanHandler() {
    fuse::alloc::clearBanHandler();

    BanHit hit;
    fuse::alloc::setBanHandler(recordBan, &hit);

    void* ptr = nullptr;
    {
        fuse::alloc::HeapGuard guard;
        expectTrue(fuse::alloc::isEngineHeapGuardArmed(), "HeapGuard arms the thread-local flag");
        ptr = fuse::alloc::invokeBannedOperatorNew(16u);
    }

    expectTrue(!fuse::alloc::isEngineHeapGuardArmed(), "HeapGuard restores the previous flag");
    expectTrue(ptr != nullptr, "probe still returns storage after reporting");

#if defined(FUSE_DEBUG) && FUSE_DEBUG
    expectTrue(hit.count >= 1, "ban handler records a hit when guard is armed");
    expectTrue(hit.lastKind == "operator new", "ban kind is operator new");
#else
    expectTrue(hit.count == 0, "ban path is compiled out in non-debug builds");
#endif

    ::operator delete(ptr);
    fuse::alloc::clearBanHandler();
}

void testCheckedMallocHooks() {
    fuse::alloc::clearBanHandler();

    BanHit hit;
    fuse::alloc::setBanHandler(recordBan, &hit);

    {
        fuse::alloc::HeapGuard guard;
        void* ptr = fuse::alloc::checkedMalloc(8u);
        expectTrue(ptr != nullptr, "checkedMalloc returns CRT storage");
        fuse::alloc::checkedFree(ptr);
    }

#if defined(FUSE_DEBUG) && FUSE_DEBUG
    expectTrue(hit.count >= 1, "checkedMalloc reports through the ban handler");
#else
    expectTrue(hit.count == 0, "checkedMalloc is quiet in non-debug builds");
#endif

    fuse::alloc::clearBanHandler();
}

} // namespace

int main() {
    testHeapGuardOffAllowsVector();
    testArmedGuardFiresBanHandler();
    testCheckedMallocHooks();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_new_ban: all tests passed\n");
    return EXIT_SUCCESS;
}
