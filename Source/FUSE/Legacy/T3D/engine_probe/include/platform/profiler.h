#ifndef _PROFILER_H_
#define _PROFILER_H_

#ifndef _TORQUECONFIG_H_
#include "torqueConfig.h"
#endif

/// FUSE_T3D_LEGACY_ENGINE_PROBE shadow — no-op profiler macros (bitmapSTB uses PROFILE_START_IF).
#define PROFILE_START(name)
#define PROFILE_END()
#define PROFILE_END_NAMED(name)
#define PROFILE_START_IF(act, val, fmt) if (false) { (void)0; }
#define PROFILE_SCOPE(name)

#endif // _PROFILER_H_
