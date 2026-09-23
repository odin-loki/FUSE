#pragma once

#include <fuse/types.hpp>

#include <memory>

namespace fuse::platform {

/// Low-level cooperative fiber context.
/// Backends: POSIX ucontext (desktop Linux/macOS), Win32 fibers (Windows), stub elsewhere.
struct FiberContext;

bool cooperativeFibersAvailable();

/// Diagnostic label for the active backend ("posix-ucontext", "win32", or "stub").
const char* fiberBackendName();

FiberContext* fiberAllocateContext();
void fiberCaptureCurrent(FiberContext* ctx);
FiberContext* fiberCreate(u32 stackBytes, void (*entry)(void* userData), void* userData);
void fiberSwap(FiberContext* from, FiberContext* to);
void fiberDestroy(FiberContext* ctx);

/// Owning handle for a fiber context (fiberDestroy on release). Prefer this over raw pointers.
struct FiberDeleter {
    void operator()(FiberContext* ctx) const { fiberDestroy(ctx); }
};
using UniqueFiber = std::unique_ptr<FiberContext, FiberDeleter>;

} // namespace fuse::platform
