#!/usr/bin/env bash
# Builds org.fuse.ui.hello (NativeActivity sample + FuseActivity) without Gradle:
#   ANDROID_SDK=... ANDROID_NDK=... JAVA_HOME=... ./build_apk.sh <out-dir> [abi...]
# FUSE_ANDROID_VK=OFF drops the Vulkan swapchain presenter (CPU ANativeWindow_lock blit only).
# Uses: cmake + ninja (NDK toolchain), javac, d8, aapt2, zipalign, apksigner (build-tools 34), keytool.
set -euo pipefail
OUT=$(realpath -m "${1:?out dir}"); shift
ABIS=("${@:-arm64-v8a}")
HERE=$(cd "$(dirname "$0")" && pwd)
UI=$(cd "$HERE/../.." && pwd)
: "${ANDROID_SDK:?}" "${ANDROID_NDK:?}" "${JAVA_HOME:?}"
export PATH="$JAVA_HOME/bin:$PATH"
BT="$ANDROID_SDK/build-tools/34.0.0"
JAR="$ANDROID_SDK/platforms/android-34/android.jar"
mkdir -p "$OUT/stage/lib" "$OUT/classes"
for abi in "${ABIS[@]}"; do
    cmake -S "$UI" -B "$OUT/build-$abi" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM=android-26 -DFUSE_BUILD_UI_TESTS=OFF -DFUSE_BUILD_UI_DESIGNER=OFF \
        -DFUSE_BUILD_GUI_EXAMPLES=OFF -DFUSE_BUILD_UI_THEORA=OFF -DFUSE_BUILD_UI_VK="${FUSE_ANDROID_VK:-ON}" -DFUSE_BUILD_APP_SAMPLES=ON >/dev/null
    ninja -C "$OUT/build-$abi" fuse_android_hello >/dev/null
    mkdir -p "$OUT/stage/lib/$abi"
    "$ANDROID_NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -o "$OUT/stage/lib/$abi/libfuse_android_hello.so" \
        "$OUT/build-$abi/App/libfuse_android_hello.so"
done
"$JAVA_HOME/bin/javac" -source 8 -target 8 -bootclasspath "$JAR" -classpath "$JAR" -d "$OUT/classes" \
    -Xlint:-options "$HERE"/java/org/fuse/ui/*.java
"$BT/d8" --min-api 26 --lib "$JAR" --output "$OUT/stage" $(find "$OUT/classes" -name '*.class')
FONTS="$UI/../../../vendor/fonts"
rm -rf "$OUT/assets" && mkdir -p "$OUT/assets/fonts/opensans" "$OUT/assets/fonts/noto" "$OUT/assets/fonts/twemoji"
cp "$FONTS/opensans/OpenSans.ttf" "$OUT/assets/fonts/opensans/"
cp "$FONTS"/noto/NotoSans{Hebrew,Arabic,Devanagari,Symbols2}-Regular.ttf "$OUT/assets/fonts/noto/"
cp "$FONTS/twemoji/TwemojiMozilla.ttf" "$OUT/assets/fonts/twemoji/"
"$BT/aapt2" link -o "$OUT/base.apk" --manifest "$HERE/AndroidManifest.xml" -I "$JAR" -A "$OUT/assets"
python3 - "$OUT/base.apk" "$OUT/stage" <<'PY'
import os, sys, zipfile
apk, stage = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(stage):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, stage))
PY
"$BT/zipalign" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"
KS="$OUT/debug.keystore"
[ -f "$KS" ] || "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass android -keypass android -alias debug \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=FUSE debug" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --out "$OUT/fuse_hello.apk" "$OUT/aligned.apk"
echo "$OUT/fuse_hello.apk"
