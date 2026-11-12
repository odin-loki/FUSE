// fuse_app entry point (plan UI5): one `int FUSE_APP_MAIN(int argc, char** argv)` on every platform.
//
// Windows, Linux, macOS and FreeBSD start in main() as usual, so there FUSE_APP_MAIN is `main`. Android
// (NativeActivity -> android_main, on its own thread) and iOS (UIApplicationMain, which never returns) own the real
// entry point, so there FUSE_APP_MAIN is `fuse_app_main` and fuse_app_native's entry calls it once the platform is
// up:
//
//   #include "fuse/app/main.hpp"
//   int FUSE_APP_MAIN(int argc, char** argv) { // must have exactly this signature
//       fuse::app::Application app{{.name = "hello", .argc = argc, .argv = argv}};
//       fuse::app::NativePlatform native(app);
//       native.show(app.createWindow({.title = "Hello"}));
//       return app.run();
//   }
//
// Include it in the translation unit that defines the app's entry function.
#pragma once

#if defined(__ANDROID__)
#define FUSE_APP_RENAMED_MAIN 1
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define FUSE_APP_RENAMED_MAIN 1
#endif
#endif

#if defined(FUSE_APP_RENAMED_MAIN)
int fuse_app_main(int argc, char** argv);
#define FUSE_APP_MAIN fuse_app_main
#else
#define FUSE_APP_MAIN main
#endif
