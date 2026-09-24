#!/usr/bin/env python3
"""FUSE Relight RL-4.2: Wine test driver of the raster remaster (d3d9.dll, relight.frame.mode = raster).

Runs an RL-0.4 test app through the Relight d3d9.dll under cmake/toolchains/fuse-wine-xvfb-run.sh (run directories and
environment handling: RL-1.1's rl_tap_run.py, imported from --tap-tools), with the capture tap, the frame orchestration
in raster mode and FUSE_RENDER_TIER_MAX = the tier (the adopted device's RendererCaps tier is capped by it):

  tier         the last frame's image (the app's dump when injected at the first UI draw, else FUSE's output dump:
               relight.frame.dumpPath; an app without UI reads its back buffer before Present, i.e. before the
               composite) vs the committed golden Tests/relight/golden/raster/<app>_<tier>.png (exact, or
               within the documented tolerance: per channel <= 2 on <= 0.5% of the pixels, which LLVM / Lavapipe
               floating-point differences stay under), plus the documented checks of the app (CHECKS below) against
               the frame record and the FUSE_RELIGHT=0 reference. --bless writes the golden from this run instead.
  decal        ff_alpha with its alpha-ramp texture listed in rtx.decalTextures (its hash read from a first run's
               capture record): the two alpha-tested cells become Decal-category draws, drawn by the decal pass
               (blended into the G-buffer albedo, depth-tested, alpha test kept): per frame 2 decals and one more
               graph pass than without; the rejected (low-alpha) quarter still shows the background exactly, the
               kept quarter is brighter than it.
  fog          ff_fog at T0: the frame fog is its first fogged draw's (vertex LINEAR, start 2, end 22, colour
               0x8090a0); the far wall (z = 40, beyond FOGEND) at the app's three probes is exactly the fog colour
               (+- 1), the nearest pillar keeps more of its own colour than the wall. Per-draw viewports: every draw
               carries its 64 x 48 quadrant rectangle (28 per frame) and the remaster covers the same pixels as the
               game's own four quadrants (IoU >= 0.97 of the non-black masks, every quadrant drawn, equal within 5%):
               the quadrants no longer overlap.
  replace      ff_lit with the RL-3.4 fixture mod lit_lights (a game light replaced, one deleted, lights attached to the
               preserved spheres), capture-only run vs raster run at T1: the replacement record (every replace_frame
               line and draw replacement) is identical (the engine's injection-time half + its flush half = the flush
               alone); in the raster run every frame feeds the GPU scene at the injection point with this frame
               ("feed": "inject", gpu_frame = the frame), at least one replaced draw, and the GPU scene's and the
               remaster's light count = the frame's replaced light list; the image is not the unmodded T1 golden.
  determinism  the same app rendered twice at T1: bit-identical dumps and frame records.
  validation   the host validation layer + synchronization validation (injected into the host loader): the capture tap
               alone vs raster mode, legacy binding model (gate: no message id's count grows) and DXVK's default
               descriptor-buffer model (gate: no new message id), as rl_frame_run.py's validation.

Documented checks (hand-inspected once, then enforced; W x H = 128 x 96, (x, y) from the top-left):
  every app   every frame is injected and rendered by the raster pass ("pass": "raster") with the requested tier;
              T2 renders T1's features (its image is the T1 golden, bit for bit) and reports the absent RT features /
              meshlet path as degraded; T0 has no
              shadow map; the background shows the game's clear colour; blend order = submission order.
  ff_lit      (1, 1) is the black clear colour; the emissive-only sphere (probe from the app's JSON) shows its emissive
              colour (64, 128, 191) +- 8 (black material: only the dielectric specular); lit spheres are shaded (a lit pixel differs
              from the unlit albedo and the sphere tops are brighter than their undersides under the directional light);
              the D3DRS_EMISSIVEMATERIALSOURCE = D3DMCS_COLOR1 sphere (lower left) emits its vertex colour (G = 0xc0 on
              every vertex): one emissive-vertex draw per frame, and each of its pixels (the game's own non-black
              pixels in its box) has G >= 184 even on the side the lights miss.
  ff_alpha    fallback light (no game light); the GREATER alpha-test cell rejects its low-alpha quarter (the pixel
              equals the background seen through the gap next to the cell exactly) and keeps the high-alpha quarter
              (brighter); the
              blended cells are ordered and obey their blend op relations over the background: ONE/ONE add >=
              background, MIN <= background, MAX >= background, REVSUBTRACT <= background.
  sky_ui_hud  injected at the first UI draw (the POSITIONT crosshair); the HUD drawn after it (crosshair, text block)
              equals the FUSE_RELIGHT=0 reference exactly (composite below the UI); the sky (unlit, category Sky) is
              the reference's sky within 3 per channel; the lit floor differs from the reference (remastered
              lighting); from T1 the sun's shadows only darken (>= 15 pixels darker than the T0 golden by more than
              8 in luminance, none brighter by more than 2).
  raster_vs_rhw injected at Present (no UI: rtx.preTransformedVerticesIsUI off for this app); per frame 5 scene draws,
              none skipped: 1 programmable-VS draw from the RL-1.6 object-space positions, 1 from the captured clip
              positions (its constants carry a world translation D3DTS_WORLD does not), 2 XYZRHW draws; the unlit
              XYZRHW backdrop / panel pixels equal their colours (+- 1); the backdrop (z 0.995) shows exactly where the
              game's image shows it (<= 1% of its band differs: depth from DXVK's POSITIONT mapping); the VS quads keep
              their vertex colours' hue at the app's probes; the non-black coverage matches the game's (IoU >= 0.97).

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb, or no validation layer for 'validation').
"""
import argparse
import json
import os
import shutil
import struct
import sys
import zlib

SKIP = 77
W, H = 128, 96
TIERS = {"t0": 0, "t1": 1, "t2": 2}

CAPTURE = {"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture"}
RASTER = dict(CAPTURE, FUSE_RELIGHT_FRAME_MODE="raster", FUSE_RELIGHT_FRAME_STATS="relight_frame.jsonl",
              FUSE_RELIGHT_FRAME_DUMP="relight_output.rgba")


# ---- PNG (RGB8, no third-party code) -----------------------------------------------------------------------------

def png_write(path, rgb, w, h):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def png_read(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6), "RGB8 / RGBA8 PNG only"
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xff
            elif f == 2:
                line[i] = (line[i] + b) & 0xff
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xff
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xff
        prev = line
        for x in range(w):
            out += line[x * bpp:x * bpp + 3]
    return bytes(out), w, h


def rgb_of(rgba):
    return bytes(b for i in range(0, len(rgba), 4) for b in rgba[i:i + 3])


def px(rgb, x, y):
    o = (y * W + x) * 3
    return tuple(rgb[o:o + 3])


def lum(p):
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]


def close(a, b, tol):
    return all(abs(int(x) - int(y)) <= tol for x, y in zip(a, b))


# ---- runs ---------------------------------------------------------------------------------------------------------

def load_tap_tools(path):
    sys.path.insert(0, path)
    import rl_tap_run  # noqa: E402
    return rl_tap_run


# Apps whose pre-transformed (XYZRHW) draws are scene geometry, not UI.
PRETRANSFORMED_SCENE = {"raster_vs_rhw"}


def rtx_conf(out_dir, app=""):
    """An rtx.conf (Wine path) that makes pre-transformed (POSITIONT) draws UI (as rl_frame_run.py), except for the apps
    whose XYZRHW draws are scene geometry (rtx.preTransformedVerticesIsUI off: Remix rasterizes them, the remaster
    renders them)."""
    scene = app in PRETRANSFORMED_SCENE
    path = os.path.join(os.path.abspath(out_dir), "raster_scene.rtx.conf" if scene else "raster.rtx.conf")
    with open(path, "w") as f:
        f.write("rtx.preTransformedVerticesIsUI = %s\n" % ("False" if scene else "True"))
    return "Z:" + path.replace("/", "\\")


def records(run_dir):
    path = os.path.join(run_dir, "relight_frame.jsonl")
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def run(tools, args, exe, run_dir, env):
    rc, text = tools.run_app(args, exe, run_dir, env)
    if rc != 0 and rc != SKIP:
        print(text.strip()[-4000:])
    return rc, text


def raster_run(tools, args, exe, app, run_dir, tier):
    env = dict(RASTER, FUSE_RENDER_TIER_MAX=str(tier), DXVK_RTX_CONFIG_FILE=rtx_conf(args.out, app))
    rc, text = run(tools, args, exe, run_dir, env)
    if rc != 0:
        return rc, None, None, text
    recs = records(run_dir) or []
    frames = [r for r in recs if r.get("ev") == "frame"]
    # Injected at the first UI draw: the app's own dump shows the composite with the UI on top. Injected at Present
    # (no UI): the app read its back buffer before the composite, so the image is FUSE's output dump.
    at_ui = bool(frames) and all(r.get("inject") == "ui" for r in frames)
    name = app + ".rgba" if at_ui else "relight_output.rgba"
    path = os.path.join(run_dir, name)
    if not os.path.isfile(path):
        return 1, None, recs, text + f"\nno image {name}"
    with open(path, "rb") as f:
        rgba = f.read()
    return rc, rgb_of(rgba), recs, text


def compare(got, want):
    """(identical, max channel difference, fraction of pixels differing)."""
    diff_px = 0
    worst = 0
    for i in range(0, len(want), 3):
        d = max(abs(got[i + c] - want[i + c]) for c in range(3))
        worst = max(worst, d)
        diff_px += 1 if d else 0
    return got == want, worst, diff_px / (len(want) // 3)


# ---- documented checks --------------------------------------------------------------------------------------------

def check_common(app, tier, recs, fail):
    header = next((r for r in recs if r.get("ev") == "header"), {})
    frames = [r for r in recs if r.get("ev") == "frame"]
    if not frames:
        fail("no frame records")
        return frames
    for r in frames:
        rs = r.get("raster", {})
        if r.get("pass") != "raster" or not rs.get("rendered"):
            fail(f"frame {r['frame']}: not rendered by the raster pass ({r.get('pass')}, {r.get('error')}, "
                 f"{rs.get('error')})")
            continue
        if rs["tier"] != tier:
            fail(f"frame {r['frame']}: tier {rs['tier']}, expected {tier}")
        order = rs.get("blend_order", [])
        if order != sorted(order):
            fail(f"frame {r['frame']}: blended draws out of submission order {order}")
        feats = rs.get("features", "").split(",")
        if "gbuffer" not in feats or "clustered" not in feats:
            fail(f"frame {r['frame']}: minimum features missing: {feats}")
        if tier == 0 and ("shadows" in feats or rs.get("shadow")):
            fail(f"frame {r['frame']}: T0 renders shadows")
        if tier >= 1 and "shadows" not in feats:
            fail(f"frame {r['frame']}: T{tier} without shadows: {feats}")
        degraded = rs.get("degraded", "")
        if tier == 2 and not all(x in degraded for x in ("rt_shadows", "rt_reflections", "ddgi", "meshlet_path")):
            fail(f"frame {r['frame']}: T2 does not report the absent RT features as degraded: '{degraded}'")
        if tier == 0 and degraded:
            fail(f"frame {r['frame']}: T0 degraded '{degraded}'")
    rd = header.get("raster", {})
    if header and rd.get("tier") != tier:
        fail(f"header tier plan {rd}")
    return frames


def app_json(run_dir, app):
    with open(os.path.join(run_dir, app + ".json")) as f:
        return json.load(f)


def check_ff_lit(rgb, ref, frames, run_dir, tier, fail, note):
    if px(rgb, 1, 1) != (0, 0, 0):
        fail(f"background (1, 1) = {px(rgb, 1, 1)}, expected the black clear colour")
    probes = [p for p in app_json(run_dir, "ff_lit").get("probes", []) if "emissive" in p.get("what", p.get("name", ""))]
    for p in probes:
        x, y = p["x"], p["y"]
        if not close(px(rgb, x, y), (64, 128, 191), 8):
            fail(f"emissive-only sphere ({x}, {y}) = {px(rgb, x, y)}, expected (64, 128, 191) +- 8")
        note(f"emissive probe ({x},{y}) = {px(rgb, x, y)}")
    if not probes:
        fail("no emissive probe in ff_lit.json")
    last = frames[-1]["raster"]
    if last["lights"] < 2 or last["fallback_light"]:
        fail(f"ff_lit: game lights expected ({last['lights']}, fallback {last['fallback_light']})")
    # Sphere 0 (world -1.25, 0.55): its top is lit by the directional light (travelling -y), its bottom is not.
    col = [px(rgb, x, y) for x in range(20, 44) for y in range(10, 60)]
    lit = [p for p in col if p != (0, 0, 0)]
    if len(lit) < 50:
        fail("ff_lit: sphere 0 not rendered")
        return
    top = max(range(10, 60), key=lambda y: 0 if px(rgb, 32, y) == (0, 0, 0) else -y)
    ys = [y for y in range(10, 60) if px(rgb, 32, y) != (0, 0, 0)]
    t, b = px(rgb, 32, ys[0] + 2), px(rgb, 32, ys[-1] - 2)
    note(f"sphere 0 column x=32: top {t}, bottom {b}")
    if not lum(t) > lum(b):
        fail(f"ff_lit: sphere 0 top {t} not brighter than its bottom {b} (directional light from above)")
    del top
    if last.get("emissive_vertex") != 1:
        fail(f"ff_lit: {last.get('emissive_vertex')} emissive-vertex draw(s), expected 1 (D3DMCS_COLOR1 sphere)")
    dim = [(x, y, px(rgb, x, y)) for x in range(44, 61) for y in range(54, 73)
           if px(ref, x, y) != (0, 0, 0) and px(rgb, x, y)[1] < 184]
    note(f"emissive COLOR1 sphere (52, 64) = {px(rgb, 52, 64)}; {len(dim)} pixel(s) with G < 184")
    if dim:
        fail(f"ff_lit: the COLOR1-emissive sphere does not emit its vertex colour (G 0xc0): {dim[:4]}")


def check_ff_alpha(rgb, ref, frames, run_dir, tier, fail, note):
    last = frames[-1]["raster"]
    if not last["fallback_light"]:
        fail("ff_alpha: no game light, the fallback light must be on")
    if last["blended"] < 8:
        fail(f"ff_alpha: {last['blended']} blended draw(s), expected the 8 blend cells in the forward pass")
    # Cell geometry from the app: cell i at world (x, y) = (-2.3 + 1.2 (i % 4), 0.9 - 1.2 (i / 4)), size 1; camera
    # (0, 0, -3.6), fov 60, 128 x 96, quads at z = 0.5 -> distance 4.1.
    import math
    f = 1.0 / math.tan(math.pi / 6.0)

    def to_px(wx, wy, wz=0.5):
        d = wz + 3.6
        return int((wx * f / (W / H) / d * 0.5 + 0.5) * W), int((0.5 - wy * f / d * 0.5) * H)

    def cell(i):
        return -2.3 + 1.2 * (i % 4), 0.9 - 1.2 * (i // 4)

    def cell_px(i, u, v):
        x, y = cell(i)
        return to_px(x + u, y + v)

    def bg_next(i):
        # The background seen through the gap left of the cell (screen position of the gap at the cells' depth).
        x, y = cell(i)
        return to_px(x - 0.1, y + 0.5)

    # Alpha test (cell 0: GREATER 0x80): low alpha rejected -> exactly the background behind the cell.
    lo = px(rgb, *cell_px(0, 0.12, 0.5))
    hi = px(rgb, *cell_px(0, 0.88, 0.5))
    bg = px(rgb, *bg_next(0))
    note(f"alpha test GREATER: rejected {lo} (background {bg}), kept {hi}")
    if lo != bg:
        fail(f"ff_alpha: the rejected alpha-test pixel {lo} is not the background {bg}")
    if lum(hi) <= lum(lo):
        fail(f"ff_alpha: GREATER keeps the high-alpha quarter ({hi}) brighter than the rejected one ({lo})")
    # Blend relations (cells 3 ONE/ONE add, 8 MIN, 9 MAX, 7 REVSUBTRACT) against the background next to them.
    rel = {3: ">=", 8: "<=", 9: ">=", 7: "<="}
    for i, op in rel.items():
        c = px(rgb, *cell_px(i, 0.5, 0.5))
        b = px(rgb, *bg_next(i))
        ok = all(cc >= bb for cc, bb in zip(c, b)) if op == ">=" else all(cc <= bb for cc, bb in zip(c, b))
        note(f"blend cell {i}: {c} {op} background {b}")
        if not ok:
            fail(f"ff_alpha: blend cell {i} {c} not {op} the background {b}")


def check_sky_ui_hud(rgb, ref, frames, run_dir, tier, fail, note):
    if any(r["inject"] != "ui" for r in frames):
        fail(f"sky_ui_hud: injections {[r['inject'] for r in frames]}, expected the first UI draw")
    for x, y, what in ((64, 48, "crosshair"), (6, 6, "text block")):
        if px(rgb, x, y) != px(ref, x, y):
            fail(f"HUD {what} ({x}, {y}) = {px(rgb, x, y)}, reference {px(ref, x, y)} (HUD must stay on top)")
    # Sky: upper rows (above the horizon) unlit = the reference within 3.
    sky_bad = [(x, 2) for x in range(40, 128, 8) if not close(px(rgb, x, 2), px(ref, x, 2), 3)]
    note(f"sky (100, 2): {px(rgb, 100, 2)} vs reference {px(ref, 100, 2)}")
    if sky_bad:
        fail(f"sky pixels differ from the reference's unlit sky: {[(p, px(rgb, *p), px(ref, *p)) for p in sky_bad[:4]]}")
    # Floor: remastered lighting differs from the game's Gouraud lighting.
    floor = [(x, 80) for x in range(70, 100, 4)]
    if all(px(rgb, *p) == px(ref, *p) for p in floor):
        fail("sky_ui_hud: the lit floor equals the reference (no remastered lighting)")
    last = frames[-1]["raster"]
    if last["unlit"] < 1:
        fail("sky_ui_hud: no unlit (sky) draw")
    note(f"floor (80, 80): {px(rgb, 80, 80)} vs reference {px(ref, 80, 80)}")


def check_raster_vs_rhw(rgb, ref, frames, run_dir, tier, fail, note):
    """Programmable-VS draws from the RL-1.6 capture and XYZRHW draws land where the game draws them."""
    if any(r["inject"] != "present" for r in frames):
        fail(f"raster_vs_rhw: injections {[r['inject'] for r in frames]}, expected present (no UI)")
    last = frames[-1]["raster"]
    want = {"draws": 5, "skipped": 0, "vertex_captured": 1, "capture_clip": 1, "pretransformed": 2}
    got = {k: last.get(k) for k in want}
    note(f"draw sources {got}")
    if got != want:
        fail(f"raster_vs_rhw: draw sources {got}, expected {want}")
    if last.get("skips"):
        fail(f"raster_vs_rhw: skipped draws {last['skips']}")
    backdrop, panel = (0x20, 0x38, 0x60), (0xe0, 0xa0, 0x20)
    # Unlit XYZRHW colours pass through; their pixels match the game's.
    for (x, y), c, what in (((4, 4), backdrop, "backdrop"), ((106, 75), panel, "panel")):
        note(f"{what} ({x}, {y}) = {px(rgb, x, y)}")
        if not close(px(rgb, x, y), c, 1):
            fail(f"raster_vs_rhw: XYZRHW {what} ({x}, {y}) = {px(rgb, x, y)}, expected {c} (unlit, as the game)")
    # Depth: the backdrop (z 0.995) is covered wherever the game covers it; the panel (z 0.2) covers everything.
    band = [(x, y) for y in range(40) for x in range(W)]
    wrong = [p for p in band if close(px(rgb, *p), backdrop, 1) != close(px(ref, *p), backdrop, 1)]
    note(f"backdrop band: {len(wrong)} pixel(s) where the backdrop shows in one image only")
    if len(wrong) > len(band) // 100:
        fail(f"raster_vs_rhw: backdrop occlusion differs from the game's at {len(wrong)} pixel(s), e.g. {wrong[:4]}")
    # The VS quads: their probes (the app's) keep the vertex colour's hue; A green, B red.
    probes = {p["what"]: p for p in app_json(run_dir, "raster_vs_rhw").get("probes", [])}
    for key, hue in (("vs_2_0 quad A (vertex colour)", 1), ("vs_2_0 quad B (constants-only world)", 0)):
        p = probes.get(key)
        if not p:
            fail(f"raster_vs_rhw: no probe '{key}'")
            continue
        c = px(rgb, p["x"], p["y"])
        note(f"{key} ({p['x']}, {p['y']}) = {c}")
        if c[hue] <= max(c[k] for k in range(3) if k != hue) or c == (0, 0, 0):
            fail(f"raster_vs_rhw: {key} at ({p['x']}, {p['y']}) = {c} lost its vertex colour's hue")
    # Coverage: every object where the game has it (lighting changes the colours, not the silhouettes).
    covered = lambda img, x, y: px(img, x, y) != (0, 0, 0)
    both = sum(1 for y in range(H) for x in range(W) if covered(rgb, x, y) and covered(ref, x, y))
    either = sum(1 for y in range(H) for x in range(W) if covered(rgb, x, y) or covered(ref, x, y))
    iou = both / either if either else 0.0
    note(f"coverage IoU vs the game {iou:.4f}")
    if iou < 0.97:
        fail(f"raster_vs_rhw: coverage IoU {iou:.4f} with the game's image (>= 0.97)")


CHECKS = {"ff_lit": check_ff_lit, "ff_alpha": check_ff_alpha, "sky_ui_hud": check_sky_ui_hud,
          "raster_vs_rhw": check_raster_vs_rhw}


def cmd_tier(tools, args):
    exe, app, tier = args.exe[0], args.app, TIERS[args.tier]
    failures = []
    notes = []

    def fail(msg):
        failures.append(msg)

    def note(msg):
        notes.append(msg)
    ref_dir = os.path.join(args.out, "reference_relight_off")
    rc, text = run(tools, args, exe, ref_dir, {"FUSE_RELIGHT": "0"})
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: reference run exited with {rc}")
        return 1
    with open(os.path.join(ref_dir, app + ".rgba"), "rb") as f:
        ref = rgb_of(f.read())
    run_dir = os.path.join(args.out, "raster")
    rc, rgb, recs, text = raster_run(tools, args, exe, app, run_dir, tier)
    if rc != 0:
        print(f"FAIL: {app} T{tier}: raster run exited with {rc}")
        return 1
    png_write(os.path.join(args.out, f"{app}_{args.tier}.png"), rgb, W, H)
    frames = check_common(app, tier, recs, fail)
    if frames:
        CHECKS[app](rgb, ref, frames, run_dir, tier, fail, note)
        t1 = os.path.join(args.golden_dir, f"{app}_t1.png")
        if tier == 2 and os.path.isfile(t1) and png_read(t1)[0] != rgb:
            fail(f"T2 differs from the T1 golden: the absent T2 features must degrade to exactly T1's image")
        t0 = os.path.join(args.golden_dir, f"{app}_t0.png")
        if tier >= 1 and app == "sky_ui_hud" and os.path.isfile(t0):
            # Shadows from T1: they only remove light (no pixel brighter than in the T0 golden), and the sun's
            # shadows of the cubes (on the floor and the cubes' own far sides) darken some pixels.
            base, _, _ = png_read(t0)
            darker = sum(1 for y in range(H) for x in range(W) if lum(px(rgb, x, y)) < lum(px(base, x, y)) - 8)
            brighter = sum(1 for y in range(H) for x in range(W) if lum(px(rgb, x, y)) > lum(px(base, x, y)) + 2)
            note(f"shadows: {darker} pixel(s) darker than T0, {brighter} brighter")
            if darker < 15 or brighter:
                fail(f"sky_ui_hud T{tier}: shadows vs T0: {darker} pixel(s) darker (>= 15), {brighter} brighter (0)")
    golden = os.path.join(args.golden_dir, f"{app}_{args.tier}.png")
    if args.bless:
        if failures:
            print("\n".join("FAIL: " + f for f in failures))
            print(f"FAIL: {app} T{tier}: not blessing a run that fails its checks")
            return 1
        os.makedirs(args.golden_dir, exist_ok=True)
        png_write(golden, rgb, W, H)
        print(f"BLESSED: {golden}")
    elif not os.path.isfile(golden):
        fail(f"no golden {golden} (run with --bless to create it)")
    else:
        want, gw, gh = png_read(golden)
        if (gw, gh) != (W, H):
            fail(f"golden {golden} is {gw} x {gh}")
        else:
            same, worst, frac = compare(rgb, want)
            note(f"golden: identical={same} max diff {worst}, {frac * 100:.2f}% pixels differ")
            if not same and (worst > 2 or frac > 0.005):
                fail(f"golden mismatch: max channel difference {worst}, {frac * 100:.2f}% pixels differ "
                     f"(tolerance 2 on <= 0.5%); image: {os.path.join(args.out, f'{app}_{args.tier}.png')}")
    for n in notes:
        print("  " + n)
    if failures:
        print("\n".join("FAIL: " + f for f in failures))
        return 1
    last = frames[-1]["raster"]
    print(f"PASS: {app} T{tier}: raster remaster in {len(frames)} frame(s) ({last['features']}; degraded: "
          f"'{last['degraded'] or 'none'}'), {last['opaque']} opaque / {last['blended']} blended / {last['decals']} "
          f"decal / {last['skipped']} skipped draw(s), {last['lights']} light(s), shadow {last['shadow']}; "
          f"golden {'blessed' if args.bless else 'matched'}")
    return 0


def cmd_decal(tools, args):
    exe, app = args.exe[0], args.app
    rc, rgb0, recs0, text = raster_run(tools, args, exe, app, os.path.join(args.out, "plain"), 1)
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: plain run exited with {rc}")
        return 1
    hashes = set()
    with open(os.path.join(args.out, "plain", "relight_capture.jsonl")) as f:
        for line in f:
            r = json.loads(line)
            if r.get("ev") == "draw":
                hashes.update(t["hash"] for t in r.get("textures", []))
    if len(hashes) != 1:
        print(f"FAIL: {app}: expected one texture hash (the alpha ramp), found {sorted(hashes)}")
        return 1
    conf = os.path.join(os.path.abspath(args.out), "decal.rtx.conf")
    with open(conf, "w") as f:
        f.write("rtx.preTransformedVerticesIsUI = True\nrtx.decalTextures = 0x%s\n" % next(iter(hashes)))
    env = dict(RASTER, FUSE_RENDER_TIER_MAX="1", DXVK_RTX_CONFIG_FILE="Z:" + conf.replace("/", "\\"))
    run_dir = os.path.join(args.out, "decal")
    rc, text = run(tools, args, exe, run_dir, env)
    if rc != 0:
        print(f"FAIL: {app}: decal run exited with {rc}")
        return 1
    with open(os.path.join(run_dir, "relight_output.rgba"), "rb") as f:
        rgb = rgb_of(f.read())
    failed = False
    frames = [r for r in records(run_dir) or [] if r.get("ev") == "frame"]
    plain = [r for r in recs0 if r.get("ev") == "frame"]
    for r, p in zip(frames, plain):
        if r["raster"]["decals"] != 2 or r["passes"] != p["passes"] + 1:
            print(f"FAIL: {app}: frame {r['frame']}: {r['raster']['decals']} decal(s), {r['passes']} pass(es) "
                  f"(plain {p['passes']})")
            failed = True
    import math
    f_ = 1.0 / math.tan(math.pi / 6.0)

    def to_px(wx, wy, wz=0.5):
        d = wz + 3.6
        return int((wx * f_ / (W / H) / d * 0.5 + 0.5) * W), int((0.5 - wy * f_ / d * 0.5) * H)
    lo, hi, bg = px(rgb, *to_px(-2.3 + 0.12, 1.4)), px(rgb, *to_px(-2.3 + 0.88, 1.4)), px(rgb, *to_px(-2.4, 1.4))
    print(f"  decal cell 0: rejected {lo}, kept {hi}, background {bg}")
    if lo != bg or not lum(hi) > lum(bg):
        print(f"FAIL: {app}: decal alpha test / blend: rejected {lo} != background {bg} or kept {hi} not brighter")
        failed = True
    if rgb == rgb0:
        print(f"FAIL: {app}: the decal run renders exactly the plain image")
        failed = True
    if failed:
        return 1
    print(f"PASS: {app}: rtx.decalTextures -> 2 decal draws per frame in the decal pass ({len(frames)} frame(s)); "
          f"alpha test kept, blended into the albedo")
    return 0


def cmd_fog(tools, args):
    exe, app = args.exe[0], args.app
    rc, rgb, recs, text = raster_run(tools, args, exe, app, os.path.join(args.out, "fog"), 0)
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: raster run exited with {rc}")
        return 1
    run_dir = os.path.join(args.out, "fog")
    frames = [r for r in recs if r.get("ev") == "frame"]
    failed = False
    if not frames or any(r["raster"]["fog"] != 3 for r in frames):
        print(f"FAIL: {app}: frame fog {[r['raster'].get('fog') for r in frames]}, expected D3DFOG_LINEAR (3)")
        failed = True
    # Four quadrant viewports x (6 pillars + wall): every draw has its 64 x 48 rectangle.
    if not frames or any(r["raster"].get("viewports") != 28 for r in frames):
        print(f"FAIL: {app}: draws with a viewport rectangle {[r['raster'].get('viewports') for r in frames]}, "
              f"expected 28 per frame")
        failed = True
    fog = (0x80, 0x90, 0xa0)
    # The app's own probes (the far wall of the linear quadrants, beyond FOGEND): the fog colour. The remaster applies
    # the frame fog (Remix: one fog per frame, the first fogged draw's LINEAR 2..22) in every quadrant.
    probes = app_json(run_dir, app).get("probes", [])
    for p in probes:
        got = px(rgb, p["x"], p["y"])
        print(f"  probe {p['what']} ({p['x']}, {p['y']}) = {got}")
        if not close(got, fog, 1):
            print(f"FAIL: {app}: the far wall at ({p['x']}, {p['y']}) {got} is not the fog colour {fog}")
            failed = True
    if len(probes) != 3:
        print(f"FAIL: {app}: {len(probes)} probe(s) in ff_fog.json, expected 3")
        failed = True
    nearest = px(rgb, 17, 32)  # quadrant 0: the nearest pillar's front face (4 units from the eye)
    print(f"  nearest pillar (17, 32) = {nearest}")
    if nearest == (0, 0, 0) or sum(abs(a - b) for a, b in zip(nearest, fog)) < 40:
        print(f"FAIL: {app}: the nearest pillar {nearest} does not keep its own colour against the fog {fog}")
        failed = True
    # No overlap: the remaster covers the same pixels as the game's own quadrants (its back buffer before the
    # composite), the scene drawn once per quadrant rectangle.
    with open(os.path.join(run_dir, app + ".rgba"), "rb") as f:
        game = rgb_of(f.read())
    covered = lambda img, x, y: px(img, x, y) != (0, 0, 0)
    both = either = 0
    per_quadrant = [0, 0, 0, 0]
    for y in range(H):
        for x in range(W):
            a, b = covered(rgb, x, y), covered(game, x, y)
            both += 1 if a and b else 0
            either += 1 if a or b else 0
            per_quadrant[(y // 48) * 2 + x // 64] += 1 if a else 0
    iou = both / either if either else 0.0
    print(f"  coverage vs the game's quadrants: IoU {iou:.4f}; per quadrant {per_quadrant}")
    if iou < 0.97 or min(per_quadrant) == 0 or max(per_quadrant) - min(per_quadrant) > max(per_quadrant) // 20:
        print(f"FAIL: {app}: the quadrant viewports overlap or differ from the game's (IoU {iou:.4f}, per quadrant "
              f"{per_quadrant})")
        failed = True
    if failed:
        return 1
    print(f"PASS: {app}: four quadrant viewports drawn into their rectangles (IoU {iou:.4f} with the game's); D3D "
          f"linear fog in the deferred pass (far wall = fog colour, near geometry {nearest})")
    return 0


def replace_lines(path):
    """The replacement engine's record lines of a capture record: per frame the replace_frame line and every draw's
    replacement, keyed (frame, di)."""
    out = {}
    with open(path) as f:
        for line in f:
            if not line.strip():
                continue
            r = json.loads(line)
            if r.get("ev") == "replace_frame":
                out[("frame", r["frame"])] = r
            elif r.get("ev") == "draw" and "replacement" in r:
                out[(r["frame"], r["di"])] = r["replacement"]
    return out


def cmd_replace(tools, args):
    """RL-3.4 replacements active in raster mode: the GPU scene is fed at the injection point (no frame of lag)."""
    import subprocess
    exe, app = args.exe[0], args.app
    if not (args.stager and args.fixtures):
        print("FAIL: replace needs --stager and --fixtures")
        return 1
    mods_root = os.path.join(os.path.abspath(args.out), "mods")
    if os.path.isdir(mods_root):
        shutil.rmtree(mods_root)
    os.makedirs(mods_root)
    emulator = [p for p in (args.emulator or "").split("|") if p]
    proc = subprocess.run(emulator + [args.stager, "--stage", os.path.join(args.fixtures, "mods", "lit_lights"),
                                      os.path.join(mods_root, "lit_lights")],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        print(proc.stdout.decode(errors="replace")[-2000:])
        print(f"FAIL: {app}: staging the lit_lights fixture mod failed ({proc.returncode})")
        return 1
    mod_env = {"FUSE_RELIGHT_REPLACE_MOD_PATHS": "remix:Z:" + mods_root.replace("/", "\\"),
               "FUSE_RELIGHT_TAP_CAPTURE_PATH": "relight_capture.jsonl", "DXVK_RTX_CONFIG_FILE": rtx_conf(args.out, app)}
    # The capture-only run: the replacement engine at the flush alone (RL-3.4's own path).
    cap_dir = os.path.join(args.out, "capture")
    rc, text = run(tools, args, exe, cap_dir, dict(CAPTURE, **mod_env))
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0:
        print(f"FAIL: {app}: capture run exited with {rc}")
        return 1
    run_dir = os.path.join(args.out, "raster")
    rc, text = run(tools, args, exe, run_dir, dict(RASTER, FUSE_RENDER_TIER_MAX="1", **mod_env))
    if rc != 0:
        print(f"FAIL: {app}: raster run exited with {rc}")
        return 1
    failed = []
    base = replace_lines(os.path.join(cap_dir, "relight_capture.jsonl"))
    live = replace_lines(os.path.join(run_dir, "relight_capture.jsonl"))
    if not any(k[0] == "frame" for k in base):
        failed.append("the capture run has no replace_frame line (no replacement engine?)")
    if base != live:
        diff = sorted(str(k) for k in set(base) | set(live) if base.get(k) != live.get(k))
        failed.append(f"the replacement record differs between the capture-only and the raster run: {diff[:6]}")
    frames = [r for r in records(run_dir) or [] if r.get("ev") == "frame"]
    if not frames:
        failed.append("no frame records")
    for r in frames:
        sc, rs = r["scene"], r["raster"]
        rf = live.get(("frame", r["frame"]), {})
        lights = len(rf.get("lights", []))
        if sc["feed"] != "inject" or sc["gpu_frame"] != r["frame"] or not sc["sink"]:
            failed.append(f"frame {r['frame']}: scene feed {sc['feed']} of frame {sc['gpu_frame']} (sink {sc['sink']}),"
                          f" expected this frame's at the injection point")
        if sc["replaced"] < 1:
            failed.append(f"frame {r['frame']}: {sc['replaced']} replaced draw(s) fed at the injection point")
        if sc["lights"] != lights or rs["lights"] != lights or lights == 0:
            failed.append(f"frame {r['frame']}: GPU-scene lights {sc['lights']}, raster lights {rs['lights']}, "
                          f"expected the frame's replaced light list ({lights})")
        if r.get("pass") != "raster":
            failed.append(f"frame {r['frame']}: not rendered by the raster pass")
    with open(os.path.join(run_dir, "relight_output.rgba"), "rb") as f:
        rgb = rgb_of(f.read())
    golden = os.path.join(args.golden_dir, f"{app}_t1.png")
    if os.path.isfile(golden) and png_read(golden)[0] == rgb:
        failed.append("the image equals the unmodded T1 golden: the replaced / attached lights do not reach the remaster")
    for r in frames[-1:]:
        print(f"  frame {r['frame']}: feed {r['scene']['feed']}, {r['scene']['replaced']} replaced draw(s), "
              f"{r['scene']['lights']} light(s), raster lights {r['raster']['lights']}")
    if failed:
        print("\n".join(f"FAIL: {app}: {f}" for f in failed))
        return 1
    print(f"PASS: {app}: with RL-3.4 replacements (lit_lights) the GPU scene is fed at the injection point with the "
          f"frame's replaced draws and lights ({len(frames)} frame(s)); the replacement record equals the capture-only "
          f"run's")
    return 0


def cmd_determinism(tools, args):
    exe, app = args.exe[0], args.app
    outs = []
    for k in range(2):
        rc, rgb, recs, text = raster_run(tools, args, exe, app, os.path.join(args.out, f"run{k}"), 1)
        if rc == SKIP:
            print(text.strip())
            return SKIP
        if rc != 0:
            print(f"FAIL: {app}: run {k} exited with {rc}")
            return 1
        frames = [r for r in recs if r.get("ev") == "frame"]
        for r in frames:
            for key in ("acquire", "release"):
                r.pop(key, None)
        outs.append((rgb, json.dumps(frames, sort_keys=True)))
    if outs[0][0] != outs[1][0]:
        same, worst, frac = compare(outs[0][0], outs[1][0])
        print(f"FAIL: {app}: two runs differ (max {worst}, {frac * 100:.2f}% pixels)")
        return 1
    if outs[0][1] != outs[1][1]:
        print(f"FAIL: {app}: two runs' frame records differ")
        return 1
    print(f"PASS: {app}: two raster runs bit-identical (dumps and frame records)")
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
    failed = False
    lines = []
    for exe in args.exe:
        app = tools.app_name_of(exe)
        conf = rtx_conf(args.out, app)
        runs = []
        for model, extra in (("legacy", legacy), ("default", {})):
            runs += [(model + "_capture", dict(CAPTURE, DXVK_RTX_CONFIG_FILE=conf, **extra)),
                     (model + "_raster", dict(RASTER, FUSE_RENDER_TIER_MAX="2", DXVK_RTX_CONFIG_FILE=conf, **extra))]
        result = {}
        for name, env in runs:
            e = dict(common)
            e.update(env)
            rc, text = run(tools, args, exe, os.path.join(args.out, app, name), e)
            if rc == SKIP:
                print(text.strip())
                return SKIP
            if rc != 0:
                print(f"FAIL: {app} ({name}): exited with {rc}")
                failed = True
                break
            result[name] = tools.validation_messages(text)
            if "raster" in name:
                recs = records(os.path.join(args.out, app, name)) or []
                frames = [r for r in recs if r.get("ev") == "frame"]
                if not frames or any(r.get("pass") != "raster" for r in frames):
                    print(f"FAIL: {app} ({name}): not every frame rendered by the raster pass")
                    failed = True
        if len(result) != len(runs):
            continue
        grown = result["legacy_raster"][1] - result["legacy_capture"][1]
        if grown:
            failed = True
            print(f"FAIL: {app}: raster adds validation messages (legacy binding model): {dict(grown)}")
        new_ids = set(result["default_raster"][1]) - set(result["default_capture"][1])
        if new_ids:
            failed = True
            print(f"FAIL: {app}: raster adds validation message ids (descriptor buffers): {sorted(new_ids)}")
        lines.append(f"{app}: messages capture/raster = legacy {result['legacy_capture'][0]}/"
                     f"{result['legacy_raster'][0]}, default {result['default_capture'][0]}/"
                     f"{result['default_raster'][0]}")
    for l in lines:
        print("  " + l)
    if failed:
        return 1
    print(f"PASS: {len(args.exe)} app(s): the raster remaster (T2 plan) adds no validation or synchronization-"
          f"validation message over the capture tap alone (host layer {layer})")
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
    p.add_argument("--golden-dir", required=True)
    sub = p.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tier")
    t.add_argument("--tier", required=True, choices=sorted(TIERS))
    t.add_argument("--out", required=True)
    t.add_argument("--bless", action="store_true")
    for name in ("determinism", "validation", "decal", "fog", "replace"):
        s = sub.add_parser(name)
        s.add_argument("--out", required=True)
        if name == "replace":
            s.add_argument("--stager")
            s.add_argument("--fixtures")
            s.add_argument("--emulator", default="")
    args = p.parse_args()
    if not shutil.which("wine") and not shutil.which("wine64"):
        print("SKIP: wine not installed")
        return SKIP
    tools = load_tap_tools(args.tap_tools)
    os.makedirs(args.out, exist_ok=True)
    return {"tier": cmd_tier, "determinism": cmd_determinism, "validation": cmd_validation,
            "decal": cmd_decal, "fog": cmd_fog, "replace": cmd_replace}[args.cmd](tools, args)


if __name__ == "__main__":
    sys.exit(main())
