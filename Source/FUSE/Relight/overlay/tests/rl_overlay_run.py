#!/usr/bin/env python3
"""FUSE Relight RL-6.1: Wine test driver for the in-game developer overlay (d3d9.dll / d3d8.dll in-process).

Runs RL-0.4 test apps through the Relight DLLs under cmake/toolchains/fuse-wine-xvfb-run.sh (run directories and
environment handling: RL-1.1's rl_tap_run.py, imported from --tap-tools), with the capture tap and
relight.frame.mode = passthrough (the frame orchestration the overlay draws through) and the overlay's stats record
(relight.overlay.statsPath). Scripted input (relight.overlay.script) goes through the real window hook: SendMessage
into the subclassed WndProc of the app's window.

  hidden    overlay available but never shown, the script sends game input every frame: every dump bit-identical to
            FUSE_RELIGHT=0; every scripted game message forwarded to the app's WndProc, none consumed; no overlay round
            (nothing drawn); the WndProc hook installed.
  shown     relight.overlay.startVisible (deterministic text): the back buffer after the overlay (relight.overlay.
            dumpPath, last frame) equals the game's image (the app's own dump, FUSE_RELIGHT=0) outside the panel
            rectangle bit for bit, and inside it equals tests/golden/<app>.png (--update-golden rewrites it).
  options   Alt+X, Options tab, filter "useVertexCapture", click the value, Save: rtx.conf (DXVK_RTX_CONFIG_FILE) now
            holds rtx.useVertexCapture = False and keeps its other lines.
  tag       Alt+X, Textures tab, click U on texture 0, Save: rtx.conf holds rtx.uiTextures = <hash of the first
            texture the frame sampled, from the capture record>.
  capture   Alt+X, Capture tab, Capture: the RL-1.8 capture of the next frame is written (USDA stage present).
  validation  the host validation layer + synchronization validation (as rl_frame_run.py's validation): frame
            passthrough alone vs frame passthrough + the overlay shown on every frame, in DXVK's legacy binding model
            (no message id's count grows) and its descriptor-buffer model (no new message id).

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb, or no validation layer for 'validation').
"""
import argparse
import glob
import json
import os
import shutil
import struct
import sys
import zlib

SKIP = 77

CAPTURE = {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture"}
FRAME = dict(CAPTURE, FUSE_RELIGHT_FRAME_MODE="passthrough", FUSE_RELIGHT_FRAME_STATS="relight_frame.jsonl")
OVERLAY = dict(FRAME, FUSE_RELIGHT_OVERLAY="1", FUSE_RELIGHT_OVERLAY_STATS="relight_overlay.jsonl",
               FUSE_RELIGHT_OVERLAY_DETERMINISTIC="1")


def winpath(path):
    return "Z:" + os.path.abspath(path).replace("/", "\\")


def write_script(out_dir, name, text):
    path = os.path.join(os.path.abspath(out_dir), name + ".script")
    with open(path, "w") as f:
        f.write(text)
    return winpath(path)


def load_tap_tools(path):
    sys.path.insert(0, path)
    import rl_tap_run  # noqa: E402
    return rl_tap_run


def records(run_dir, name="relight_overlay.jsonl"):
    path = os.path.join(run_dir, name)
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def run(tools, args, exe, run_dir, env):
    rc, text = tools.run_app(args, exe, run_dir, env)
    if rc != 0 and rc != SKIP:
        print(text.strip()[-4000:])
    return rc, text


def read(path):
    with open(path, "rb") as f:
        return f.read()


# ---- minimal PNG (RGBA8, filter 0) -----------------------------------------------------------------------------------
def png_write(path, w, h, rgba):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    rows = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def png_read(path):
    data = read(path)
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, w, h, idat = 8, 0, 0, b""
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype != 6:
                raise ValueError("golden must be RGBA8")
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    out = bytearray()
    stride = w * 4
    for y in range(h):
        row = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        if row[0] != 0:
            raise ValueError("golden rows must use filter 0")
        out += row[1:]
    return w, h, bytes(out)


def crop(rgba, w, x, y, cw, ch):
    return b"".join(rgba[((y + r) * w + x) * 4:((y + r) * w + x + cw) * 4] for r in range(ch))


def overlay_dump(path):
    data = read(path)
    magic, w, h, _ = struct.unpack("<IIII", data[:16])
    if magic != 0x564F4C52:
        raise ValueError("bad overlay dump header")
    return w, h, data[16:]


def check_header(recs, app):
    header = next((r for r in recs if r.get("ev") == "header"), None)
    if not header:
        return f"{app}: no overlay header record (the overlay did not attach)"
    if not header.get("hook"):
        return f"{app}: the window hook was not installed"
    if header.get("script") is False and header.get("script_error"):
        return f"{app}: script: {header['script_error']}"
    return None


# ---- cases -------------------------------------------------------------------------------------------------------------
def cmd_hidden(tools, args):
    exe, app = args.exe[0], args.app
    ref_dir = os.path.join(args.out, "reference_relight_off")
    rc, text = run(tools, args, exe, ref_dir, {"FUSE_RELIGHT": "0"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: reference run exited with {rc}")
        return 1
    per_frame = 24
    script = write_script(args.out, "hidden", "".join(f"{f} game {per_frame}\n" for f in range(64)))
    run_dir = os.path.join(args.out, "overlay_hidden")
    rc, _ = run(tools, args, exe, run_dir, dict(OVERLAY, FUSE_RELIGHT_OVERLAY_SCRIPT=script))
    if rc != 0:
        print(f"FAIL: {app}: overlay run exited with {rc}")
        return 1
    failed = False
    ref, got = tools.dumps(ref_dir), tools.dumps(run_dir)
    if got != ref or not ref:
        print(f"FAIL: {app}: dumps {got} != reference {ref}")
        return 1
    for d in ref:
        if tools.sha(os.path.join(ref_dir, d)) != tools.sha(os.path.join(run_dir, d)):
            print(f"FAIL: {app}: {d}: differs from FUSE_RELIGHT=0 with the overlay hidden")
            failed = True
    recs = records(run_dir)
    if recs is None:
        print(f"FAIL: {app}: no overlay record: the overlay did not attach")
        return 1
    err = check_header(recs, app)
    if err:
        print("FAIL: " + err)
        return 1
    frames = [r for r in recs if r.get("ev") == "overlay"]
    sent = sum(r["script_commands"] for r in frames) * per_frame
    last = frames[-1] if frames else {}
    if not frames or any(r["visible"] for r in frames):
        print(f"FAIL: {app}: the overlay was shown")
        failed = True
    if last.get("rounds", 1) != 0:
        print(f"FAIL: {app}: {last.get('rounds')} overlay round(s) while hidden")
        failed = True
    # forwarded also counts the window's own traffic (activation, paint ...): at least every scripted message.
    if sent == 0 or last.get("forwarded", 0) < sent or last.get("consumed") != 0 or last.get("events") != 0:
        print(f"FAIL: {app}: input not passed through: sent {sent}, forwarded {last.get('forwarded')}, consumed "
              f"{last.get('consumed')}, menu events {last.get('events')}")
        failed = True
    if any(r["script_error"] for r in frames):
        print(f"FAIL: {app}: script error {frames[-1]['script_error']}")
        failed = True
    if failed:
        return 1
    print(f"PASS: {app}: overlay hidden: {len(ref)} dump(s) bit-identical to FUSE_RELIGHT=0; {sent} scripted game "
          f"message(s) through the WndProc hook, all forwarded, none consumed; no overlay round in {len(frames)} "
          f"frame(s)")
    return 0


def cmd_shown(tools, args):
    exe, app = args.exe[0], args.app
    ref_dir = os.path.join(args.out, "reference_relight_off")
    rc, text = run(tools, args, exe, ref_dir, {"FUSE_RELIGHT": "0"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: reference run exited with {rc}")
        return 1
    run_dir = os.path.join(args.out, "overlay_shown")
    rc, _ = run(tools, args, exe, run_dir, dict(OVERLAY, FUSE_RELIGHT_OVERLAY_VISIBLE="1",
                                                FUSE_RELIGHT_OVERLAY_DUMP="relight_overlay.bin"))
    if rc != 0:
        print(f"FAIL: {app}: overlay run exited with {rc}")
        return 1
    recs = records(run_dir) or []
    err = check_header(recs, app)
    if err:
        print("FAIL: " + err)
        return 1
    frames = [r for r in recs if r.get("ev") == "overlay"]
    last = frames[-1] if frames else {}
    if not frames or not all(r["visible"] for r in frames) or last.get("rounds") != len(frames):
        print(f"FAIL: {app}: overlay rounds {last.get('rounds')} over {len(frames)} shown frame(s); error "
              f"'{last.get('error')}'")
        return 1
    dump = os.path.join(run_dir, "relight_overlay.bin")
    if not os.path.isfile(dump):
        print(f"FAIL: {app}: no overlay dump")
        return 1
    w, h, after = overlay_dump(dump)
    game = read(os.path.join(ref_dir, app + ".rgba"))
    if len(game) != w * h * 4 or len(after) != w * h * 4:
        print(f"FAIL: {app}: dump sizes differ ({len(after)} vs {len(game)}, {w}x{h})")
        return 1
    px, py, pw, ph = last["panel"]
    if last["region"] != last["panel"]:
        print(f"FAIL: {app}: drawn region {last['region']} != panel {last['panel']}")
        return 1
    outside_diff = 0
    inside_diff = 0
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 4
            # RGB: the app's GetRenderTargetData dump of an X8R8G8B8 back buffer reports alpha 255, the overlay's raw
            # copy keeps whatever alpha the draws left (never presented).
            same = after[o:o + 3] == game[o:o + 3]
            if px <= x < px + pw and py <= y < py + ph:
                inside_diff += 0 if same else 1
            elif not same:
                outside_diff += 1
    failed = False
    if outside_diff:
        print(f"FAIL: {app}: {outside_diff} pixel(s) outside the panel rectangle {last['panel']} differ from the game's "
              f"image")
        failed = True
    if inside_diff == 0:
        print(f"FAIL: {app}: the overlay drew nothing inside its rectangle")
        failed = True
    panel = crop(after, w, px, py, pw, ph)
    golden = os.path.join(args.golden_dir, app + ".png")
    if args.update_golden or not os.path.isfile(golden):
        os.makedirs(args.golden_dir, exist_ok=True)
        png_write(golden if args.update_golden else os.path.join(args.out, app + ".golden_candidate.png"), pw, ph,
                  panel)
        if not args.update_golden:
            print(f"FAIL: {app}: no golden {golden}; candidate written to {args.out}")
            return 1
        print(f"updated golden {golden}")
    gw, gh, grgba = png_read(golden)
    if (gw, gh) != (pw, ph) or grgba != panel:
        png_write(os.path.join(args.out, app + ".panel.png"), pw, ph, panel)
        diff = sum(1 for i in range(0, min(len(grgba), len(panel)), 4) if grgba[i:i + 4] != panel[i:i + 4])
        print(f"FAIL: {app}: panel {pw}x{ph} differs from the golden {gw}x{gh} ({diff} pixel(s)); see "
              f"{args.out}/{app}.panel.png")
        failed = True
    if failed:
        return 1
    print(f"PASS: {app}: overlay shown in {len(frames)} frame(s) ({last['rounds']} round(s)); outside the panel "
          f"{last['panel']} bit-identical to the game's image, {inside_diff} pixel(s) inside == golden {app}.png")
    return 0


def rtx_conf(out_dir, name, text):
    path = os.path.join(os.path.abspath(out_dir), name)
    with open(path, "w") as f:
        f.write(text)
    return path


def cmd_options(tools, args):
    exe, app = args.exe[0], args.app
    conf = rtx_conf(args.out, "options.rtx.conf", "rtx.preTransformedVerticesIsUI = True\n")
    script = write_script(args.out, "options",
                          "0 toggle\n1 clickw tab.opts\n1 clickw opt.filter\n1 type useVertexCapture\n"
                          "1 clickw opt.rtx.useVertexCapture\n1 clickw save\n")
    run_dir = os.path.join(args.out, "overlay_options")
    rc, text = run(tools, args, exe, run_dir, dict(OVERLAY, FUSE_RELIGHT_OVERLAY_SCRIPT=script,
                                                   DXVK_RTX_CONFIG_FILE=winpath(conf)))
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: overlay run exited with {rc}")
        return 1
    recs = records(run_dir) or []
    err = check_header(recs, app)
    if err:
        print("FAIL: " + err)
        return 1
    frames = [r for r in recs if r.get("ev") == "overlay"]
    last = next((r for r in reversed(frames) if r["visible"]), {})
    saved = read(conf).decode()
    failed = False
    if any(r["script_error"] for r in frames):
        print(f"FAIL: {app}: script: {frames[-1]['script_error']}")
        failed = True
    if last.get("edits") != 1 or last.get("saves") != 1 or last.get("tab") != "opts":
        print(f"FAIL: {app}: edits {last.get('edits')} saves {last.get('saves')} tab {last.get('tab')}")
        failed = True
    if "rtx.useVertexCapture = False" not in saved or "rtx.preTransformedVerticesIsUI = True" not in saved:
        print(f"FAIL: {app}: rtx.conf after Save:\n{saved}")
        failed = True
    if failed:
        return 1
    print(f"PASS: {app}: scripted option edit (filter, click, Save) through the window hook: rtx.conf now has "
          f"rtx.useVertexCapture = False and keeps its other settings")
    return 0


def first_texture_hash(capture_path, frame):
    with open(capture_path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("ev") == "draw" and r.get("frame") == frame:
                for t in r.get("textures", []):
                    if t.get("hash") and int(t["hash"], 16) != 0:
                        return t["hash"].upper()
    return None


def cmd_tag(tools, args):
    exe, app = args.exe[0], args.app
    conf = rtx_conf(args.out, "tag.rtx.conf", "")
    script = write_script(args.out, "tag", "0 toggle\n1 clickw tab.tex\n1 clickw tex.0.ui\n1 clickw save\n")
    run_dir = os.path.join(args.out, "overlay_tag")
    rc, text = run(tools, args, exe, run_dir, dict(OVERLAY, FUSE_RELIGHT_OVERLAY_SCRIPT=script,
                                                   DXVK_RTX_CONFIG_FILE=winpath(conf),
                                                   FUSE_RELIGHT_TAP_CAPTURE_PATH="relight_capture.jsonl"))
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: overlay run exited with {rc}")
        return 1
    recs = records(run_dir) or []
    err = check_header(recs, app)
    if err:
        print("FAIL: " + err)
        return 1
    expected = first_texture_hash(os.path.join(run_dir, "relight_capture.jsonl"), 1)
    if not expected:
        print(f"FAIL: {app}: no textured draw in frame 1 of the capture record")
        return 1
    frames = [r for r in recs if r.get("ev") == "overlay"]
    shown = next((r for r in frames if r["frame"] == 1), {})
    listed = [t["hash"] for t in shown.get("textures", [])]
    saved = read(conf).decode()
    failed = False
    if any(r["script_error"] for r in frames):
        print(f"FAIL: {app}: script: {frames[-1]['script_error']}")
        failed = True
    if not listed or listed[0] != expected:
        print(f"FAIL: {app}: the Textures tab lists {listed[:4]}, expected {expected} first")
        failed = True
    if shown.get("tags") != 1 or shown.get("saves") != 1:
        print(f"FAIL: {app}: tags {shown.get('tags')} saves {shown.get('saves')}")
        failed = True
    if f"rtx.uiTextures = 0x{expected}" not in saved:
        print(f"FAIL: {app}: rtx.conf after Save lacks rtx.uiTextures = 0x{expected}:\n{saved}")
        failed = True
    if failed:
        return 1
    print(f"PASS: {app}: scripted click-to-tag: texture 0 of the frame (0x{expected}, as the capture record hashes it) "
          f"-> rtx.uiTextures in rtx.conf ({len(listed)} texture(s) listed)")
    return 0


def cmd_capture(tools, args):
    exe, app = args.exe[0], args.app
    script = write_script(args.out, "capture", "0 toggle\n0 clickw tab.cap\n0 clickw cap.go\n")
    run_dir = os.path.join(args.out, "overlay_capture")
    rc, text = run(tools, args, exe, run_dir, dict(OVERLAY, FUSE_RELIGHT_OVERLAY_SCRIPT=script,
                                                   FUSE_RELIGHT_OVERLAY_CAPTURE_DIR="overlay_capture"))
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: overlay run exited with {rc}")
        return 1
    recs = records(run_dir) or []
    err = check_header(recs, app)
    if err:
        print("FAIL: " + err)
        return 1
    frames = [r for r in recs if r.get("ev") == "overlay"]
    last = frames[-1] if frames else {}
    cap = last.get("capture", {})
    stages = glob.glob(os.path.join(run_dir, "overlay_capture*", "**", "*.usda"), recursive=True)
    failed = False
    if any(r["script_error"] for r in frames):
        print(f"FAIL: {app}: script: {frames[-1]['script_error']}")
        failed = True
    if not cap.get("written") or not cap.get("ok"):
        print(f"FAIL: {app}: capture status {cap} ({last.get('error')})")
        failed = True
    if not stages:
        print(f"FAIL: {app}: no USDA stage under {run_dir}/overlay_capture*")
        failed = True
    if failed:
        return 1
    print(f"PASS: {app}: Capture button -> RL-1.8 capture of {cap.get('frames')} frame(s) written "
          f"({len(stages)} USDA file(s), dir {cap.get('dir')})")
    return 0


def cmd_validation(tools, args):
    layer = next((p for p in tools.HOST_LAYER_JSON if os.path.isfile(p)), None)
    if not layer:
        print("SKIP: VK_LAYER_KHRONOS_validation is not installed on the host")
        return SKIP
    common = {
        "VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
        "VK_KHRONOS_VALIDATION_REPORT_FLAGS": "error,warn",
        "VK_LAYER_ENABLES": "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT",
        "VK_KHRONOS_VALIDATION_VALIDATE_SYNC": "true",
        "DXVK_LOG_LEVEL": "info",
        "FUSE_RELIGHT_VK_VALIDATION": "1",
    }
    legacy = {"DXVK_CONFIG": "dxvk.enableDescriptorBuffer = False"}
    runs = []
    for model, extra in (("legacy", legacy), ("default", {})):
        runs += [(model + "_frame", dict(FRAME, FUSE_RELIGHT_OVERLAY="0", **extra)),
                 (model + "_overlay", dict(OVERLAY, FUSE_RELIGHT_OVERLAY_VISIBLE="1", **extra))]
    failed = False
    lines = []
    for exe in args.exe:
        app = tools.app_name_of(exe)
        result = {}
        for name, env in runs:
            e = dict(common)
            e.update(env)
            run_dir = os.path.join(args.out, app, name)
            rc, text = run(tools, args, exe, run_dir, e)
            if rc == SKIP:
                print(text.strip())
                return SKIP
            if rc != 0:
                print(f"FAIL: {app} ({name}): exited with {rc}")
                failed = True
                break
            result[name] = tools.validation_messages(text)
            if name.endswith("_overlay"):
                recs = records(run_dir) or []
                frames = [r for r in recs if r.get("ev") == "overlay"]
                if not frames or frames[-1].get("rounds") != len(frames):
                    print(f"FAIL: {app} ({name}): overlay rounds {frames[-1].get('rounds') if frames else None} "
                          f"of {len(frames)} ({frames[-1].get('error') if frames else ''})")
                    failed = True
        if len(result) != len(runs):
            continue
        grown = result["legacy_overlay"][1] - result["legacy_frame"][1]
        if grown:
            failed = True
            print(f"FAIL: {app}: the overlay adds validation messages (legacy binding model): {dict(grown)}")
        new_ids = set(result["default_overlay"][1]) - set(result["default_frame"][1])
        if new_ids:
            failed = True
            print(f"FAIL: {app}: the overlay adds validation message ids (descriptor buffers): {sorted(new_ids)}")
        lines.append(f"{app}: messages frame/overlay = legacy {result['legacy_frame'][0]}/"
                     f"{result['legacy_overlay'][0]}, default {result['default_frame'][0]}/"
                     f"{result['default_overlay'][0]}")
    for line in lines:
        print("  " + line)
    if failed:
        return 1
    print(f"PASS: {len(args.exe)} app(s): the overlay (window hook, compose pass, extra composite round) adds no "
          f"validation or synchronization-validation message over the frame orchestration (host layer {layer})")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--tap-tools", required=True)
    p.add_argument("--exe", action="append", required=True)
    p.add_argument("--app", required=True)
    p.add_argument("--d3d9", required=True)
    p.add_argument("--d3d8", required=True)
    p.add_argument("--runner", required=True)
    p.add_argument("--prefix-root", required=True)
    p.add_argument("--golden-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "golden"))
    p.add_argument("--update-golden", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("hidden", "shown", "options", "tag", "capture", "validation"):
        s = sub.add_parser(name)
        s.add_argument("--out", required=True)
    args = p.parse_args()
    if not shutil.which("wine") and not shutil.which("wine64"):
        print("SKIP: wine not installed")
        return SKIP
    tools = load_tap_tools(args.tap_tools)
    os.makedirs(args.out, exist_ok=True)
    return {"hidden": cmd_hidden, "shown": cmd_shown, "options": cmd_options, "tag": cmd_tag,
            "capture": cmd_capture, "validation": cmd_validation}[args.cmd](tools, args)


if __name__ == "__main__":
    sys.exit(main())
