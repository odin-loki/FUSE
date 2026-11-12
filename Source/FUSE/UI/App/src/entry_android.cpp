// fuse_app Android entry: android_native_app_glue calls android_main on the app thread; it runs the app's
// `int main(int, char**)` (renamed fuse_app_main by fuse/app/main.hpp). Kept in its own translation unit so an app
// that defines its own android_main simply does not pull this object out of libfuse_app_native.a.
#include <android_native_app_glue.h>

int fuse_app_main(int argc, char** argv);

namespace fuse::app::detail {
void androidRunMain(android_app* app, int (*appMain)(int, char**));
}

extern "C" void android_main(android_app* app) { fuse::app::detail::androidRunMain(app, &fuse_app_main); }
