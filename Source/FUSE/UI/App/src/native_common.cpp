// fuse_app native backends: the parts of NativePlatform that are the same on every platform.
#include "fuse/app/native.hpp"

namespace fuse::app {

void NativePlatform::openFileDialog(AppWindow& w, const FileDialogOptions& o, FileDialogCallback cb) {
    std::string path;
    switch (fileDialog(&w, o, path)) {
    case FileDialogStatus::Accepted:
        if (cb.done) cb.done(cb.user, w, path);
        return;
    case FileDialogStatus::Cancelled:
        if (cb.done) cb.done(cb.user, w, {});
        return;
    case FileDialogStatus::Unavailable: break;
    }
    openToolkitFileDialog(w, o, cb); // no native dialog here: the in-toolkit one, answered later from the UI
}

} // namespace fuse::app
