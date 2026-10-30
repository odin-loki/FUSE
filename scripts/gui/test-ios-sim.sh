#!/usr/bin/env bash
# iOS simulator build of the UIKit backend (fuse_app_native). Build only: no simulator run yet. CI: ios-simulator.
# Env: IOS_ARCH (default arm64; x86_64 on Intel Macs), IOS_MIN (default 15.0).
source "$(dirname "$0")/lib.sh"
BUILD_DIR=${BUILD_DIR:-$ROOT/build-gui-ios-sim}
[ "$(uname)" = Darwin ] || { skip ios-sim "needs macOS + Xcode"; summary ios-sim; }
if want_deps; then command -v ninja >/dev/null || step "deps (ninja)" brew install ninja; fi
step configure configure -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES="${IOS_ARCH:-arm64}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_MIN:-15.0}" -DFUSE_BUILD_UI_TESTS=OFF -DFUSE_BUILD_UI_DESIGNER=OFF \
    -DFUSE_BUILD_GUI_EXAMPLES=OFF -DFUSE_BUILD_UI_VK=OFF
step "build fuse_app_native" build --target fuse_app_native
summary ios-sim
