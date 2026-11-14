// SystemVideo backend stub for platforms without one (the Apple backend lives in media_avf.mm).
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
#include "media_internal.hpp"
namespace fuse::ui::media::detail {
bool backendAvailable() { return false; }
std::string backendName() { return {}; }
std::unique_ptr<Decoder> openFile(const std::string&, StreamKind, std::string* e) { if (e) *e = "no system decoder on this platform"; return nullptr; }
std::unique_ptr<Decoder> openPipeline(const std::string&, StreamKind, std::string* e) { if (e) *e = "pipelines need GStreamer"; return nullptr; }
} // namespace fuse::ui::media::detail
#endif
