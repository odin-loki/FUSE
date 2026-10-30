# Testing the FUSE GUI on every platform

All scripts live in `scripts/gui/` (see its README for the per-script env vars). Each builds `Source/FUSE/UI`,
runs its tests, prints a `==== <platform> summary ====` block of PASS/FAIL/SKIP steps and ends with
`RESULT: <platform> PASS|FAIL` (exit 0/1). They mirror the staged CI workflow `docs/gui/ci/fuse-gui.yml`
(copy it to `.github/workflows/` to enable CI).

```sh
scripts/gui/test-all.sh                 # everything runnable on this host, one table, logs in build-gui-logs/
scripts/gui/test-linux.sh headless x11 wayland   # Xvfb, headless weston; lavapipe used for Vulkan when present
scripts/gui/test-wine.sh                # llvm-mingw cross-build + ctest under Wine (WINEPATH set up)
scripts/gui/test-android.sh             # NDK APK + emulator smoke (frame, tap, IME, TalkBack); ANDROID_ACCEL=off for TCG
scripts/gui/freebsd-cross.sh            # FreeBSD 14.4 cross-build from Linux
scripts/gui/run-freebsd-vm.sh           # FreeBSD 14.4 qemu VM (official image), runs tests in the VM; VM_ACCEL=tcg
scripts/gui/test-freebsd.sh             # natively on FreeBSD
scripts/gui/test-macos.sh / test-ios-sim.sh   # on a Mac with Xcode
pwsh scripts/gui/test-windows.ps1       # on Windows with MSVC
```

| platform | host needed | status on the Linux dev box |
|---|---|---|
| Linux X11/Wayland/headless | Linux | runs |
| Windows (Wine) | Linux | runs |
| Windows (MSVC) | Windows | not run (no Windows host) |
| Android | Linux/macOS + SDK/NDK | runs (software emulator is slow: raise BOOT_TIMEOUT/WAIT_SECS) |
| FreeBSD cross / VM | Linux (+ qemu) | runs; the VM under TCG uses cross mode (build on host, ctest in VM) |
| macOS / iOS simulator | macOS + Xcode | not run (no Apple host) |
