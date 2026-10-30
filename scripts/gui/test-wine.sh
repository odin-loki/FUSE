#!/usr/bin/env bash
# Windows build cross-compiled with llvm-mingw, tests run under Wine (+Xvfb). CI: windows-mingw-wine.
# Env: MINGW (llvm-mingw root; downloaded to ~/.cache/fuse-gui when unset), WINE (default wine), WINEPREFIX.
source "$(dirname "$0")/lib.sh"
BUILD_DIR=${BUILD_DIR:-$ROOT/build-gui-wine}; WINE=${WINE:-wine}
export WINEPREFIX=${WINEPREFIX:-$HOME/.cache/fuse-gui/wineprefix} WINEDEBUG=${WINEDEBUG:--all}
V=20261020; if [ -z "${MINGW:-}" ]; then
    MINGW=$HOME/.cache/fuse-gui/llvm-mingw-$V-ucrt-ubuntu-22.04-x86_64
    [ -d "$MINGW" ] || { want_deps && step "deps (llvm-mingw)" bash -c "mkdir -p ~/.cache/fuse-gui && curl -sfL https://github.com/mstorsjo/llvm-mingw/releases/download/$V/llvm-mingw-$V-ucrt-ubuntu-22.04-x86_64.tar.xz | tar -xJ -C ~/.cache/fuse-gui"; }
fi
command -v "$WINE" >/dev/null || { skip wine "wine not found"; FAILED=1; summary windows-wine; }
RUN=("$WINE"); command -v xvfb-run >/dev/null && RUN=(xvfb-run -a -s "-screen 0 1280x800x24" "$WINE")
EMU=$(IFS=";"; echo "${RUN[*]}")
# Initialise the prefix first (the build runs cooking tools under Wine, and fuse_ui_os_seam reads PATH, which only a
# completed wineboot writes into the registry Environment key).
# Headless wineboot sometimes stops before writing the Environment key, so set PATH explicitly if it is still missing.
haspath() { grep -q '^"PATH"=' "$WINEPREFIX/system.reg" 2>/dev/null; }
haspath || step "wineboot" timeout 600 "${RUN[@]}" wineboot -i
haspath || step "prefix PATH" timeout 120 "${RUN[@]}" reg add 'HKLM\System\CurrentControlSet\Control\Session Manager\Environment' \
    /v PATH /t REG_EXPAND_SZ /d 'C:\windows\system32;C:\windows;C:\windows\system32\wbem' /f
step configure configure -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER="$MINGW/bin/x86_64-w64-mingw32-clang" \
    -DCMAKE_CXX_COMPILER="$MINGW/bin/x86_64-w64-mingw32-clang++" -DCMAKE_EXE_LINKER_FLAGS=-static "-DCMAKE_CROSSCOMPILING_EMULATOR=$EMU"
step build build
step "test (wine)" ctest_run
summary windows-wine
