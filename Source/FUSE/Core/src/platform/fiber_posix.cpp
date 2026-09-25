#include <fuse/platform/fiber.hpp>

// ucontext is unavailable on Android NDK (aarch64) and deprecated on iOS.
// Mobile builds use the CV fallback below; cooperativeFibersAvailable() returns false.
#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && \
    !(defined(FUSE_PLATFORM_MOBILE) && FUSE_PLATFORM_MOBILE)
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

// Tell valgrind about fiber stacks so stack switches are not reported as invalid accesses.
// The client-request macros are no-ops when the process is not running under valgrind.
#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define FUSE_FIBER_VALGRIND 1
#endif
#endif

#if FUSE_FIBER_ASAN
#include <pthread.h>
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
    bool started = false;
#if FUSE_FIBER_VALGRIND
    unsigned valgrindStackId = 0;
#endif
#if FUSE_FIBER_ASAN
    void* asanFakeStack = nullptr;
    const void* asanThreadStackBottom = nullptr;
    size_t asanThreadStackSize = 0;
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
#if FUSE_FIBER_ASAN
    // First entry on this stack: complete the switch started in fiberSwap().
    __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
    if (self->entry) {
        self->entry(self->userData);
    }
}

#if FUSE_FIBER_ASAN
// `from` keeps its own fake stack while it is switched out; the target's stack bounds tell ASan
// which stack becomes current (a captured thread context reports its pthread stack bounds).
void asanBeforeSwitch(FiberContext* from, FiberContext* to) {
    const void* bottom = nullptr;
    size_t size = 0;
    if (!to->stack.empty()) {
        bottom = to->stack.data();
        size = to->stack.size();
    } else {
        bottom = to->asanThreadStackBottom;
        size = to->asanThreadStackSize;
    }
    __sanitizer_start_switch_fiber(&from->asanFakeStack, bottom, size);
}

void asanAfterSwitch(FiberContext* from) {
    __sanitizer_finish_switch_fiber(from->asanFakeStack, nullptr, nullptr);
}

void asanCaptureThreadStack(FiberContext* ctx) {
#if defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
            ctx->asanThreadStackBottom = addr;
            ctx->asanThreadStackSize = size;
        }
        pthread_attr_destroy(&attr);
    }
#else
    (void)ctx;
#endif
}
#endif

} // namespace

bool cooperativeFibersAvailable() {
    return true;
}

const char* fiberBackendName() {
    return "posix-ucontext";
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
#if FUSE_FIBER_VALGRIND
    fiber->valgrindStackId =
        VALGRIND_STACK_REGISTER(fiber->stack.data(), fiber->stack.data() + fiber->stack.size());
#endif
    fiber->entry = entry;
    fiber->userData = userData;

    if (getcontext(&fiber->ctx) != 0) {
        delete fiber;
        return nullptr;
    }

    fiber->ctx.uc_stack.ss_sp = fiber->stack.data();
    fiber->ctx.uc_stack.ss_size = fiber->stack.size();
    fiber->ctx.uc_link = nullptr;

    makecontext(&fiber->ctx, reinterpret_cast<void (*)()>(fiberEntryStub), 0);
    return fiber;
}

void fiberCaptureCurrent(FiberContext* ctx) {
    if (getcontext(&ctx->ctx) != 0) {
        std::abort();
    }
    ctx->captured = true;
#if FUSE_FIBER_ASAN
    asanCaptureThreadStack(ctx);
#endif
}

void fiberSwap(FiberContext* from, FiberContext* to) {
    if (!from || !to) {
        std::abort();
    }

    // Hand the entry context over at first switch-in (not at create time) so several fibers can
    // be created before any of them runs without the last create clobbering the others.
    if (!to->captured && !to->started) {
        to->started = true;
        g_pendingFiberStart = to;
    }

#if FUSE_FIBER_ASAN
    asanBeforeSwitch(from, to);
#endif

    if (swapcontext(&from->ctx, &to->ctx) != 0) {
        std::abort();
    }

#if FUSE_FIBER_ASAN
    asanAfterSwitch(from);
#endif
}

void fiberDestroy(FiberContext* ctx) {
#if FUSE_FIBER_VALGRIND
    if (ctx != nullptr && !ctx->stack.empty()) {
        VALGRIND_STACK_DEREGISTER(ctx->valgrindStackId);
    }
#endif
    delete ctx;
}

} // namespace fuse::platform

#else // !FUSE_HAS_UCONTEXT — Android, iOS, Emscripten, and other non-ucontext targets

namespace fuse::platform {

bool cooperativeFibersAvailable() {
    return false; // JobCounter::wait() uses condition-variable blocking on these platforms
}

const char* fiberBackendName() {
    return "stub";
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
