#include <fuse/platform/fiber.hpp>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>

namespace fuse::platform {

struct FiberContext {
    void* fiberHandle = nullptr;
    void (*entry)(void*) = nullptr;
    void* userData = nullptr;
    bool captured = false;
};

namespace {

void WINAPI fiberEntryStub(void* param) {
    auto* self = static_cast<FiberContext*>(param);
    if (!self) {
        std::abort();
    }
    if (self->entry) {
        self->entry(self->userData);
    }
}

} // namespace

bool cooperativeFibersAvailable() {
    return true;
}

const char* fiberBackendName() {
    return "win32";
}

FiberContext* fiberAllocateContext() {
    return new FiberContext();
}

FiberContext* fiberCreate(u32 stackBytes, void (*entry)(void* userData), void* userData) {
    if (stackBytes < 4096u) {
        stackBytes = 4096u;
    }

    auto* fiber = new FiberContext();
    fiber->entry = entry;
    fiber->userData = userData;

    fiber->fiberHandle = CreateFiber(static_cast<SIZE_T>(stackBytes), fiberEntryStub, fiber);
    if (!fiber->fiberHandle) {
        delete fiber;
        return nullptr;
    }

    return fiber;
}

void fiberCaptureCurrent(FiberContext* ctx) {
    if (!ctx->fiberHandle) {
        ctx->fiberHandle = ConvertThreadToFiber(nullptr);
        if (!ctx->fiberHandle) {
            std::abort();
        }
    }
    ctx->captured = true;
}

void fiberSwap(FiberContext* from, FiberContext* to) {
    if (!from || !to || !to->fiberHandle) {
        std::abort();
    }
    SwitchToFiber(to->fiberHandle);
}

void fiberDestroy(FiberContext* ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->captured) {
        if (ctx->fiberHandle) {
            ConvertFiberToThread();
            ctx->fiberHandle = nullptr;
        }
    } else if (ctx->fiberHandle) {
        DeleteFiber(ctx->fiberHandle);
        ctx->fiberHandle = nullptr;
    }

    delete ctx;
}

} // namespace fuse::platform

#endif // _WIN32
