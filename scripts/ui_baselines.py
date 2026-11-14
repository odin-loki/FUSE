#!/usr/bin/env python3
"""FUSE UI §6 baselines (docs/ui/BASELINES.md): binary sizes, startup to first frame, idle CPU / wakeups, memory and
frame times, measured from existing build trees. Prints a Markdown table (and --json writes the raw numbers).

Build trees (any subset; missing ones are skipped):
  --tree DIR     full FUSE tree (fuse_runtime_smoke, fuse_app_host, fuse_editor_fuse, fuse_editor_ui_gates)
  --ui-min DIR   standalone UI build, -DFUSE_UI_TEXT=MINIMAL  (cmake -S Source/FUSE/UI ...)
  --ui-full DIR  standalone UI build, -DFUSE_UI_TEXT=FULL

Windows: run from a normal desktop session (the windows really show). Linux: run under a display, e.g.
  xvfb-run -a -s "-screen 0 1920x1080x24" python3 scripts/ui_baselines.py --ui-min build-ui ...
with VK_ICD_FILENAMES pointing at the driver to measure (lavapipe on CI). Sizes on Linux are of `strip`ped copies;
MSVC Release executables carry no symbols (PDBs are separate), so their file size is the stripped size.
"""
import argparse
import ctypes
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""
REPO = Path(__file__).resolve().parent.parent


def find(root, name):
    """First file called name(.exe) under root."""
    if not root:
        return None
    for p in Path(root).rglob(name + EXE):
        if p.is_file():
            return p
    return None


def stripped_size(path):
    if WINDOWS or not shutil.which("strip"):
        return path.stat().st_size
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / path.name
        subprocess.run(["strip", "-o", str(out), str(path)], check=True)
        return out.stat().st_size


def run(cmd, timeout=120):
    t0 = time.perf_counter()
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=timeout)
    return time.perf_counter() - t0, p.returncode, p.stdout + p.stderr


def median_of(values):
    return statistics.median(values) if values else None


# ---- memory of a running process ------------------------------------------------------------------------------------
def memory_kb(pid):
    """(resident KB, peak resident KB) of a live process."""
    if WINDOWS:
        class PMC(ctypes.Structure):
            _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
        h = ctypes.windll.kernel32.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ
        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        ok = ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
        ctypes.windll.kernel32.CloseHandle(h)
        return (pmc.WorkingSetSize // 1024, pmc.PeakWorkingSetSize // 1024) if ok else (None, None)
    rss = hwm = None
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                rss = int(line.split()[1])
            elif line.startswith("VmHWM:"):
                hwm = int(line.split()[1])
    return rss, hwm


def idle_memory(cmd, settle=3.0):
    """Starts an app, lets it settle into its idle loop, reads its memory, then kills it."""
    p = subprocess.Popen([str(c) for c in cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(settle)
        if p.poll() is not None:
            return None
        return memory_kb(p.pid)
    finally:
        p.kill()
        p.wait()


# ---- measurements ---------------------------------------------------------------------------------------------------
def first_frame(exe, extra, runs):
    """hello_app --startup-probe: (median ms from process creation to the first presented frame, median ms to the
    first Vulkan-presented frame after the async switch (None without one), first presenter)."""
    first, switch = [], []
    out = ""
    for _ in range(runs):
        _, rc, out = run([exe, "--startup-probe", *extra])
        m = re.search(r"first_frame_ms=([\d.]+) presenter=(\w+)", out)
        if rc == 0 and m:
            first.append(float(m.group(1)))
            s = parse(r"switch_ms=([\d.]+)", out)
            if s is not None and s > 0:
                switch.append(s)
    pres = re.search(r"presenter=(\w+)", out)
    gpu = re.search(r"hello_app: presenter [^\n]*? on ([^\n(]+)", out)
    note = (pres.group(1) if pres else "?") + (f" first, then Vulkan on {gpu.group(1).strip()}" if switch and gpu else "")
    return median_of(first), median_of(switch), note


def parse(pattern, text, cast=float):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None


def best_of(cmd, pattern, runs=3, timeout=300):
    vals = []
    for _ in range(runs):
        _, _, out = run(cmd, timeout)
        v = parse(pattern, out)
        if v is not None:
            vals.append(v)
    return min(vals) if vals else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tree")
    ap.add_argument("--ui-min")
    ap.add_argument("--ui-full")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--idle-seconds", type=float, default=10.0)
    ap.add_argument("--json")
    a = ap.parse_args()
    r = {"platform": f"{platform.system()} {platform.release()}", "machine": platform.processor() or platform.machine(),
         "date": time.strftime("%Y-%m-%d"), "sizes": {}, "startup": {}, "idle": {}, "memory": {}, "frames": {}}

    # sizes (stripped)
    for profile, root in (("MINIMAL", a.ui_min), ("FULL", a.ui_full), ("tree", a.tree)):
        for name in ("hello_app", "notes_app", "game_menu", "hud_demo", "fuse_app_host", "fuse_hybrid_host", "fuse_runtime_smoke", "fuse_editor_fuse"):
            exe = find(root, name)
            if exe:
                r["sizes"][f"{name} ({profile})"] = stripped_size(exe)

    # startup to first presented frame
    hello = find(a.ui_min, "hello_app") or find(a.tree, "hello_app")
    if hello:
        r["startup"]["hello_app default (software first frame, async Vulkan)"] = first_frame(hello, [], a.runs)
        r["startup"]["hello_app synchronous Vulkan start-up (GPU draw)"] = first_frame(hello, ["--sync-vulkan"], a.runs)
        r["startup"]["hello_app synchronous Vulkan, CPU upload"] = first_frame(hello, ["--sync-vulkan", "--cpu-upload"], a.runs)
        r["startup"]["hello_app software only (--no-vulkan)"] = first_frame(hello, ["--no-vulkan"], a.runs)
        # Vulkan start-up phases (FUSE_APP_VK_TRACE=1 on stderr), median per phase over the runs
        phases = {}
        env = dict(os.environ, FUSE_APP_VK_TRACE="1")
        for _ in range(a.runs):
            p = subprocess.run([hello, "--startup-probe"], capture_output=True, text=True, timeout=120, env=env)
            for name, ms in re.findall(r"fuse_app vk: ([\w. ]+?)\s+([\d.]+) ms$", p.stderr, re.M):
                phases.setdefault(name.strip(), []).append(float(ms))
        r["vk_phases"] = {k: (min(v), median_of(v)) for k, v in phases.items()}
        # steady frames on the real window: small change (hover) and whole window, software and final presenter
        for label, extra in (("software", ["--no-vulkan"]), ("final presenter", [])):
            hover, full = [], []
            for _ in range(3):
                _, rc, out = run([hello, "--frame-probe", *extra])
                h, f = parse(r"hover_frame_ms=([\d.]+)", out), parse(r"full_frame_ms=([\d.]+)", out)
                if rc == 0 and h is not None:
                    hover.append(h)
                    full.append(f)
            if hover:
                r["frames"][f"hello_app frame probe, {label}: hover / whole-window frame median ms"] = (
                    f"{median_of(hover):.3f} / {median_of(full):.3f}")
    host = find(a.tree, "fuse_app_host")
    template = REPO / "Templates" / "FuseUIApp"
    if host:
        for label, extra in (("headless", ["--headless"]), ("native", [])):
            walls, inner = [], []
            for _ in range(a.runs):
                wall, rc, out = run([host, template, "--first-frame", *extra])
                v = parse(r"first frame ([\d.]+) ms", out)
                if rc == 0 and v is not None:
                    walls.append(wall * 1e3)
                    inner.append(v)
            r["startup"][f"fuse_app_host FuseUIApp ({label}): in-process first frame / process spawn→exit"] = (
                median_of(inner), median_of(walls), label)
    smoke = find(a.tree, "fuse_runtime_smoke")
    if smoke:
        walls = []
        for _ in range(a.runs):
            wall, rc, _ = run([smoke, "--gpu-frame"], 300)
            if rc == 0:
                walls.append(wall * 1e3)
        r["startup"]["fuse_runtime_smoke --gpu-frame: whole run (core + renderer + 4 offscreen GPU frames)"] = (
            median_of(walls), None, "process spawn→exit")

    # idle: real window, nothing to do
    idle = find(a.ui_min, "fuse_app_idle_native") or find(a.tree, "fuse_app_idle_native")
    if idle:
        _, rc, out = run([idle, f"--seconds={a.idle_seconds}"], 120 + a.idle_seconds)
        m = re.search(r"idle ([\d.]+) s: (\d+) frames presented, (\d+) loop wakeups, CPU ([\d.]+) ms \(([\d.]+) %\)", out)
        if m:
            r["idle"] = {"seconds": float(m.group(1)), "frames": int(m.group(2)), "wakeups": int(m.group(3)),
                         "cpu_ms": float(m.group(4)), "cpu_pct": float(m.group(5)), "ok": rc == 0,
                         "switches": parse(r"(\d+) context switches", out, int)}

    # memory of idle apps
    if hello:
        r["memory"]["hello_app (vulkan)"] = idle_memory([hello])
        r["memory"]["hello_app (software only)"] = idle_memory([hello, "--no-vulkan"])
    if host:
        r["memory"]["fuse_app_host FuseUIApp"] = idle_memory([host, template])
    shell = find(a.tree, "fuse_editor_fuse")
    if shell:
        r["memory"]["fuse_editor_fuse (editor shell)"] = idle_memory([shell], settle=5.0)

    # frame times
    for profile, root in (("MINIMAL", a.ui_min), ("FULL", a.ui_full)):
        bench = find(root, "fuse_ui_bench_10k")
        if bench:
            r["frames"][f"fuse_ui_bench_10k median ms ({profile})"] = best_of([bench], r"median ([\d.]+) ms")
        raster = find(root, "fuse_ui_cpu_raster_bench")
        if raster:
            _, _, out = run([raster, "--report"], 600)
            for case, ms, one, ref in re.findall(r"^\s+([\w-]+)\s+\d+x\d+\s+\d+ cmds\s+full\s+([\d.]+) ms.*?one thread\s+"
                                                 r"([\d.]+) ms.*?reference\s+([\d.]+) ms", out, re.M):
                r["frames"][f"CPU raster {case}: full / one thread / pre-change median ms ({profile})"] = (
                    f"{float(ms):.2f} / {float(one):.2f} / {float(ref):.2f}")
            inc = re.search(r"incremental .*?: ([\d.]+) ms .*?reference ([\d.]+) ms", out)
            if inc:
                r["frames"][f"CPU raster incremental (one button of cards-1080p) / pre-change median ms ({profile})"] = (
                    f"{float(inc.group(1)):.3f} / {float(inc.group(2)):.3f}")
        budget = find(root, "fuse_ui_vk_budget")
        if budget:
            _, _, out = run([budget])
            for case, ms in re.findall(r"^\s+([\w-]+)\s+\d+ cmds,.*?median ([\d.]+) ms", out, re.M):
                r["frames"][f"fuse_ui_vk_budget {case} GPU median ms, 1920x1080 ({profile})"] = float(ms)
            r["frames"]["vk device"] = parse(r"device: ([^\n]+)", out, str)
    gates = find(a.tree, "fuse_editor_ui_gates")
    if gates:
        with tempfile.TemporaryDirectory() as d:
            cmd = [gates, "--gate=ui_frame", f"--out={d}/ui_frame.png"]
            r["frames"]["FUSE editor shell hover frame ms"] = best_of(cmd, r"ui_frame: ([\d.]+) ms")
            r["frames"]["FUSE editor shell full-window CPU raster ms"] = best_of(cmd, r"CPU raster ([\d.]+) ms")

    print(f"# FUSE UI baselines — {r['platform']} — {r['date']}\n")
    print("| Binary | Stripped size |\n|---|---|")
    for k, v in r["sizes"].items():
        print(f"| {k} | {v / 1e6:.2f} MB |")
    print("\n| Startup | First presented frame | Vulkan frame / second figure | Notes |\n|---|---|---|---|")
    for k, (first, second, note) in r["startup"].items():
        fmt = lambda x: "—" if x is None else f"{x:.0f} ms"
        print(f"| {k} | {fmt(first)} | {fmt(second)} | {note} |")
    if r.get("vk_phases"):
        print("\n| Vulkan start-up phase (FUSE_APP_VK_TRACE) | min | median |\n|---|---|---|")
        for k, (lo, med) in r["vk_phases"].items():
            print(f"| {k} | {lo:.1f} ms | {med:.1f} ms |")
    if r["idle"]:
        i = r["idle"]
        print(f"\nIdle real window: {i['seconds']:.1f} s, {i['frames']} frames, {i['wakeups']} loop wakeups, "
              f"CPU {i['cpu_ms']:.1f} ms ({i['cpu_pct']:.3f} %)" + (f", {i['switches']} context switches" if i['switches'] else ""))
    print("\n| Idle memory | Resident | Peak |\n|---|---|---|")
    for k, v in r["memory"].items():
        print(f"| {k} | " + ("— | — |" if not v or v[0] is None else f"{v[0] / 1024:.1f} MB | {v[1] / 1024:.1f} MB |"))
    print("\n| Frame time | Value |\n|---|---|")
    for k, v in r["frames"].items():
        print(f"| {k} | {v if isinstance(v, str) or v is None else f'{v:.3f}'} |")
    if a.json:
        Path(a.json).write_text(json.dumps(r, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
