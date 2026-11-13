#!/usr/bin/env bash
# FreeBSD cross-build from Linux (no VM): fetch a FreeBSD base sysroot, build Source/FUSE/UI with host clang/lld.
# Tests are only built, not run (run them with run-freebsd-vm.sh). Env: FREEBSD_VERSION (14.4), FREEBSD_SYSROOT, BUILD_DIR.
source "$(dirname "$0")/lib.sh"
V=${FREEBSD_VERSION:-14.4}
SYSROOT=${FREEBSD_SYSROOT:-$ROOT/build-gui-freebsd-vm/sysroot-$V}
BUILD_DIR=${BUILD_DIR:-$ROOT/build-gui-freebsd-cross}
CXX=${CXX:-clang++}; CC=${CC:-clang}
command -v "$CXX" >/dev/null || { skip "freebsd-cross" "clang++ not found"; summary freebsd-cross; }
[ -d "$SYSROOT/usr/lib" ] || step "sysroot $V" bash -c "mkdir -p '$SYSROOT' && curl -sfL https://download.freebsd.org/releases/amd64/$V-RELEASE/base.txz | tar -xJf - -C '$SYSROOT' ./usr/include ./usr/lib ./lib 2>/dev/null; [ -d '$SYSROOT/usr/lib' ]"
step "cross configure" env FREEBSD_SYSROOT="$SYSROOT" cmake -S "$UI" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/docs/gui/ci/freebsd-cross.cmake" -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_COMPILER_TARGET=x86_64-unknown-freebsd$V -DCMAKE_CXX_COMPILER_TARGET=x86_64-unknown-freebsd$V
step "cross build" build
step "ELF is FreeBSD" bash -c "f=\$(find '$BUILD_DIR' -maxdepth 3 -type f -perm -u+x -name 'fuse_*' | head -1); [ -n \"\$f\" ] && readelf -h \"\$f\" | grep -q FreeBSD"
summary freebsd-cross
