// fuse_app iOS entry: the process main enters UIKit (UIApplicationMain) with FUSE's app delegate, which runs the
// app's `int main(int, char**)` (renamed fuse_app_main by fuse/app/main.hpp) once the application has launched.
// Kept in its own translation unit: an executable that defines its own main does not pull this object out of
// libfuse_app_native.a (and then has no UIApplication, so NativePlatform::available() is false).
int fuse_app_main(int argc, char** argv);

namespace fuse::app::detail {
int iosRunMain(int argc, char** argv, int (*appMain)(int, char**));
}

int main(int argc, char** argv) { return fuse::app::detail::iosRunMain(argc, argv, &fuse_app_main); }
