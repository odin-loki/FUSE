#!/usr/bin/env bash
# FUSE Relight runner (RL-0.3): runs one Windows program under Wine with a real X display (Xvfb)
# and Mesa's Lavapipe as the Vulkan driver, so winevulkan / DXVK / wined3d work headless.
# Modelled on fuse-wine-run.sh, which uses Wine's null graphics driver; that driver cannot load
# winevulkan ("Failed to load Wine graphics driver supporting Vulkan", plan §6.1), hence this one.
#
# Usage: fuse-wine-xvfb-run.sh [options] <prefix-root> <program.exe> [args...]
#   Options (before <prefix-root>; each also has an environment form):
#     --native-d3d        load native (build-tree / app-dir) d3d9, d3d8 and dxgi before Wine's
#                         builtins: adds "d3d9,d3d8,dxgi=n,b" to WINEDLLOVERRIDES
#     --dll-override S    append S (WINEDLLOVERRIDES syntax) to the overrides; repeatable
#                         (env: FUSE_WINE_DLL_OVERRIDES)
#     --fresh-prefix      recopy this slot's prefix from the cached template before the run, for
#                         tests that change the registry (env: FUSE_WINE_FRESH_PREFIX=1)
#   Other environment knobs:
#     FUSE_WINE               Wine loader to use (default: wine64 for PE32+, wine for PE32)
#     FUSE_WINE_SLOTS         number of Xvfb/prefix slots (default: nproc, max 32)
#     FUSE_WINE_USE_DISPLAY=1 use the caller's $DISPLAY instead of starting Xvfb
#     FUSE_VK_ICD_FILES       ':'-separated ICD JSONs (default: every lvp_icd*.json found)
#     FUSE_VK_LAYER_DIR       directory with the *Windows* VkLayer_khronos_validation.{dll,json};
#                             staged into the prefix and registered under
#                             HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers (used by a native
#                             Khronos vulkan-1.dll; Wine's builtin vulkan-1 does not load layers)
#
# Behaviour:
# - Slots: <prefix-root>/slot-<n> is a WINEPREFIX that one run holds at a time (flock), so
#   `ctest -j N` gives every job its own prefix, wineserver and Xvfb; nothing is shared or raced.
#   Slot prefixes are copies of <prefix-root>/template, booted once (wineboot under flock) and
#   rebuilt when the Wine version or wine32 availability changes. Slots are hard-linked copies
#   (registry hives copied), so N slots cost little disk; reuse keeps a run to about 3 s.
# - Display: a private Xvfb per run (-displayfd picks a free display number), killed on exit,
#   together with the slot's wineserver (so no process outlives the test and holds ctest's pipes).
# - Vulkan: VK_ICD_FILENAMES and VK_DRIVER_FILES point at Lavapipe (the host loader that
#   winevulkan uses reads them); LIBGL_ALWAYS_SOFTWARE=1 keeps wined3d (the reference D3D9
#   renderer) on llvmpipe too.
# - FUSE_INSTRUMENTED_RUN=wine (timing budgets are reported, not enforced), WINEDEBUG=-all.
# - Exit codes: the program's exit code is passed through unchanged. 77 (ctest SKIP_RETURN_CODE)
#   when Wine or Xvfb is missing, or when the program is a 32-bit PE and wine32 is not installed.
set -uo pipefail

self="fuse-wine-xvfb-run"
native_d3d=0
fresh="${FUSE_WINE_FRESH_PREFIX:-0}"
extra_overrides="${FUSE_WINE_DLL_OVERRIDES:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --native-d3d) native_d3d=1; shift ;;
        --dll-override) extra_overrides="${extra_overrides:+$extra_overrides;}$2"; shift 2 ;;
        --fresh-prefix) fresh=1; shift ;;
        --) shift; break ;;
        -*) echo "$self: unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
if [ $# -lt 2 ]; then
    echo "usage: $0 [--native-d3d] [--dll-override S] [--fresh-prefix] <prefix-root> <program.exe> [args...]" >&2
    exit 2
fi
root="$1"
shift
program="$1"

# ---- what is the program? (PE machine: 0x14c i386, 0x8664 x86-64) ----------------------------
pe_machine() {
    local off
    off="$(od -An -tu4 -j60 -N4 "$1" 2>/dev/null | tr -d ' ')" || return 1
    [ -n "$off" ] || return 1
    od -An -tx2 -j$((off + 4)) -N2 "$1" 2>/dev/null | tr -d ' '
}
machine=""
[ -f "$program" ] && machine="$(pe_machine "$program" || true)"

have_wine32() {
    local d
    for d in /usr/lib/i386-linux-gnu/wine/i386-windows /usr/lib/x86_64-linux-gnu/wine/i386-windows \
             /usr/lib/wine/i386-windows /usr/lib32/wine/i386-windows /opt/wine*/lib/wine/i386-windows \
             /usr/local/lib/wine/i386-windows; do
        [ -f "$d/ntdll.dll" ] && [ -f "$d/kernel32.dll" ] && return 0
    done
    return 1
}
wine32=0
have_wine32 && wine32=1

# ---- tools --------------------------------------------------------------------------------------
wine_bin="${FUSE_WINE:-}"
if [ -z "$wine_bin" ]; then
    candidates=(wine64 /usr/lib/wine/wine64 /usr/lib/x86_64-linux-gnu/wine/wine64 wine)
    # A PE32 program needs the 32-bit loader (Debian/Ubuntu: /usr/lib/wine/wine from wine32).
    [ "$machine" = "014c" ] && candidates=(/usr/lib/wine/wine wine "${candidates[@]}")
    for candidate in "${candidates[@]}"; do
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
if [ "$machine" = "014c" ] && [ "$wine32" = 0 ]; then
    echo "SKIP: $program is a 32-bit PE and wine32 is not installed (dpkg --add-architecture i386; apt-get install wine32:i386)"
    exit 77
fi
wineserver_bin="$(dirname "$wine_bin")/wineserver"
[ -x "$wineserver_bin" ] || wineserver_bin="$(command -v wineserver || true)"
if [ -z "${FUSE_WINE_USE_DISPLAY:-}" ] && ! command -v Xvfb >/dev/null 2>&1; then
    echo "SKIP: Xvfb not installed"
    exit 77
fi

# ---- environment ---------------------------------------------------------------------------------
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEARCH=win64
export FUSE_INSTRUMENTED_RUN="${FUSE_INSTRUMENTED_RUN:-wine}"
overrides="mscoree,mshtml=;winemenubuilder.exe=d"
[ "$native_d3d" = 1 ] && overrides="$overrides;d3d9,d3d8,dxgi=n,b"
[ -n "$extra_overrides" ] && overrides="$overrides;$extra_overrides"
[ -n "${WINEDLLOVERRIDES:-}" ] && overrides="$overrides;$WINEDLLOVERRIDES"
export WINEDLLOVERRIDES="$overrides"
unset WAYLAND_DISPLAY

icd="${FUSE_VK_ICD_FILES:-}"
if [ -z "$icd" ]; then
    for f in /usr/share/vulkan/icd.d/lvp_icd*.json /usr/local/share/vulkan/icd.d/lvp_icd*.json \
             /etc/vulkan/icd.d/lvp_icd*.json; do
        [ -f "$f" ] && icd="${icd:+$icd:}$f"
    done
fi
if [ -n "$icd" ]; then
    export VK_ICD_FILENAMES="$icd" VK_DRIVER_FILES="$icd"
else
    echo "$self: warning: no Lavapipe ICD (lvp_icd*.json) found; install mesa-vulkan-drivers" >&2
fi
export LIBGL_ALWAYS_SOFTWARE=1

wine_version="$("$wine_bin" --version 2>/dev/null || echo unknown)"
stamp_text="$wine_version wine32=$wine32"

mkdir -p "$root"
root="$(cd "$root" && pwd)"

# ---- slot (prefix + display) ---------------------------------------------------------------------
slots="${FUSE_WINE_SLOTS:-$(nproc 2>/dev/null || echo 4)}"
[ "$slots" -ge 1 ] 2>/dev/null || slots=4
[ "$slots" -le 32 ] || slots=32
slot=""
for ((i = 0; i < slots; i++)); do
    exec 8>"$root/slot-$i.lock"
    if flock -n 8; then
        slot=$i
        break
    fi
    exec 8>&-
done
if [ -z "$slot" ]; then
    slot=$(($$ % slots))
    exec 8>"$root/slot-$slot.lock"
    flock 8
fi
export WINEPREFIX="$root/slot-$slot"

xvfb_pid=""
cleanup() {
    if [ -n "$wineserver_bin" ]; then
        "$wineserver_bin" -k </dev/null >/dev/null 2>&1 || true
        "$wineserver_bin" -w </dev/null >/dev/null 2>&1 || true
    fi
    if [ -n "$xvfb_pid" ]; then
        kill "$xvfb_pid" 2>/dev/null || true
        wait "$xvfb_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [ -z "${FUSE_WINE_USE_DISPLAY:-}" ]; then
    dfile="$root/slot-$slot.display"
    : >"$dfile"
    Xvfb -displayfd 9 -screen 0 1280x1024x24 -nolisten tcp -nolisten unix -noreset +extension GLX 8>&- \
        9>"$dfile" </dev/null >/dev/null 2>&1 &
    xvfb_pid=$!
    for ((t = 0; t < 200; t++)); do
        num="$(head -n1 "$dfile" 2>/dev/null | tr -dc 0-9)"
        [ -n "$num" ] && break
        kill -0 "$xvfb_pid" 2>/dev/null || break
        sleep 0.05
    done
    if [ -z "${num:-}" ]; then
        # -nolisten unix is rejected by some Xvfb builds (then only the abstract socket is off).
        kill "$xvfb_pid" 2>/dev/null || true
        Xvfb -displayfd 9 -screen 0 1280x1024x24 -nolisten tcp -noreset +extension GLX 8>&- \
            9>"$dfile" </dev/null >/dev/null 2>&1 &
        xvfb_pid=$!
        for ((t = 0; t < 200; t++)); do
            num="$(head -n1 "$dfile" 2>/dev/null | tr -dc 0-9)"
            [ -n "$num" ] && break
            kill -0 "$xvfb_pid" 2>/dev/null || break
            sleep 0.05
        done
    fi
    if [ -z "${num:-}" ]; then
        echo "$self: Xvfb failed to start" >&2
        exit 1
    fi
    export DISPLAY=":$num"
elif [ -z "${DISPLAY:-}" ]; then
    echo "SKIP: FUSE_WINE_USE_DISPLAY=1 but DISPLAY is not set"
    exit 77
fi

# ---- template prefix (booted once; cached across runs and build trees' ctest invocations) --------
template="$root/template"
if [ "$(cat "$template/.fuse-ready" 2>/dev/null)" != "$stamp_text" ]; then
    (
        flock 7
        if [ "$(cat "$template/.fuse-ready" 2>/dev/null)" != "$stamp_text" ]; then
            rm -rf "$template"
            mkdir -p "$template"
            WINEPREFIX="$template" "$wine_bin" wineboot --init </dev/null >/dev/null 2>&1 || true
            # Crashes terminate with the exception code (no winedbg / crash dialog), as in
            # fuse-wine-run.sh.
            WINEPREFIX="$template" "$wine_bin" reg delete 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\AeDebug' /v Debugger /f </dev/null >/dev/null 2>&1 || true
            WINEPREFIX="$template" "$wine_bin" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f </dev/null >/dev/null 2>&1 || true
            [ -n "$wineserver_bin" ] && WINEPREFIX="$template" "$wineserver_bin" -w </dev/null >/dev/null 2>&1
            if [ -f "$template/system.reg" ]; then
                printf '%s' "$stamp_text" >"$template/.fuse-ready"
            else
                echo "$self: wineboot failed to create $template" >&2
            fi
        fi
    ) 7>"$root/template.lock"
fi
if [ ! -f "$template/.fuse-ready" ]; then
    exit 1
fi
if [ "$fresh" = 1 ] || [ "$(cat "$WINEPREFIX/.fuse-ready" 2>/dev/null)" != "$stamp_text" ]; then
    rm -rf "$WINEPREFIX"
    # Hard-linked copy (the template's ~700 MB of system32 is shared, not duplicated); the
    # registry hives are the files Wine rewrites, so they become real copies. Wine replaces rather
    # than rewrites other prefix files, so the template stays intact. Fallback: a plain copy.
    if cp -al "$template" "$WINEPREFIX" 2>/dev/null; then
        for reg in "$WINEPREFIX"/*.reg "$WINEPREFIX/.fuse-ready"; do
            [ -f "$reg" ] && cp --remove-destination "$template/$(basename "$reg")" "$reg"
        done
    else
        rm -rf "$WINEPREFIX"
        cp -a "$template" "$WINEPREFIX"
    fi
fi

# ---- validation layer staging (Windows build of the Khronos layer, optional) ----------------------
if [ -n "${FUSE_VK_LAYER_DIR:-}" ] && [ -f "$FUSE_VK_LAYER_DIR/VkLayer_khronos_validation.json" ]; then
    layer_dst="$WINEPREFIX/drive_c/fuse/vk-layers"
    if [ ! -f "$layer_dst/VkLayer_khronos_validation.json" ] ||
       [ "$FUSE_VK_LAYER_DIR/VkLayer_khronos_validation.dll" -nt "$layer_dst/VkLayer_khronos_validation.dll" ]; then
        mkdir -p "$layer_dst"
        cp -f "$FUSE_VK_LAYER_DIR"/VkLayer_khronos_validation.* "$layer_dst/"
        "$wine_bin" reg add 'HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers' \
            /v 'C:\fuse\vk-layers\VkLayer_khronos_validation.json' /t REG_DWORD /d 0 /f \
            </dev/null >/dev/null 2>&1 || true
    fi
fi

# ---- run ------------------------------------------------------------------------------------------
# Start the slot's wineserver and Wine's system processes with stdio on /dev/null first, so none of
# them inherits ctest's output pipe (see fuse-wine-run.sh).
if [ -n "$wineserver_bin" ]; then
    "$wineserver_bin" -p5 </dev/null >/dev/null 2>&1 8>&- || true
    "$wine_bin" cmd.exe /c exit </dev/null >/dev/null 2>&1 8>&- || true
fi
"$wine_bin" "$@"
rc=$?
exit "$rc"
