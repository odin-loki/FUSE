#include <fuse/platform/fiber.hpp>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ASan fiber switches use the same hooks as the POSIX backend
// (__sanitizer_start_switch_fiber / __sanitizer_finish_switch_fiber).
// __asan_handle_no_return is not a fiber annotation: it only clears shadow for the
// current thread stack and would hide bugs without telling ASan about the new stack.
//
// Clang/compiler-rt and the MSVC ASan runtimes checked here (14.44 and 14.51) export
// those hooks. If the linked runtime does not, /alternatename satisfies the link,
// fiberASanAnnotationsActive() stays false, and SwitchToFiber is not annotated.
//
// CreateFiber does not return a stack pointer. The fiber handle begins with the user
// parameter (GetFiberData reads the first pointer), then ExceptionList, StackBase,
// StackLimit, and DeallocationStack. ASan bounds are the reserve
// [DeallocationStack, StackBase), not StackLimit (that committed edge moves as the
// stack grows). VirtualQuery must agree, and the first entry on that fiber must see
// the same range in the TEB. A mismatch aborts; false bounds are not passed to ASan.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define FUSE_FIBER_ASAN 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define FUSE_FIBER_ASAN 1
#endif

#if !defined(FUSE_FIBER_ASAN)
#define FUSE_FIBER_ASAN 0
#endif

#if FUSE_FIBER_ASAN
#if defined(__clang__) || defined(__GNUC__)
#define FUSE_FIBER_NO_SANITIZE __attribute__((no_sanitize("address")))
#elif defined(_MSC_VER)
#define FUSE_FIBER_NO_SANITIZE __declspec(no_sanitize_address)
#else
#define FUSE_FIBER_NO_SANITIZE
#endif

extern "C" {
void __cdecl __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __cdecl __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old,
                                             size_t* size_old);

#if defined(_MSC_VER)
void __cdecl fuse_asan_start_switch_fiber_unavailable(void**, const void*, size_t) {}
void __cdecl fuse_asan_finish_switch_fiber_unavailable(void*, const void**, size_t*) {}
#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:___sanitizer_start_switch_fiber=_fuse_asan_start_switch_fiber_unavailable")
#pragma comment(linker, "/alternatename:___sanitizer_finish_switch_fiber=_fuse_asan_finish_switch_fiber_unavailable")
#else
#pragma comment(linker, "/alternatename:__sanitizer_start_switch_fiber=fuse_asan_start_switch_fiber_unavailable")
#pragma comment(linker, "/alternatename:__sanitizer_finish_switch_fiber=fuse_asan_finish_switch_fiber_unavailable")
#endif
#endif
}
#else
#define FUSE_FIBER_NO_SANITIZE
#endif

namespace fuse::platform {

struct FiberContext {
    void* fiberHandle = nullptr;
    void (*entry)(void*) = nullptr;
    void* userData = nullptr;
    bool captured = false;
#if FUSE_FIBER_ASAN
    void* asanFakeStack = nullptr;
    const void* asanStackBottom = nullptr;
    size_t asanStackSize = 0;
#endif
};

namespace {

#if FUSE_FIBER_ASAN

struct FiberPrefix {
    void* fiberData;
    void* exceptionList;
    void* stackBase;
    void* stackLimit;
    void* deallocationStack;
};

bool asanHooksLinked() {
#if defined(_MSC_VER)
    return reinterpret_cast<void*>(&__sanitizer_start_switch_fiber) !=
               reinterpret_cast<void*>(&fuse_asan_start_switch_fiber_unavailable) &&
           reinterpret_cast<void*>(&__sanitizer_finish_switch_fiber) !=
               reinterpret_cast<void*>(&fuse_asan_finish_switch_fiber_unavailable);
#else
    return true;
#endif
}

// Stack the thread is executing on right now (thread stack or the switched-in fiber).
bool currentThreadStackBounds(const void** bottom, size_t* size) {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    if (tib == nullptr || tib->StackBase == nullptr || tib->StackLimit == nullptr) {
        return false;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(tib->StackLimit, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }

    auto* low = static_cast<unsigned char*>(info.AllocationBase);
    auto* high = static_cast<unsigned char*>(tib->StackBase);
    if (low == nullptr || low >= high) {
        return false;
    }

    *bottom = low;
    *size = static_cast<size_t>(high - low);
    return true;
}

bool fiberObjectStackBounds(void* fiberHandle, const void** bottom, size_t* size) {
    if (fiberHandle == nullptr) {
        return false;
    }

    FiberPrefix prefix{};
    std::memcpy(&prefix, fiberHandle, sizeof(prefix));
    auto* low = static_cast<unsigned char*>(prefix.deallocationStack);
    auto* high = static_cast<unsigned char*>(prefix.stackBase);
    if (low == nullptr || high == nullptr || low >= high) {
        return false;
    }

    MEMORY_BASIC_INFORMATION lowInfo{};
    MEMORY_BASIC_INFORMATION highInfo{};
    if (VirtualQuery(low, &lowInfo, sizeof(lowInfo)) != sizeof(lowInfo)) {
        return false;
    }
    if (VirtualQuery(high - 1, &highInfo, sizeof(highInfo)) != sizeof(highInfo)) {
        return false;
    }
    if (lowInfo.AllocationBase != low || highInfo.AllocationBase != low) {
        return false;
    }

    *bottom = low;
    *size = static_cast<size_t>(high - low);
    return true;
}

bool fiberStackMatchesLive(const FiberContext* ctx) {
    const void* liveBottom = nullptr;
    size_t liveSize = 0;
    if (ctx == nullptr || ctx->asanStackBottom == nullptr || ctx->asanStackSize == 0) {
        return false;
    }
    if (!currentThreadStackBounds(&liveBottom, &liveSize)) {
        return false;
    }
    return liveBottom == ctx->asanStackBottom && liveSize == ctx->asanStackSize;
}

[[noreturn]] FUSE_FIBER_NO_SANITIZE void failASanBounds(const char* message) {
    std::fprintf(stderr, "fuse: %s\n", message);
    std::abort();
}

#endif

FUSE_FIBER_NO_SANITIZE void WINAPI fiberEntryStub(void* param) {
    auto* self = static_cast<FiberContext*>(param);
    if (!self) {
        std::abort();
    }
#if FUSE_FIBER_ASAN
    if (asanHooksLinked()) {
        // First entry on this stack: complete the switch started in fiberSwap().
        __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
        if (!fiberStackMatchesLive(self)) {
            failASanBounds("Win32 fiber stack bounds do not match the TEB; ASan annotations would be incorrect");
        }
    }
#endif
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

bool fiberASanAnnotationsActive() {
#if FUSE_FIBER_ASAN
    return asanHooksLinked();
#else
    return false;
#endif
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

#if FUSE_FIBER_ASAN
    if (asanHooksLinked()) {
        const void* bottom = nullptr;
        size_t size = 0;
        if (!fiberObjectStackBounds(fiber->fiberHandle, &bottom, &size)) {
            DeleteFiber(fiber->fiberHandle);
            delete fiber;
            failASanBounds("CreateFiber stack bounds are not reliable for ASan");
        }
        fiber->asanStackBottom = bottom;
        fiber->asanStackSize = size;
    }
#endif

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
#if FUSE_FIBER_ASAN
    if (asanHooksLinked()) {
        const void* liveBottom = nullptr;
        size_t liveSize = 0;
        const void* objectBottom = nullptr;
        size_t objectSize = 0;
        if (!currentThreadStackBounds(&liveBottom, &liveSize) ||
            !fiberObjectStackBounds(ctx->fiberHandle, &objectBottom, &objectSize) ||
            liveBottom != objectBottom || liveSize != objectSize) {
            failASanBounds("thread fiber stack bounds are not reliable for ASan");
        }
        ctx->asanStackBottom = liveBottom;
        ctx->asanStackSize = liveSize;
    }
#endif
}

FUSE_FIBER_NO_SANITIZE void fiberSwap(FiberContext* from, FiberContext* to) {
    if (!from || !to || !to->fiberHandle) {
        std::abort();
    }
#if FUSE_FIBER_ASAN
    const bool annotate = asanHooksLinked();
    if (annotate) {
        if (to->asanStackBottom == nullptr || to->asanStackSize == 0) {
            failASanBounds("Win32 fiber is missing ASan stack bounds");
        }
        __sanitizer_start_switch_fiber(&from->asanFakeStack, to->asanStackBottom, to->asanStackSize);
    }
#endif
    SwitchToFiber(to->fiberHandle);
#if FUSE_FIBER_ASAN
    if (annotate) {
        __sanitizer_finish_switch_fiber(from->asanFakeStack, nullptr, nullptr);
    }
#endif
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
