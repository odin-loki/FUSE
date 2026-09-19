#ifndef FUSE_COLOR_CPP_PRELUDE_H
#define FUSE_COLOR_CPP_PRELUDE_H

// Force-included only for color.cpp / color_probe_smoke.cpp — shadow engineAPI before real color.h parses.
#include "console/engineAPI.h"
#include "console/console.h"
#include "core/strings/stringFunctions.h"
#include "core/stringTable.h"

// StockColors MODULE_BEGIN block in color.cpp — stub module registration (no module.cpp closure).
#define MODULE_BEGIN(name) namespace { namespace _##name {
#define MODULE_INIT_AFTER(name)
#define MODULE_INIT void _fuseColorProbeInit()
#define MODULE_SHUTDOWN void _fuseColorProbeShutdown()
#define MODULE_END }; }

#endif // FUSE_COLOR_CPP_PRELUDE_H
