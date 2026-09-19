#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#ifndef _PLATFORM_H_
#include "platform/platform.h"
#endif

/// FUSE_T3D_LEGACY_ENGINE_PROBE shadow — real console pulls SimObject / TorqueScript.
namespace Con {
void printf(const char* fmt, ...);
void errorf(const char* fmt, ...);
} // namespace Con

#endif // _CONSOLE_H_
