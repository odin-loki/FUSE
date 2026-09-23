#!/usr/bin/env bash
# CMAKE_CROSSCOMPILING_EMULATOR for the MinGW-w64 toolchain: runs one Windows test binary under
# Wine, headless. Usage: fuse-wine-run.sh <wineprefix> <program.exe> [args...]
#
# - WINEDEBUG=-all keeps Wine's own diagnostics out of test output.
# - The prefix lives in the build tree; it is created once (wineboot, serialised with flock so
#   parallel ctest jobs do not race on it) and reused by every test.
# - No display is needed: the prefix is configured for Wine's null graphics driver, which still
#   supports HWNDs and message queues (set FUSE_WINE_KEEP_DISPLAY=1 before the prefix is first
#   created to use the host X display instead).
# - Exit codes are passed through unchanged (ctest SKIP_RETURN_CODE 77 keeps working).
set -euo pipefail
prefix="$1"
shift

wine_bin="${FUSE_WINE:-}"
if [ -z "$wine_bin" ]; then
    for candidate in wine64 /usr/lib/wine/wine64 /usr/lib/x86_64-linux-gnu/wine/wine64 wine; do
        if command -v "$candidate" >/dev/null 2>&1; then
            wine_bin="$(command -v "$candidate")"
            break
        fi
    done
fi
if [ -z "$wine_bin" ]; then
    echo "SKIP: wine not installed"
    exit 77
fi

export WINEPREFIX="${WINEPREFIX:-$prefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEARCH=win64
# Wine is an emulated run for timing purposes (fuse::core::timingBudgetsEnforced() — the same
# switch valgrind runs use): wall-clock budgets are reported but not enforced; correctness is.
export FUSE_INSTRUMENTED_RUN="${FUSE_INSTRUMENTED_RUN:-wine}"
# No Mono/Gecko install prompts during wineboot.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
if [ -z "${FUSE_WINE_KEEP_DISPLAY:-}" ]; then
    unset DISPLAY WAYLAND_DISPLAY
fi

if [ ! -f "$WINEPREFIX/system.reg" ]; then
    mkdir -p "$WINEPREFIX"
    (
        flock 9
        if [ ! -f "$WINEPREFIX/system.reg" ]; then
            "$wine_bin" wineboot --init >/dev/null 2>&1 || true
            # Headless: Wine's null graphics driver (no X server needed) still creates HWNDs,
            # message queues and DCs, so window/input/DPI tests run on CI.
            # Unhandled exceptions (the crash-handler tests crash on purpose) must terminate the
            # process with the exception code, as Windows without a JIT debugger does — not
            # attach winedbg --auto (AeDebug) or wait on a crash dialog.
            "$wine_bin" reg delete 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\AeDebug' /v Debugger /f >/dev/null 2>&1 || true
            "$wine_bin" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1 || true
            if [ -z "${FUSE_WINE_KEEP_DISPLAY:-}" ]; then
                "$wine_bin" reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f >/dev/null 2>&1 || true
            fi
            boot_server="$(dirname "$wine_bin")/wineserver"
            [ -x "$boot_server" ] || boot_server=wineserver
            "$boot_server" -w >/dev/null 2>&1 || true
        fi
    ) 9>"$WINEPREFIX.lock"
fi

# Keep one wineserver (and the services Wine starts with it) alive across the whole ctest run:
# without it every test starts and stops its own server, and a test that launches while the
# previous server is shutting down can fail to start (a 0.01 s failure under ctest -j). The
# server and its helper processes are started here with every stdio stream on /dev/null — if they
# inherited ctest's output pipe, ctest would wait for them for the whole persistence window.
wineserver_bin="$(dirname "$wine_bin")/wineserver"
[ -x "$wineserver_bin" ] || wineserver_bin="$(command -v wineserver || true)"
if [ -n "$wineserver_bin" ] && [ -z "${FUSE_WINE_NO_PERSIST:-}" ]; then
    "$wineserver_bin" -p120 </dev/null >/dev/null 2>&1 || true
    "$wine_bin" cmd.exe /c exit </dev/null >/dev/null 2>&1 || true
fi

exec "$wine_bin" "$@"
