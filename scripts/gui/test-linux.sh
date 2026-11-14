#!/usr/bin/env bash
# Linux GUI build + tests in three modes (CI: linux headless/x11/wayland).
# Usage: test-linux.sh [headless] [x11] [wayland]   (default: all three; x11/wayland skip if Xvfb/weston missing)
# Env: CC/CXX (default clang-18 if present), BUILD_DIR (default build-gui-linux).
source "$(dirname "$0")/lib.sh"
MODES=("${@:-headless x11 wayland}"); MODES=(${MODES[@]})
BUILD_DIR=${BUILD_DIR:-$ROOT/build-gui-linux}
if want_deps && command -v apt-get >/dev/null; then
    step "deps (apt)" as_root apt-get install -y -q ninja-build cmake xvfb weston mesa-vulkan-drivers libvulkan-dev >/dev/null
fi
: "${CXX:=$(command -v clang++-18 || command -v clang++ || echo c++)}" "${CC:=$(command -v clang-18 || command -v clang || echo cc)}"
# clang < 19 with libstdc++ hides std::expected (it checks __cpp_concepts >= 202002 while clang 18 reports 201907).
XFLAGS=
printf '#include <expected>\nstd::expected<int,int> e{1};\nint main(){return *e-1;}\n' > /tmp/fuse_exp.cpp
"$CXX" -std=c++23 -c /tmp/fuse_exp.cpp -o /dev/null 2>/dev/null || XFLAGS="-D__cpp_concepts=202002L -Wno-builtin-macro-redefined"
step configure configure -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" "-DCMAKE_CXX_FLAGS=$XFLAGS"
step build build
for m in "${MODES[@]}"; do case $m in
  headless) step "test headless" env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$JOBS" ;;
  x11) if command -v xvfb-run >/dev/null; then step "test x11 (Xvfb)" xvfb-run -a -s "-screen 0 1280x800x24" ctest --test-dir "$BUILD_DIR" --output-on-failure -j 2
       else skip "test x11" "no xvfb-run"; fi ;;
  wayland) if command -v weston >/dev/null; then
       wl() { local d; d=$(mktemp -d); chmod 700 "$d"; XDG_RUNTIME_DIR=$d weston --backend=headless --socket=wl-fuse --idle-time=0 >/dev/null 2>&1 & local p=$!
              for _ in $(seq 50); do [ -S "$d/wl-fuse" ] && break; sleep 0.2; done
              XDG_RUNTIME_DIR=$d WAYLAND_DISPLAY=wl-fuse FUSE_UI_BACKEND=wayland ctest --test-dir "$BUILD_DIR" --output-on-failure -j 2; local rc=$?; kill $p; return $rc; }
       step "test wayland (weston headless)" wl
       else skip "test wayland" "no weston"; fi ;;
esac; done
summary linux
