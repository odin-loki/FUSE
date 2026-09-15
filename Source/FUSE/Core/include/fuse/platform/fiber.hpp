#pragma once

#include <fuse/types.hpp>

namespace fuse::platform {

/// Low-level cooperative fiber context (POSIX ucontext on native Linux/macOS).
struct FiberContext;

bool cooperativeFibersAvailable();

FiberContext* fiberAllocateContext();
void fiberCaptureCurrent(FiberContext* ctx);
FiberContext* fiberCreate(u32 stackBytes, void (*entry)(void* userData), void* userData);
void fiberSwap(FiberContext* from, FiberContext* to);
void fiberDestroy(FiberContext* ctx);

} // namespace fuse::platform
