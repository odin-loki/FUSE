#!/usr/bin/env bash
# ctest rl_dxvk_smoke (RL-0.2): runs create_device.exe through our d3d9.dll under Xvfb + Wine,
# with Lavapipe as the Vulkan driver.
#   usage: run_smoke.sh <fuse-wine-xvfb-run.sh> <prefix-root> <create_device.exe>
#
# It uses RL-0.3's runner, which provides a private Xvfb, a per-slot prefix and Lavapipe ICDs.
# `--native-d3d` loads the d3d9.dll/d3d8.dll next to the exe before Wine's builtins, and the app
# checks that itself. Wine's null display driver cannot back winevulkan (plan §6.1), so the null-driver
# emulator (fuse-wine-run.sh) is not used here.
#
# Exit codes:
#   0 / 1  the app's own (pass / fail);
#   77     ctest skip, in two cases. First, Wine or Xvfb is missing (the runner reports it).
#          Second, the Wine in use is too old for unpatched DXVK 3.1.1. DXVK requires
#          VK_KHR_load_store_op_none, and winevulkan before that extension existed (e.g. Ubuntu's
#          Wine 9.0) filters it out even though Lavapipe has it. The skip needs DXVK's own
#          "Skipping: ... 'khrLoadStoreOpNone'" rejection in the log; any other failure stays a failure.
set -uo pipefail
runner="$1"
prefix_root="$2"
exe="$3"
if [ ! -x "$runner" ]; then
    echo "SKIP: $runner missing"
    exit 77
fi
# info level so a device rejection names its reason; the plain info chatter is filtered below.
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-info}"
export DXVK_LOG_PATH=none
export DXVK_SHADER_CACHE=0
export DXVK_HUD=0
# DXVK skips CPU (software) Vulkan devices unless a device-name filter is set; the gate runs on
# Lavapipe. Override with DXVK_FILTER_DEVICE_NAME to target another device.
export DXVK_FILTER_DEVICE_NAME="${DXVK_FILTER_DEVICE_NAME:-llvmpipe}"

log="$(mktemp)"
trap 'rm -f "$log"' EXIT
"$runner" --native-d3d "$prefix_root" "$exe" >"$log" 2>&1
rc=$?
# Show the app's lines, DXVK warnings/errors and device-selection decisions.
grep -E '^(rl_smoke|SKIP|warn|err)|Found device|Skipping' "$log" || true
if [ "$rc" -ne 0 ] && [ "$rc" -ne 77 ] &&
   grep -q "Skipping: Device does not support required feature 'khrLoadStoreOpNone'" "$log" &&
   grep -q "No adapters found" "$log"; then
    echo "SKIP: this Wine's winevulkan does not expose VK_KHR_load_store_op_none, which DXVK 3.1.1 requires"
    echo "      (the host Vulkan driver may have it; winevulkan filters extensions newer than itself)."
    echo "      Needs a Wine release that knows the extension (Wine 9.0 does not)."
    exit 77
fi
exit "$rc"
