#define FUSE_NEW_BAN_NO_KEYWORD_INTERCEPT

#include <fuse/alloc/new_ban.hpp>

#include <fuse/assert.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

namespace fuse::alloc {

#if defined(FUSE_DEBUG) && FUSE_DEBUG
thread_local bool engineHeapGuard = false;
#endif

namespace {

std::atomic<BanHandlerFn> g_banHandler{nullptr};
std::atomic<void*> g_banUserData{nullptr};

} // namespace

HeapGuard::HeapGuard()
    : m_previous(isEngineHeapGuardArmed()) {
#if defined(FUSE_DEBUG) && FUSE_DEBUG
    engineHeapGuard = true;
#endif
}

HeapGuard::~HeapGuard() {
#if defined(FUSE_DEBUG) && FUSE_DEBUG
    engineHeapGuard = m_previous;
#endif
}

void setBanHandler(BanHandlerFn handler, void* userData) {
    g_banHandler.store(handler, std::memory_order_release);
    g_banUserData.store(userData, std::memory_order_release);
}

void clearBanHandler() {
    g_banHandler.store(nullptr, std::memory_order_release);
    g_banUserData.store(nullptr, std::memory_order_release);
}

bool isEngineHeapGuardArmed() {
#if defined(FUSE_DEBUG) && FUSE_DEBUG
    return engineHeapGuard;
#else
    return false;
#endif
}

void checkEngineHeapBan(const char* kind) {
#if defined(FUSE_DEBUG) && FUSE_DEBUG
    if (!engineHeapGuard) {
        return;
    }

    BanHandlerFn handler = g_banHandler.load(std::memory_order_acquire);
    if (handler != nullptr) {
        handler(kind != nullptr ? kind : "heap", g_banUserData.load(std::memory_order_acquire));
        return;
    }

    fuse::assertion::fatal(
        kind != nullptr ? kind : "naked heap operation in engine TU",
        __FILE__,
        static_cast<u32>(__LINE__));
#else
    (void)kind;
#endif
}

void* checkedMalloc(std::size_t size) {
    checkEngineHeapBan("malloc");
    return std::malloc(size);
}

void checkedFree(void* ptr) {
    checkEngineHeapBan("free");
    std::free(ptr);
}

void* invokeBannedOperatorNew(std::size_t size) {
    checkEngineHeapBan("operator new");
    return ::operator new(size);
}

void invokeBannedOperatorDelete(void* ptr) noexcept {
    checkEngineHeapBan("operator delete");
    ::operator delete(ptr);
}

} // namespace fuse::alloc
