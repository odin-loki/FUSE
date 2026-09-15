#pragma once

#include <fuse/types.hpp>

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

} // namespace fuse::platform
