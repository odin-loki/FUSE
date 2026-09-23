#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::io {

/// Last-write time of a file in nanoseconds (0 when the path does not exist).
///
/// Only differences between two calls are meaningful (hot reload / cook-cache staleness); the
/// epoch is platform specific. POSIX: std::filesystem::last_write_time (st_mtim, ns). Windows:
/// GetFileAttributesExW (FILETIME, 100 ns ticks since 1601) — MinGW's libstdc++ implements
/// last_write_time with _wstat64, which truncates to whole seconds, so two writes within the same
/// second would look unchanged. `path` is UTF-8.
u64 fileWriteTimeNs(const std::string& path);

} // namespace fuse::io
