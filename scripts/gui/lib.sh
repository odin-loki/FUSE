# Shared helpers for scripts/gui/test-*.sh: step runner + pass/fail summary.
# Env: FUSE_NO_DEPS=1 skips dependency installation; BUILD_DIR overrides the build directory; JOBS sets parallelism.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
UI="$ROOT/Source/FUSE/UI"
JOBS=${JOBS:-$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)}
RESULTS=(); FAILED=0
step() { # step <name> <cmd...>
    local name=$1; shift
    printf '\n==> %s\n' "$name"
    if "$@"; then RESULTS+=("PASS  $name"); else RESULTS+=("FAIL  $name"); FAILED=1; fi
}
skip() { RESULTS+=("SKIP  $1 ($2)"); }
summary() { # summary <platform>
    printf '\n==== %s summary ====\n' "$1"; printf '  %s\n' "${RESULTS[@]}"
    if [ $FAILED = 0 ]; then echo "RESULT: $1 PASS"; else echo "RESULT: $1 FAIL"; fi
    exit $FAILED
}
want_deps() { [ "${FUSE_NO_DEPS:-0}" != 1 ]; }
as_root() { if [ "$(id -u)" = 0 ]; then "$@"; elif command -v sudo >/dev/null; then sudo -n "$@"; else "$@"; fi; }
configure() { cmake -S "$UI" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo} "$@"; }
build() { cmake --build "$BUILD_DIR" -j "$JOBS" "$@"; }
ctest_run() { ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error -j "${CTEST_JOBS:-4}" "$@"; }
