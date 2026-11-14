#!/usr/bin/env bash
# FreeBSD GUI build + tests, run ON FreeBSD (CI: vmactions/freebsd-vm). Installs deps with pkg when root/sudo.
# For a cross-build from Linux instead, see docs/gui/ci/freebsd-cross.cmake; to boot a local VM see run-freebsd-vm.sh.
source "$(dirname "$0")/lib.sh"
BUILD_DIR=${BUILD_DIR:-$ROOT/build-gui-freebsd}
if want_deps && command -v pkg >/dev/null; then
    step "deps (pkg)" as_root env ASSUME_ALWAYS_YES=yes pkg install -y cmake-core ninja bash
fi
CC=${CC:-cc} CXX=${CXX:-c++}   # base system clang (19 on 14.4)
step configure configure -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
step build build
step "test headless" env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir "$BUILD_DIR" --output-on-failure -j "${CTEST_JOBS:-2}"
summary freebsd
