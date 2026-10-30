#!/usr/bin/env bash
# Android: build the sample APK with the NDK, then run the emulator smoke test (frame, tap, IME, a11y tree).
# Env: ANDROID_SDK (or ANDROID_HOME), ANDROID_NDK (or ANDROID_NDK_HOME / ANDROID_NDK_LATEST_HOME), JAVA_HOME (JDK 17),
#      ABIS (default "x86_64 arm64-v8a"), OUT (default build-gui-android), EMULATOR=0 to skip the emulator,
#      ANDROID_ACCEL=off forces software CPU emulation (where nested KVM is unusable), AVD (default fuse34; created when missing), SERIAL (use an already-running device instead of booting one), BOOT_TIMEOUT seconds (1800).
source "$(dirname "$0")/lib.sh"
export ANDROID_SDK=${ANDROID_SDK:-${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}}
export ANDROID_NDK=${ANDROID_NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-}}}
OUT=${OUT:-$ROOT/build-gui-android}; ABIS=(${ABIS:-x86_64 arm64-v8a}); AVD=${AVD:-fuse34}
[ -n "$ANDROID_SDK" ] && [ -n "$ANDROID_NDK" ] && [ -n "${JAVA_HOME:-}" ] || { echo "set ANDROID_SDK, ANDROID_NDK, JAVA_HOME"; skip android "SDK/NDK/JDK not set"; FAILED=1; summary android; }
SDKM=$(ls "$ANDROID_SDK"/cmdline-tools/*/bin/sdkmanager 2>/dev/null | head -1)
if want_deps && [ -n "$SDKM" ]; then
    pk=("build-tools;34.0.0" "platforms;android-34")
    [ "${EMULATOR:-1}" = 1 ] && pk+=(emulator platform-tools "system-images;android-34;default;x86_64")
    step "deps (sdkmanager)" bash -c 'yes | "$0" "$@" >/dev/null' "$SDKM" "${pk[@]}"
fi
step "build APK (${ABIS[*]})" "$UI/App/android/build_apk.sh" "$OUT" "${ABIS[@]}"
APK=$OUT/fuse_hello.apk
if [ "${EMULATOR:-1}" != 1 ]; then skip "emulator smoke" "EMULATOR=0"; summary android; fi
ADB=$ANDROID_SDK/platform-tools/adb; EMU=$ANDROID_SDK/emulator/emulator; EPID=
if [ -z "${SERIAL:-}" ] && ! "$ADB" devices | grep -q 'device$'; then
    if ! "$EMU" -list-avds | grep -qx "$AVD"; then
        AVDM=$(dirname "$SDKM")/avdmanager
        step "create AVD $AVD" bash -c 'echo no | "$0" create avd -n "$1" -k "system-images;android-34;default;x86_64" -f >/dev/null' "$AVDM" "$AVD"
    fi
    accel=(); { [ -w /dev/kvm ] && [ "${ANDROID_ACCEL:-}" != off ]; } || accel=(-accel off)
    "$EMU" -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect "${accel[@]}" >"$OUT/emulator.log" 2>&1 & EPID=$!
    boot() { "$ADB" wait-for-device; for _ in $(seq $(( ${BOOT_TIMEOUT:-1800} / 5 ))); do [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && return 0; sleep 5; done; return 1; }
    step "boot emulator" boot
fi
[ -n "${SERIAL:-}" ] && export ANDROID_SERIAL=$SERIAL
step "emulator smoke" env ADB="$ADB" "$UI/App/android/run_emulator_smoke.sh" "$APK"
[ -n "$EPID" ] && { "$ADB" emu kill >/dev/null 2>&1; wait $EPID 2>/dev/null; }
summary android
