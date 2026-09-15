#pragma once

#include <fuse/types.hpp>

namespace fuse::legacy::t3d {

/// Quarantined Torque3D dimension — prefixed symbols, game-thread init only (U2).
bool initialize();
void shutdown();
bool isInitialized();

namespace Con {
void execute(const char* script);
void printf(const char* fmt, ...);
} // namespace Con

u32 stringTableEntryCount();

} // namespace fuse::legacy::t3d
