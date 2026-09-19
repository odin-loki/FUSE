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
int getIntVariable(const char* name, int def = 0);
void setIntVariable(const char* name, int value);
bool getBoolVariable(const char* name, bool def = false);
void setBoolVariable(const char* name, bool value);
void addPathExpando(const char* expandoName, const char* path);
bool expandPath(char* dst, u32 size, const char* src, const char* workingDirHint = nullptr,
                bool ensureTrailingSlash = false);
void collapsePath(char* dst, u32 size, const char* src, const char* workingDirHint = nullptr);
} // namespace Con

u32 stringTableEntryCount();

} // namespace fuse::legacy::t3d
