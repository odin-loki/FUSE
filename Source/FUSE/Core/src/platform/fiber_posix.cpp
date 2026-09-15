#include <fuse/platform/fiber.hpp>

#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__)
#define FUSE_HAS_UCONTEXT 1
#include <ucontext.h>
#endif

#if defined(FUSE_HAS_UCONTEXT)

#include <cstdlib>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define FUSE_FIBER_ASAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define FUSE_FIBER_ASAN 1
#endif

#if FUSE_FIBER_ASAN
extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, size_t* size_old);
}
#endif

namespace fuse::platform {

struct FiberContext {
    ucontext_t ctx{};
    std::vector<char> stack;
    void (*entry)(void*) = nullptr;
    void* userData = nullptr;
    bool captured = false;
#if FUSE_FIBER_ASAN
    void* asanFakeStack = nullptr;
#endif
};

namespace {

thread_local FiberContext* g_pendingFiberStart = nullptr;

void fiberEntryStub() {
    FiberContext* self = g_pendingFiberStart;
    if (!self) {
        std::abort();
    }
    g_pendingFiberStart = nullptr;
    if (self->entry) {
        self->entry(self->userData);
    }
}

#if FUSE_FIBER_ASAN
void asanBeforeSwitch(FiberContext* to) {
    void* fakeStack = nullptr;
    if (to && !to->stack.empty()) {
        __sanitizer_start_switch_fiber(&fakeStack, to->stack.data(), to->stack.size());
    } else {
        __sanitizer_start_switch_fiber(&fakeStack, nullptr, 0);
    }
    if (to) {
        to->asanFakeStack = fakeStack;
    }
}

void asanAfterSwitch(FiberContext* from) {
    const void* oldBottom = nullptr;
    size_t oldSize = 0;
    __sanitizer_finish_switch_fiber(from ? from->asanFakeStack : nullptr, &oldBottom, &oldSize);
    (void)oldBottom;
    (void)oldSize;
}
#endif

} // namespace

bool cooperativeFibersAvailable() {
    return true;
}

FiberContext* fiberAllocateContext() {
    return new FiberContext();
}

FiberContext* fiberCreate(u32 stackBytes, void (*entry)(void* userData), void* userData) {
    if (stackBytes < 4096u) {
        stackBytes = 4096u;
    }

    auto* fiber = new FiberContext();
    fiber->stack.resize(stackBytes);
    fiber->entry = entry;
    fiber->userData = userData;

    if (getcontext(&fiber->ctx) != 0) {
        delete fiber;
        return nullptr;
    }

    fiber->ctx.uc_stack.ss_sp = fiber->stack.data();
    fiber->ctx.uc_stack.ss_size = fiber->stack.size();
    fiber->ctx.uc_link = nullptr;

    g_pendingFiberStart = fiber;
    makecontext(&fiber->ctx, reinterpret_cast<void (*)()>(fiberEntryStub), 0);
    return fiber;
}

void fiberCaptureCurrent(FiberContext* ctx) {
    if (getcontext(&ctx->ctx) != 0) {
        std::abort();
    }
    ctx->captured = true;
}

void fiberSwap(FiberContext* from, FiberContext* to) {
    if (!from || !to) {
        std::abort();
    }

#if FUSE_FIBER_ASAN
    asanBeforeSwitch(to);
#endif

    if (swapcontext(&from->ctx, &to->ctx) != 0) {
        std::abort();
    }

#if FUSE_FIBER_ASAN
    asanAfterSwitch(from);
#endif
}

void fiberDestroy(FiberContext* ctx) {
    delete ctx;
}

} // namespace fuse::platform

#else // !FUSE_HAS_UCONTEXT

namespace fuse::platform {

bool cooperativeFibersAvailable() {
    return false;
}

FiberContext* fiberAllocateContext() {
    return nullptr;
}

FiberContext* fiberCreate(u32 /*stackBytes*/, void (* /*entry*/)(void*), void* /*userData*/) {
    return nullptr;
}

void fiberCaptureCurrent(FiberContext* /*ctx*/) {}

void fiberSwap(FiberContext* /*from*/, FiberContext* /*to*/) {}

void fiberDestroy(FiberContext* /*ctx*/) {}

} // namespace fuse::platform

#endif
