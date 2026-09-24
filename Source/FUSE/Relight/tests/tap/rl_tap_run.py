#!/usr/bin/env python3
"""FUSE Relight RL-1.1: Wine test driver for the tap seam and device import.

Runs an RL-0.4 test app (Tests/relight/apps) through the Relight d3d9.dll / d3d8.dll under
cmake/toolchains/fuse-wine-xvfb-run.sh (Xvfb + Wine + Lavapipe) in a private run directory that
holds the exe and both DLLs, with the environment of the configuration under test:

  golden      reference: FUSE_RELIGHT=0 (DXVK creates its own device, no tap: the RL-0.2
              behaviour), then relight.device.import on with relight.tap.mode off, null, record
              and capture (the live in-process capture: advisory, every draw stays a raster draw).
              Every dump (<app>.rgba and <app>.<name>.rgba) must be bit-identical to the reference.
  record      relight.tap.mode = record; the event stream must equal the one derived from the
              app's sidecar (rl_tap_expect.py) and report an imported device.
  validation  apps on the imported device with the Khronos validation layer (see cmd_validation).
              winevulkan loads no layers on the PE side, so the layer is injected into the host
              loader (VK_INSTANCE_LAYERS); its messages reach FUSE's debug-utils messenger
              (relight.vk.validation) or, without one, the layer's default log. Skips (77) when the
              host layer is not installed.

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb, or no validation layer for 'validation').
"""
import argparse
import collections
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rl_tap_expect  # noqa: E402

SKIP = 77
HOST_LAYER_JSON = ["/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json",
                   "/etc/vulkan/explicit_layer.d/VkLayer_khronos_validation.json",
                   "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"]


def app_name_of(exe):
    return os.path.splitext(os.path.basename(exe))[0]


def run_app(args, exe, run_dir, env_extra):
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    for src in (exe, args.d3d9, args.d3d8):
        dst = os.path.join(run_dir, os.path.basename(src))
        try:
            os.link(src, dst)  # same file system: no copy of the DLLs per run
        except OSError:
            shutil.copy2(src, dst)
    env = dict(os.environ)
    env.setdefault("DXVK_LOG_LEVEL", "warn")
    env["DXVK_LOG_PATH"] = "none"
    env.update(env_extra)
    local_exe = os.path.join(run_dir, os.path.basename(exe))
    cmd = ["bash", args.runner, "--native-d3d", args.prefix_root, local_exe, "--out", ".", "--quiet"]
    proc = subprocess.run(cmd, cwd=run_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    with open(os.path.join(run_dir, "run.log"), "w") as f:
        f.write(text)
    return proc.returncode, text


def dumps(run_dir):
    return sorted(os.path.basename(p) for p in glob.glob(os.path.join(run_dir, "*.rgba")))


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


CONFIGS = [
    ("import_tap_off", {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "off"}),
    ("import_tap_null", {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "null"}),
    ("import_tap_record", {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "record"}),
    ("import_tap_capture", {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture"}),
]


def cmd_golden(args):
    exe = args.exe[0]
    app = args.app
    ref_dir = os.path.join(args.out, "reference_relight_off")
    rc, text = run_app(args, exe, ref_dir, {"FUSE_RELIGHT": "0"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(text.strip()[-4000:])
        print(f"FAIL: {app}: reference run (FUSE_RELIGHT=0) exited with {rc}")
        return 1
    ref = dumps(ref_dir)
    if not ref:
        print(f"FAIL: {app}: the reference run wrote no .rgba dump")
        return 1
    failed = False
    for name, env in CONFIGS:
        run_dir = os.path.join(args.out, name)
        rc, text = run_app(args, exe, run_dir, env)
        if rc != 0:
            print(text.strip()[-4000:])
            print(f"FAIL: {app}: {name} exited with {rc}")
            failed = True
            continue
        got = dumps(run_dir)
        if got != ref:
            print(f"FAIL: {app}: {name} dumps {got} != reference {ref}")
            failed = True
            continue
        for d in ref:
            a, b = sha(os.path.join(ref_dir, d)), sha(os.path.join(run_dir, d))
            if a != b:
                print(f"FAIL: {app}: {name}/{d} differs from the FUSE_RELIGHT=0 reference ({b[:16]} != {a[:16]})")
                failed = True
        if name == "import_tap_record" and not rl_tap_expect.stream_reports_import(
                os.path.join(run_dir, "relight_tap.jsonl")):
            print(f"FAIL: {app}: {name}: the device was not imported from FUSE (device_create.vulkan.imported)")
            failed = True
    if failed:
        return 1
    print(f"PASS: {app}: {len(ref)} dump(s) bit-identical across FUSE_RELIGHT=0 / import+tap off / null / record / capture")
    return 0


def cmd_record(args):
    exe = args.exe[0]
    app = args.app
    run_dir = os.path.join(args.out, "run")
    rc, text = run_app(args, exe, run_dir, {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "record",
                                             "FUSE_RELIGHT_TAP_RECORD_PATH": "relight_tap.jsonl"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(text.strip()[-4000:])
        print(f"FAIL: {app}: exited with {rc}")
        return 1
    sidecar = os.path.join(run_dir, app + ".json")
    stream = os.path.join(run_dir, "relight_tap.jsonl")
    for p in (sidecar, stream):
        if not os.path.isfile(p):
            print(f"FAIL: {app}: missing {os.path.basename(p)}")
            return 1
    errors, summary = rl_tap_expect.compare_files(sidecar, stream, expect_import=True)
    if errors:
        print(f"FAIL: {app}: the tap event stream differs from the sidecar ({len(errors)} difference(s)):")
        for e in errors[:60]:
            print("  " + e)
        return 1
    print(f"PASS: {app}: {summary}")
    return 0


VVL_LINE = re.compile(r"Validation (Error|Warning|Performance Warning)")
VUID = re.compile(r"\[ ?([A-Za-z0-9_-]+) ?\]")


def validation_messages(text):
    """(count, Counter of message ids) of validation messages in a run's combined output: the host
    layer's default log (no messenger) and FUSE's debug-utils messenger ("fuse-relight vk ...")."""
    ids = collections.Counter()
    for line in text.splitlines():
        if VVL_LINE.search(line):
            m = VUID.search(line)
            ids[m.group(1) if m else "?"] += 1
    return sum(ids.values()), ids


def cmd_validation(args):
    """Every app runs on the imported device and on DXVK's own device (FUSE_RELIGHT=0), each with
    DXVK's legacy binding model (dxvk.enableDescriptorBuffer = False) and with its default
    (descriptor-buffer) model, with the Khronos layer injected into the host loader. Gate: the
    imported device reports no validation message beyond DXVK's own device in the same mode
    (legacy: per-id counts; default: message ids, since the layer's duplicate limit makes those
    counts batching-dependent).
    Reported alongside: the absolute counts. Host VVL 1.3.275 (Ubuntu 24.04) flags DXVK 3.1.1 on
    its own device too (VUID-vkCmdDraw*-None-08117/-08600 with descriptor buffers + graphics
    pipeline libraries; VUID-vkCmdDraw-viewType-07752/-None-06479 for ps_1_x texture views in the
    legacy model); those are upstream / layer-version findings, listed in the output."""
    layer = next((p for p in HOST_LAYER_JSON if os.path.isfile(p)), None)
    if not layer:
        print("SKIP: VK_LAYER_KHRONOS_validation is not installed on the host (vulkan-validationlayers)")
        return SKIP
    common = {
        "VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
        "VK_KHRONOS_VALIDATION_REPORT_FLAGS": "error,warn",
        "DXVK_LOG_LEVEL": "info",
    }
    imported = {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "record", "FUSE_RELIGHT_VK_VALIDATION": "1"}
    own = {"FUSE_RELIGHT": "0"}
    legacy = {"DXVK_CONFIG": "dxvk.enableDescriptorBuffer = False"}
    runs = [
        ("import_legacy", dict(imported, **legacy)),
        ("own_legacy", dict(own, **legacy)),
        ("import_default", imported),
        ("own_default", own),
    ]
    failed = False
    lines = []
    totals = collections.Counter()
    for exe in args.exe:
        app = app_name_of(exe)
        result = {}
        for name, env in runs:
            e = dict(common)
            e.update(env)
            rc, text = run_app(args, exe, os.path.join(args.out, app, name), e)
            if rc == SKIP:
                print(text.strip())
                return SKIP
            if rc != 0:
                print(text.strip()[-4000:])
                print(f"FAIL: {app} ({name}): exited with {rc}")
                failed = True
                break
            is_imported = "Importing device" in text
            if name.startswith("import") != is_imported:
                print(f"FAIL: {app} ({name}): device {'not ' if not is_imported else ''}imported")
                failed = True
            result[name] = validation_messages(text)
            totals[name] += result[name][0]
        if len(result) != len(runs):
            continue
        # Legacy model: message counts are deterministic and must not grow. Default model: the
        # layer's per-message-id duplicate limit makes counts depend on command-buffer batching,
        # so the gate is on message ids: none the own device does not report.
        extra = result["import_legacy"][1] - result["own_legacy"][1]
        if extra:
            failed = True
            print(f"FAIL: {app}: the imported device adds validation messages (legacy binding model): {dict(extra)}")
        new_ids = set(result["import_default"][1]) - set(result["own_default"][1])
        if new_ids:
            failed = True
            print(f"FAIL: {app}: the imported device adds validation message ids (descriptor buffers): {sorted(new_ids)}")
        lines.append(f"{app}: imported/own device messages: legacy {result['import_legacy'][0]}/{result['own_legacy'][0]}, "
                     f"default {result['import_default'][0]}/{result['own_default'][0]}; ids: "
                     f"{', '.join(sorted(set(result['own_legacy'][1]) | set(result['own_default'][1]))) or 'none'}")
    for l in lines:
        print("  " + l)
    if failed:
        return 1
    print(f"PASS: {len(args.exe)} app(s): the imported device adds no validation message over DXVK's own device "
          f"(totals imported/own: legacy {totals['import_legacy']}/{totals['own_legacy']}, "
          f"default {totals['import_default']}/{totals['own_default']}; host layer {layer})")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe", action="append", required=True)
    p.add_argument("--app", required=True)
    p.add_argument("--d3d9", required=True)
    p.add_argument("--d3d8", required=True)
    p.add_argument("--runner", required=True)
    p.add_argument("--prefix-root", required=True)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("golden", "record", "validation"):
        s = sub.add_parser(name)
        s.add_argument("--out", required=True)
    args = p.parse_args()
    if not shutil.which("wine") and not shutil.which("wine64"):
        print("SKIP: wine not installed")
        return SKIP
    os.makedirs(args.out, exist_ok=True)
    return {"golden": cmd_golden, "record": cmd_record, "validation": cmd_validation}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
