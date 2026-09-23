#include <fuse/io/file_time.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>
#else
#include <chrono>
#include <filesystem>
#include <system_error>
#endif

namespace fuse::io {

u64 fileWriteTimeNs(const std::string& path) {
    if (path.empty()) {
        return 0;
    }
#if defined(_WIN32)
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        return 0;
    }
    std::vector<wchar_t> wide(static_cast<usize>(wideLen));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wideLen);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(wide.data(), GetFileExInfoStandard, &data) == FALSE) {
        return 0;
    }
    const u64 ticks = (static_cast<u64>(data.ftLastWriteTime.dwHighDateTime) << 32u) |
                      data.ftLastWriteTime.dwLowDateTime;
    return ticks * 100ull;
#else
    std::error_code ec;
    const std::filesystem::path p(path);
    const auto written = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return 0;
    }
    return static_cast<u64>(std::chrono::time_point_cast<std::chrono::nanoseconds>(written).time_since_epoch().count());
#endif
}

} // namespace fuse::io
