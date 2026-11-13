#!/bin/sh
# Runs one test on its own X server, session bus and temp/XDG dirs so ctest -jN never shares a display, D-Bus name,
# WebKit cache or temp file between tests. Replaces `xvfb-run -a`, whose display pick (scan lock files, then start)
# races when several tests start at once. Xvfb -displayfd lets the server choose a free display atomically.
# usage: run_isolated_x.sh [-s "Xvfb args"] -- cmd args...
XARGS="-screen 0 1280x800x24"
if [ "$1" = "-s" ]; then XARGS="$2"; shift 2; fi
[ "$1" = "--" ] && shift
ROOT=$(mktemp -d "${TMPDIR:-/tmp}/fuse-test.XXXXXX") || exit 1
mkdir -p "$ROOT/tmp" "$ROOT/run" "$ROOT/cache" "$ROOT/data" "$ROOT/config" && chmod 700 "$ROOT/run"
cleanup() { [ -n "$XPID" ] && kill "$XPID" 2>/dev/null; wait "$XPID" 2>/dev/null; rm -rf "$ROOT"; }
trap cleanup EXIT INT TERM
# shellcheck disable=SC2086
Xvfb -displayfd 3 -nolisten tcp $XARGS 3>"$ROOT/display" >"$ROOT/xvfb.log" 2>&1 &
XPID=$!
i=0
while [ ! -s "$ROOT/display" ]; do
    i=$((i + 1)); [ $i -gt 1000 ] && { echo "run_isolated_x: Xvfb did not start" >&2; cat "$ROOT/xvfb.log" >&2; exit 1; }
    kill -0 "$XPID" 2>/dev/null || { echo "run_isolated_x: Xvfb exited" >&2; cat "$ROOT/xvfb.log" >&2; exit 1; }
    sleep 0.01
done
export DISPLAY=":$(head -n1 "$ROOT/display")"
export TMPDIR="$ROOT/tmp" XDG_RUNTIME_DIR="$ROOT/run" XDG_CACHE_HOME="$ROOT/cache" XDG_DATA_HOME="$ROOT/data" XDG_CONFIG_HOME="$ROOT/config"
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
if command -v dbus-run-session >/dev/null 2>&1; then dbus-run-session -- "$@"; else "$@"; fi
exit $?
