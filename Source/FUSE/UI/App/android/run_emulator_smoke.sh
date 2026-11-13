#!/usr/bin/env bash
# Emulator smoke test for the android_hello APK (build_apk.sh): install, launch, check the first frame, tap the
# button (clicks=1), type into the line edit through the IME (text=hello) and check the TalkBack tree (uiautomator
# sees the FUSE widgets as Button / EditText virtual views). Needs a booted device on adb. Usage: run_emulator_smoke.sh <apk>
# Env: ADB, SKIP_INSTALL=1, WAIT_SECS per log wait (120; raise for software-emulated devices).
set -euo pipefail
APK=${1:?apk}
ADB=${ADB:-adb}
$ADB shell settings put global hide_error_dialogs 1
# Right after sys.boot_completed a software-emulated device may not have the package manager up yet: wait, then
# retry the install (output kept so a failure says why).
for _ in $(seq 120); do $ADB shell pm path android 2>/dev/null | grep -q package: && break; sleep 5; done
if [ -z "${SKIP_INSTALL:-}" ]; then
    # adb-install verification times out on slow emulators; a debug build signed by another keystore cannot update.
    $ADB shell settings put global verifier_verify_adb_installs 0 || true
    $ADB shell settings put global package_verifier_enable 0 || true
    ok=; for _ in 1 2 3; do
        out=$(timeout 900 $ADB install -r --no-streaming "$APK" 2>&1 || true); echo "$out"
        grep -q '^Success' <<<"$out" && { ok=1; break; }
        grep -q INSTALL_FAILED_UPDATE_INCOMPATIBLE <<<"$out" && $ADB uninstall org.fuse.ui.hello >/dev/null
        sleep 20
    done
    [ -n "$ok" ] || { echo "FAIL: install"; exit 1; }
fi
$ADB shell am force-stop org.fuse.ui.hello
$ADB logcat -c
$ADB shell am start -n org.fuse.ui.hello/org.fuse.ui.FuseActivity >/dev/null
wait_log() { for _ in $(seq $(( ${WAIT_SECS:-120} / 2 ))); do $ADB logcat -d | grep -q "$1" && return 0; sleep 2; done; echo "FAIL: $1"; exit 1; }
wait_log "fuse_ui: first frame"
# FUSE_EXPECT_VULKAN=1: the APK was built with the Vulkan presenter and the device has Vulkan (emulator
# -gpu swiftshader_indirect / host): the swapchain path must have drawn that frame.
if [ -n "${FUSE_EXPECT_VULKAN:-}" ]; then wait_log "fuse_ui: Vulkan present on"; wait_log "first frame .*(vulkan)"; fi
$ADB shell uiautomator dump /sdcard/fuse_ui.xml >/dev/null
TREE=$($ADB shell cat /sdcard/fuse_ui.xml)
grep -q 'text="Click me"[^>]*class="android.widget.Button"' <<<"$TREE" || { echo "FAIL: a11y Button"; exit 1; }
grep -q 'class="android.widget.EditText"' <<<"$TREE" || { echo "FAIL: a11y EditText"; exit 1; }
node_centre() { grep -oE "class=\"$1\"[^>]*bounds=\"\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]\"" <<<"$TREE" | head -1 |
    grep -oE '[0-9]+' | tr '\n' ' ' | awk '{print int(($1+$3)/2), int(($2+$4)/2)}'; }
$ADB shell input tap $(node_centre android.widget.Button)
wait_log "fuse_android: clicks=1"
$ADB shell input tap $(node_centre android.widget.EditText)
sleep 3
$ADB shell input text hello
wait_log "fuse_android: text=hello"
echo "android smoke OK"
