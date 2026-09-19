#pragma once

#include <fuse/types.hpp>

namespace fuse::legacy::t3d {

/// Quarantined Torque3D dimension — prefixed symbols, game-thread init only (U2).
bool initialize();
void shutdown();
bool isInitialized();

namespace Con {
void init();
void execute(const char* script);
void executef(const char* fmt, ...);
void printf(const char* fmt, ...);
void errorf(const char* fmt, ...);
void warnf(const char* fmt, ...);
const char* getVariable(const char* name);
void setVariable(const char* name, const char* value);
} // namespace Con

u32 stringTableEntryCount();

} // namespace fuse::legacy::t3d
