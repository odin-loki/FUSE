#!/usr/bin/env python3
"""Host-side Android gates for fuse_app (run by ctest from the Android build tree).

Installs the fuse_ui_android_demo APK on a device / emulator, drives it with adb (am, input, settings) and reads
the state file the demo keeps current (run-as <package> cat files/fuse_demo_state.txt).

  android_gate.py --gate present|lifecycle|touch|ime|process_death|startup --apk demo.apk --package org.fuse.uidemo --abi x86_64
                  [--adb adb] [--serial emulator-5580] [--shot out.png]

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

    def pid(self):
        return self.dev.sh(f"pidof {self.a.package}").strip()

    def restored(self, s0, what):
        """Waits for a frame from a new process or main() run that restored s0's state."""
        return self.wait(lambda s: (s["pid"] != s0["pid"] or s["runs"] != s0["runs"]) and s["restored"] == "1" and s["has_surface"] == "1" and s["present_check"] == "pass"
                         and s["taps"] == s0["taps"] and s["text"] == s0["text"] and s["focus"] == s0["focus"]
                         and s["page"] == s0["page"] and abs(float(s["scroll"]) - float(s0["scroll"])) < 1.0, what)

    def gate_process_death(self):
        self.start()
        s = self.ready()
        x, y = self.rect_center(s, "tap")
        for _ in range(2):
            self.dev.sh(f"input tap {x} {y}")
        self.wait(lambda s: s["taps"] == "2", "two taps counted (app state)")
        px, py = self.rect_center(s, "page2")
        self.dev.sh(f"input tap {px} {py}")
        self.wait(lambda s: s["page"] == "2", "third tab selected (current page)")
        lx, ly, lw, lh = (int(v) for v in s["rect_list"].split(","))
        self.dev.sh(f"input swipe {lx + lw // 2} {ly + lh * 3 // 4} {lx + lw // 2} {ly + lh // 4} 400")
        self.wait(lambda s: float(s["scroll"]) > 20.0, "list scrolled")
        ex, ey = self.rect_center(s, "edit")
        self.dev.sh(f"input tap {ex} {ey}")
        self.dev.sh("input text saved_state")
        s0 = self.wait(lambda s: s["text"] == "saved_state" and s["focus"] == "1", "line edit focused with text")
        time.sleep(1.0)  # let the list settle (fling) before the snapshot that is compared
        s0 = self.state()
        pid0 = s0["pid"]
        self.dev.sh("input keyevent KEYCODE_HOME")
        self.wait(lambda s: s["has_surface"] == "0" and int(s["states_saved"]) >= 1, "home: state saved, surface released")
        # 1) process death: kill the background process, relaunch it from its task
        killed = False
        for _ in range(20):
            self.dev.sh(f"am kill {self.a.package}")
            time.sleep(0.5)
            if not self.pid():
                killed = True
                break
        self.check(killed, "am kill ended the background process")
        self.start(fresh=False)
        s = self.restored(s0, "relaunched process restored focus, text, page, scroll and app state")
        self.check(s.get("pid") != pid0, f"new process (pid {pid0} -> {s.get('pid')})")
        # 2) the low-memory killer's way: SIGKILL of the background process (run-as: the APK is debuggable)
        x, y = self.rect_center(s, "tap")
        self.dev.sh(f"input tap {x} {y}")
        self.wait(lambda s: s["taps"] == "3", "tap after restore counted (the button takes focus)")
        time.sleep(0.5)
        s1 = self.state()
        self.dev.sh("input keyevent KEYCODE_HOME")
        self.wait(lambda s: s["has_surface"] == "0" and int(s["states_saved"]) >= 1, "home again: state saved")
        self.dev.sh(f"run-as {self.a.package} kill -9 {s1['pid']}")
        time.sleep(1.0)
        self.check(not self.pid(), "SIGKILL ended the background process")
        self.start(fresh=False)
        self.restored(s1, "relaunched after SIGKILL: state restored again (taps 3, focus on the button)")
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

    # ---- engine renderer (fuse_runtime_android) --------------------------------------------------------------
    def gate_runtime(self):
        vk = self.dev.sh("pm has-feature android.hardware.vulkan.version").strip()
        if vk != "true":
            self.log("device has no Vulkan: skip")
            return SKIP
        self.dev.sh(f"am force-stop {self.a.package}")
        self.dev.sh(f"run-as {self.a.package} rm -f files/fuse_runtime_state.txt")
        self.dev.sh(f"am start -W -n {self.activity} --es args --frames=120")
        end, s = time.time() + 90, {}
        while time.time() < end:
            out = self.dev.sh(f"run-as {self.a.package} cat files/fuse_runtime_state.txt 2>/dev/null")
            s = dict(l.split("=", 1) for l in out.splitlines() if "=" in l)
            if s.get("done") == "1":
                break
            time.sleep(0.5)
        self.log(f"state: {s}")
        self.check(s.get("done") == "1", "fuse_runtime_android finished")
        self.check(s.get("error", "") == "", f"no error ({s.get('error')})")
        self.check(int(s.get("frames_rendered", "0")) == 120, f"120 frames rendered by RhiContext ({s.get('frames_rendered')})")
        self.check(int(s.get("frames_presented", "0")) >= 110,
                   f"frames presented to the window's swapchain ({s.get('frames_presented')}, {s.get('swapchain_images')} images)")
        self.check(s.get("readback") == "pass", f"raster read-back: black background, white triangle (coverage {s.get('coverage')})")
        if self.a.shot:
            self.screenshot(self.a.shot)
        return 0

    # ---- hello_app start-up (plan §6: launch to first presented frame, mid Android <= 400 ms) ------------------
    def probe(self, args):
        """One cold start of hello_app --startup-probe; returns its numbers (ms) from logcat."""
        d = self.dev
        d.sh(f"am force-stop {self.a.package}")
        d.run("logcat", "-c")
        d.sh(f"am start -W -n {self.activity} --es args {shlex.quote('--startup-probe ' + args)}")
        end = time.time() + 30
        r = {}
        while time.time() < end:
            log = d.run("logcat", "-d", "-s", "fuse_app")
            for line in log.splitlines():
                for key in ("android_main at ", "main at ", "first_frame_ms=", "switch_ms=", "max_err=", "presenter=", "final="):
                    i = line.find(key)
                    if i >= 0:
                        r[key.strip(" =")] = line[i + len(key):].split()[0].rstrip(",")
            if "switch_ms" in r:
                break
            time.sleep(0.3)
        try:
            out = {"launch": float(r["first_frame_ms"]), "main": float(r["main at"]), "first": r["presenter"],
                   "final": r["final"], "switch": float(r["switch_ms"]), "max_err": int(r["max_err"])}
        except (KeyError, ValueError):
            self.log(f"FAIL no start-up numbers for '{args}' (got {r})")
            self.failures.append("start-up probe output")
            return None
        out["own"] = out["launch"] - out["main"]
        return out

    def gate_startup(self):
        emulator = self.dev.sh("getprop ro.kernel.qemu").strip() == "1" or self.dev.sh("getprop ro.boot.qemu").strip() == "1"
        modes = {"async": "", "sync": "--sync-vulkan", "software": "--no-vulkan"}
        runs = {m: [] for m in modes}
        self.probe("")  # warm-up (pipeline cache, page cache); not counted
        for _ in range(self.a.runs):  # interleaved, so device load drifts hit every mode alike
            for m, args in modes.items():
                r = self.probe(args)
                if r:
                    runs[m].append(r)

        def med(m, k):
            v = sorted(r[k] for r in runs[m] if r[k] >= 0)
            return v[len(v) // 2] if v else float("nan")

        for m in modes:
            self.log(f"{m:8}: launch -> first frame {med(m, 'launch'):7.1f} ms, main() -> first frame {med(m, 'own'):7.1f} ms "
                     f"(median of {len(runs[m])}; first presenter {[r['first'] for r in runs[m]]})")
        self.log(f"async: Vulkan switch at {med('async', 'switch'):.1f} ms after launch")
        # the first frame is software unless the device was ready before the window arrived
        ok_async = runs["async"] and all(r["final"] == "vulkan" and r["max_err"] == 0 for r in runs["async"])
        self.check(ok_async, "async: ends on the Vulkan presenter showing the CPU raster's pixels (max error 0)")
        self.check(any(r["first"] == "software" for r in runs["async"]), "async: software first frames while Vulkan starts")
        self.check(runs["sync"] and all(r["first"] == "vulkan" for r in runs["sync"]), "sync: first frame through Vulkan")
        gain = med("sync", "own") - med("async", "own")
        self.log(f"async first frame {gain:.1f} ms earlier than sync Vulkan start-up (main() -> first frame medians)")
        if emulator:
            # the emulator's first window buffer (gralloc through the host) alone costs 200-800 ms: the plan budget is for
            # a phone; here the app's own part is held to --emulator-budget-ms
            self.check(med("async", "own") <= self.a.emulator_budget_ms,
                       f"emulator: main() -> first frame {med('async', 'own'):.1f} ms <= {self.a.emulator_budget_ms} ms")
        else:
            self.check(med("async", "launch") <= self.a.budget_ms,
                       f"launch -> first frame {med('async', 'launch'):.1f} ms <= {self.a.budget_ms} ms (plan §6)")
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gate", required=True, choices=["present", "lifecycle", "touch", "ime", "process_death", "startup", "runtime"])
    ap.add_argument("--apk", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--abi", required=True)
    ap.add_argument("--adb", default="adb")
    ap.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL"))
    ap.add_argument("--shot", help="also save adb screencap PNG(s) here")
    ap.add_argument("--runs", type=int, default=5, help="startup: cold starts per mode")
    ap.add_argument("--budget-ms", type=float, default=400.0, help="startup: launch -> first frame on a device")
    ap.add_argument("--emulator-budget-ms", type=float, default=900.0, help="startup: main() -> first frame on an emulator")
    a = ap.parse_args()
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
