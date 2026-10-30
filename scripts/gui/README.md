# FUSE GUI platform test scripts

Each script builds `Source/FUSE/UI`, runs its tests and ends with a `==== <platform> summary ====` block and a final
`RESULT: <platform> PASS|FAIL` line (exit code 0/1). They mirror the CI matrix in `docs/gui/ci/fuse-gui.yml`.
Dependencies are installed where sensible (apt/pkg/brew/winget/sdkmanager); `FUSE_NO_DEPS=1` disables that.
`BUILD_DIR` and `JOBS` are honoured everywhere.

| script | host | what it does |
|---|---|---|
| `test-linux.sh [headless] [x11] [wayland]` | Linux | build + ctest headless, under Xvfb, and under headless weston |
| `test-wine.sh` | Linux | llvm-mingw cross-build, ctest under Wine + Xvfb (initialises the prefix, incl. PATH) |
| `test-windows.ps1` | Windows | MSVC (vcvars via vswhere) + Ninja build, ctest |
| `test-macos.sh` | macOS | Cocoa build + ctest |
| `test-ios-sim.sh` | macOS + Xcode | iOS simulator build of `fuse_app_native` (build only) |
| `test-android.sh` | Linux/macOS | NDK APK build, boots an AVD (or uses `SERIAL`), emulator smoke: frame, tap, IME, a11y |
| `test-freebsd.sh` | FreeBSD | pkg deps, base clang build + ctest |
| `run-freebsd-vm.sh` | Linux | boots a FreeBSD 14.4 cloud image in qemu, copies sources, runs `test-freebsd.sh` inside |
| `test-all.sh [platform...]` | any | runs every script applicable to the host, prints one table, logs in `build-gui-logs/` |

Notes: `VM_ACCEL=tcg` / `ANDROID_ACCEL=off` force software emulation where nested KVM is present but hangs.
A Linux FreeBSD cross-build without a VM: `docs/gui/ci/freebsd-cross.cmake`.
