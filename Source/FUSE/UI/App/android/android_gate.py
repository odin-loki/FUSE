#!/usr/bin/env python3
"""Host-side Android gates for fuse_app (run by ctest from the Android build tree).

Installs the fuse_ui_android_demo APK on a device / emulator, drives it with adb (am, input, settings) and reads
the state file the demo keeps current (run-as <package> cat files/fuse_demo_state.txt).

  android_gate.py --gate present|lifecycle|touch|ime --apk demo.apk --package org.fuse.uidemo --abi x86_64
                  [--adb adb] [--serial emulator-5580] [--shot out.png]
  android_gate.py --gate hud_bench --apk hud_bench.apk --package org.fuse.hudbench ...   (fuse_ui_bench_hud_mobile)
  android_gate.py --gate startup --apk hello_app.apk --package org.fuse.hello ...        (fuse_app_hello_startup_android)

Exit 0 pass, 1 fail, 77 skip (no adb, no device with the APK's ABI, no Vulkan for the present gate).
ANDROID_SERIAL picks the device when several are attached.
"""
import argparse
import os
import shlex
import subprocess
import sys
import time

SKIP = 77


class Device:
    def __init__(self, adb, serial):
        self.adb, self.serial = adb, serial

    def run(self, *args, check=False, timeout=120):
        r = subprocess.run([self.adb, "-s", self.serial, *args], capture_output=True, timeout=timeout)
        out = r.stdout.decode("utf-8", "replace")
        if check and r.returncode != 0:
            raise RuntimeError(f"adb {' '.join(args)} failed: {out} {r.stderr.decode('utf-8', 'replace')}")
        return out

    def sh(self, cmd, timeout=120):
        return self.run("shell", cmd, timeout=timeout)


def devices(adb):
    try:
        out = subprocess.run([adb, "devices"], capture_output=True, timeout=60).stdout.decode()
    except (OSError, subprocess.TimeoutExpired):
        return None
    return [l.split()[0] for l in out.splitlines()[1:] if l.strip().endswith("\tdevice")]


class Gate:
    def __init__(self, a, dev):
        self.a, self.dev = a, dev
        self.activity = f"{a.package}/android.app.NativeActivity"
        self.failures = []

    def log(self, msg):
        print(f"[{self.a.gate}] {msg}", flush=True)

    def check(self, cond, what):
        self.log(("ok   " if cond else "FAIL ") + what)
        if not cond:
            self.failures.append(what)
        return cond

    def state(self):
        out = self.dev.sh(f"run-as {self.a.package} cat files/fuse_demo_state.txt 2>/dev/null")
        s = {}
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                s[k.strip()] = v.strip()
        return s

    def wait(self, pred, what, timeout=30.0):
        end = time.time() + timeout
        s = {}
        while time.time() < end:
            s = self.state()
            try:
                if s and pred(s):
                    self.log("ok   " + what)
                    return s
            except (KeyError, ValueError):
                pass
            time.sleep(0.3)
        self.log("FAIL " + what + f" (last state: {s})")
        self.failures.append(what)
        return s

    def num(self, s, k):
        return float(s.get(k, "nan"))

    def rect_center(self, s, name):
        x, y, w, h = (int(v) for v in s["rect_" + name].split(","))
        return x + w // 2, y + h // 2

    def setup(self):
        d = self.dev
        out = d.run("install", "-r", "-t", self.a.apk, timeout=300)
        if "Success" not in out:
            d.run("uninstall", self.a.package)
            out = d.run("install", "-t", self.a.apk, timeout=300)
        if "Success" not in out:
            raise RuntimeError("install failed: " + out)
        d.sh("input keyevent KEYCODE_WAKEUP")
        d.sh("wm dismiss-keyguard")
        d.sh("settings put system accelerometer_rotation 0")
        d.sh("settings put system user_rotation 0")

    def start(self, args="", fresh=True):
        d = self.dev
        if fresh:
            d.sh(f"am force-stop {self.a.package}")
            d.sh(f"run-as {self.a.package} rm -f files/fuse_demo_state.txt")
        extra = f" --es args {shlex.quote(args)}" if args else ""
        d.sh(f"am start -W -n {self.activity}{extra}")

    def ready(self, timeout=40.0):
        return self.wait(lambda s: s.get("has_surface") == "1" and int(s["frames"]) > 0 and s["present_check"] != "pending",
                         "app up: surface + first frame checked", timeout)

    def screenshot(self, path):
        data = subprocess.run([self.dev.adb, "-s", self.dev.serial, "exec-out", "screencap", "-p"],
                              capture_output=True, timeout=60).stdout
        if data.startswith(b"\x89PNG"):
            with open(path, "wb") as f:
                f.write(data)
            self.log(f"screenshot {path} ({len(data)} bytes)")

    # ---- gates -------------------------------------------------------------------------------------------------
    def gate_present(self):
        vk = self.dev.sh("pm has-feature android.hardware.vulkan.version").strip()
        if vk != "true":
            self.log("device has no Vulkan: skip")
            return SKIP
        for args, want in (("", "vulkan"), ("--draw=cpu", "vulkan"), ("--vulkan=0", "software")):
            self.log(f"run '{args or 'default'}'")
            self.start(args)
            s = self.ready()
            self.check(s.get("presenter") == want, f"presenter {s.get('presenter')} == {want}")
            self.check(s.get("present_check") == "pass",
                       f"presented frame matches the CPU raster (max error {s.get('present_max_error')}, "
                       f"bad fraction {s.get('present_bad_fraction')}, gpu_draw {s.get('gpu_draw')}, gpu {s.get('gpu')})")
            if not args:
                self.check(s.get("gpu_draw") == "1", "default run draws the UI with the GPU kernel")
                self.check(self.num(s, "scale") > 1.0, f"density scale {s.get('scale')} applied")
                if self.a.shot:
                    self.screenshot(self.a.shot)
        return 0

    def gate_lifecycle(self):
        self.start()
        s = self.ready()
        x, y = self.rect_center(s, "tap")
        for _ in range(2):
            self.dev.sh(f"input tap {x} {y}")
        self.wait(lambda s: s["taps"] == "2", "two taps counted")
        self.dev.sh("input keyevent KEYCODE_HOME")
        s = self.wait(lambda s: s["has_surface"] == "0" and int(s["surfaces_lost"]) >= 1 and s["foreground"] == "0",
                      "home: surface released, app in background")
        self.start(fresh=False)
        s = self.wait(lambda s: s["has_surface"] == "1" and int(s["surfaces_created"]) >= 2 and s["foreground"] == "1"
                      and s["present_checked_surface"] == s["surfaces_created"] and s["present_check"] == "pass",
                      "am start: surface rebuilt and its first frame presented correctly")
        self.check(s.get("taps") == "2", f"UI state kept across the surface loss (taps {s.get('taps')})")
        portrait = s["pixels"]
        w, h = (int(v) for v in portrait.split("x"))
        self.dev.sh("settings put system user_rotation 1")
        s = self.wait(lambda s: s["pixels"] == f"{h}x{w}" and int(s["config_changes"]) >= 1 and s["has_surface"] == "1",
                      f"rotated: window {h}x{w}")
        f0 = int(s.get("frames", "0"))
        s = self.wait(lambda s: int(s["frames"]) >= f0, "frames presented after rotation")
        if self.a.shot:
            self.screenshot(self.a.shot.replace(".png", "_landscape.png"))
        self.dev.sh("settings put system user_rotation 0")
        s = self.wait(lambda s: s["pixels"] == portrait, f"rotated back: window {portrait}")
        self.check(s.get("taps") == "2", f"UI state kept across rotation (taps {s.get('taps')})")
        return 0

    def gate_touch(self):
        self.start()
        s = self.ready()
        x, y = self.rect_center(s, "tap")
        for _ in range(3):
            self.dev.sh(f"input tap {x} {y}")
        s = self.wait(lambda s: s["taps"] == "3", "three taps on the button -> three clicks")
        lx, ly, lw, lh = (int(v) for v in s["rect_list"].split(","))
        cx = lx + lw // 2
        self.dev.sh(f"input swipe {cx} {ly + lh * 3 // 4} {cx} {ly + lh // 4} 400")
        s = self.wait(lambda s: float(s["scroll"]) > 20.0, "swipe up over the list scrolls it")
        self.check(s.get("taps") == "3", "the swipe did not click anything")
        before = float(s.get("scroll", "0"))
        self.dev.sh(f"input swipe {cx} {ly + lh // 4} {cx} {ly + lh * 3 // 4} 400")
        self.wait(lambda s: float(s["scroll"]) < before, "swipe down scrolls back")
        return 0

    def gate_ime(self):
        self.start()
        s = self.ready()
        x, y = self.rect_center(s, "edit")
        self.dev.sh(f"input tap {x} {y}")
        self.wait(lambda s: s["focus"] == "1" and s["keyboard"] == "1", "tap focuses the line edit and requests the soft keyboard")
        time.sleep(1.0)
        ime = self.dev.sh("dumpsys input_method")
        shown = "mInputShown=true" in ime or "isInputViewShown=true" in ime
        self.check(shown, "the soft keyboard is shown (dumpsys input_method)")
        self.dev.sh("input text hello_fuse")
        self.wait(lambda s: s["text"] == "hello_fuse", "adb input text reaches the focused line edit")
        self.dev.sh("input keyevent KEYCODE_DEL")
        self.wait(lambda s: s["text"] == "hello_fus", "backspace edits it")
        if self.a.shot:
            self.screenshot(self.a.shot)
        return 0

    def gate_hud_bench(self):
        # hud_bench APK: the fuse_ui_bench_hud workload; §6 budget 2 ms UI CPU per frame on a mid Android device
        self.start()
        s = self.wait(lambda s: s.get("bench_done") == "1", "HUD bench finished", timeout=300)
        med, p95 = self.num(s, "bench_median_ms"), self.num(s, "bench_p95_ms")
        self.log(f"HUD: {s.get('bench_nodes')} nodes, {s.get('bench_draw_cmds')} draw commands; UI CPU per frame "
                 f"median {med:.3f} ms, p95 {p95:.3f} ms; CPU raster 1080p {s.get('bench_raster_ms')} ms (not gated)")
        self.check(self.num(s, "bench_nodes") >= 250, "typical HUD size (~300 nodes)")
        self.check(med <= self.a.budget_ms, f"median {med:.3f} ms <= {self.a.budget_ms} ms")
        if self.a.shot:
            self.screenshot(self.a.shot)
        return 0

    def gate_startup(self):
        # hello_app --startup-probe: process creation -> first presented frame (in-app, /proc/self/stat start time),
        # cold process each run (force-stop); `am start -W` TotalTime reported beside it. First run (fresh install:
        # asset extraction) is a warm-up; the gate takes the median of the others against the §6 400 ms budget.
        runs = []
        for i in range(self.a.runs + 1):
            d = self.dev
            d.sh(f"am force-stop {self.a.package}")
            d.run("logcat", "-c")
            out = d.sh(f"am start -W -n {self.activity} --es args --startup-probe")
            total = next((int(l.split(":")[1]) for l in out.splitlines() if l.strip().startswith("TotalTime:")), -1)
            line = ""
            end = time.time() + 60
            while time.time() < end and "first_frame_ms=" not in line:
                log = d.run("logcat", "-d", "-v", "raw", "-s", "fuse_app")
                line = next((l for l in log.splitlines() if "first_frame_ms=" in l), "")
                if not line:
                    time.sleep(0.3)
            if not line:
                self.check(False, f"run {i}: no first_frame_ms line in logcat")
                continue
            ms = float(line.split("first_frame_ms=")[1].split()[0])
            presenter = line.split("presenter=")[1].split()[0] if "presenter=" in line else "?"
            self.log(f"run {i}{' (warm-up)' if i == 0 else ''}: first frame {ms:.1f} ms ({presenter}), am TotalTime {total} ms")
            if i:
                runs.append((ms, total))
            time.sleep(1.0)
        if not runs:
            return 0
        first = sorted(r[0] for r in runs)[len(runs) // 2]
        total = sorted(r[1] for r in runs)[len(runs) // 2]
        self.log(f"median launch -> first presented frame {first:.1f} ms (am TotalTime median {total} ms), budget {self.a.budget_ms} ms")
        # The budget is for a mid Android device. An emulator spends ~0.4-0.5 s in its own process / activity start
        # before main() and creates Vulkan on SwiftShader (CPU): report there, gate only with --strict.
        emulator = self.dev.sh("getprop ro.kernel.qemu").strip() == "1" or self.dev.sh("getprop ro.boot.qemu").strip() == "1"
        if emulator and not self.a.strict:
            self.log(f"emulator: not a mid Android device, budget not enforced (first frame {first:.1f} ms): skip")
            return SKIP
        self.check(first <= self.a.budget_ms, f"first frame {first:.1f} ms <= {self.a.budget_ms} ms")
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", required=True, choices=["present", "lifecycle", "touch", "ime", "hud_bench", "startup"])
    ap.add_argument("--budget-ms", type=float, help="hud_bench: 2 ms per frame; startup: 400 ms (plan §6)")
    ap.add_argument("--runs", type=int, default=5, help="startup: measured cold launches (after one warm-up)")
    ap.add_argument("--strict", action="store_true", help="startup: enforce the budget on an emulator too")
    ap.add_argument("--apk", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--abi", required=True)
    ap.add_argument("--adb", default="adb")
    ap.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL"))
    ap.add_argument("--shot", help="also save adb screencap PNG(s) here")
    a = ap.parse_args()
    if a.budget_ms is None:
        a.budget_ms = 400.0 if a.gate == "startup" else 2.0
    if not os.path.exists(a.apk):
        print(f"no APK at {a.apk}: skip")
        return SKIP
    devs = devices(a.adb)
    if devs is None:
        print("adb not runnable: skip")
        return SKIP
    if a.serial:
        devs = [d for d in devs if d == a.serial]
    serial = None
    for d in devs:
        abis = Device(a.adb, d).sh("getprop ro.product.cpu.abilist")
        if a.abi in abis.strip().split(","):
            serial = d
            break
    if not serial:
        print(f"no device / emulator with ABI {a.abi} attached: skip")
        return SKIP
    g = Gate(a, Device(a.adb, serial))
    g.log(f"device {serial}")
    try:
        g.setup()
        rc = getattr(g, "gate_" + a.gate)()
    except Exception as e:  # adb errors, timeouts
        g.failures.append(str(e))
        rc = 1
    finally:
        g.dev.sh("settings put system user_rotation 0")
        g.dev.sh(f"am force-stop {a.package}")
    if rc == SKIP:
        return SKIP
    if g.failures:
        print(f"[{a.gate}] FAILED: " + "; ".join(g.failures))
        return 1
    print(f"[{a.gate}] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
