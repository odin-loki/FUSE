#!/usr/bin/env python3
"""FUSE Relight RL-1.1: expected tap event stream from an RL-0.4 app sidecar, and the comparison.

The recording tap (Source/FUSE/Relight/tap, relight.tap.mode = record) writes every IRelightTap
event as JSON Lines. An RL-0.4 app (Tests/relight/apps) writes a sidecar with the ground truth it
fed D3D9/D3D8. This module states what the tap must have seen for that sidecar and compares.

Mapping (sidecar -> tap stream)
  device        present parameters 128x96, back buffer X8R8G8B8 (22), auto depth-stencil D24S8
                (75), windowed; vertex_processing hardware/software -> D3DCREATE_HARDWARE /
                SOFTWARE_VERTEXPROCESSING (0x40 / 0x20) in behavior_flags; vulkan.imported when
                device import is expected.
  resource ids  sidecar ids come from one counter in creation order; the tap numbers textures and
                buffers separately in creation order. Sidecar textures (by id) map one-to-one, in
                order, to the tap's texture_create events other than the implicit back buffer and
                auto depth-stencil (device_create.back_buffer / auto_depth_stencil); later tap
                textures (the app kit's read-back surfaces) are ignored. Buffers likewise.
                Checked per resource: size / dimensions, format, usage, pool, FVF, index format.
  buffer data   the n-th buffer_write of a buffer = the n-th sidecar version: offset, size,
                lock_flags and blob (SHA-256 of the whole-buffer shadow; D3DLOCK_DISCARD zeroes
                it first on both sides).
  texture data  per texture, sidecar uploads in order: LockRect -> texture_upload with the same
                level and blob (the locked rows, which DXVK lays out as Remix's canonical packed
                layout); UpdateTexture (one sidecar entry per level) -> one texture_copy
                UpdateTexture from the mapped source; UpdateSurface / CopyRects -> texture_copy
                UpdateSurface with the levels.
  frames        tap "frame" = presents so far = the sidecar frame index; the tap must report
                `frames` presents. Per recorded frame, the sidecar's clears and draws in seq order
                equal the tap's clear / draw events of that frame, in order:
    clear       flags, color, z, stencil, render target 0 (backbuffer or mapped id).
    draw        call, primitive, prim_count and the call's counts; index buffer (mapped id, format,
                blob = the sidecar version's blob); UP vertex / index blobs, stride and format;
                vertex elements (stream, offset, type, size, method, usage, usage index) and FVF;
                bound streams (mapped buffer, offset, stride, blob); every sidecar transform (the
                tap omits identity matrices: missing = identity); shaders (bytecode SHA-256 = the
                sidecar shader blob; D3D8 twins compare presence only, since d3d8 translates the
                bytecode); the state block: every render / stage / sampler state the app set,
                bound textures (both ways), lights (both ways), material, viewport, render target,
                depth-stencil, shader constants (both ways; missing = zero).
    viewport    as DXVK stores it: MinZ >= MaxZ becomes MaxZ = MinZ + 0.001 (float32).
    D3D8        programmable draws compare the vertex layout without usages (d3d8 maps the D3D8
                input registers to D3D9 usages by number); shader bytecode compares presence only.
    tag         not compared: semantic labels are for the classifier packages (RL-1.2+).
Floats compare as float32 (both writers print %.9g).
"""
import json
import struct
import sys

D3DFMT = {"X8R8G8B8": 22, "D24S8": 75, "INDEX16": 101, "INDEX32": 102}
CREATE_SOFTWARE_VP = 0x20
CREATE_HARDWARE_VP = 0x40
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def feq(a, b):
    if isinstance(a, list) or isinstance(b, list):
        return isinstance(a, list) and isinstance(b, list) and len(a) == len(b) and all(
            feq(x, y) for x, y in zip(a, b))
    return f32(a) == f32(b)


def load_stream(path):
    events = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if line:
                try:
                    events.append(json.loads(line))
                except ValueError as e:
                    raise ValueError(f"{path}:{n}: {e}")
    return events


def stream_reports_import(path):
    try:
        events = load_stream(path)
    except (OSError, ValueError):
        return False
    return any(e.get("ev") == "device_create" and e.get("vulkan", {}).get("imported") for e in events)


class Compare:
    def __init__(self, sidecar, events, expect_import):
        self.sc = sidecar
        self.ev = events
        self.expect_import = expect_import
        self.errors = []
        self.d3d8 = sidecar.get("api") == "d3d8"

    def err(self, msg):
        self.errors.append(msg)

    def check(self, cond, msg):
        if not cond:
            self.err(msg)
        return cond

    # ---- device and resources ------------------------------------------------------------------
    def device(self):
        creates = [e for e in self.ev if e["ev"] == "device_create"]
        if not self.check(len(creates) == 1, f"expected 1 device_create, got {len(creates)}"):
            return None
        d = creates[0]
        p = d["present"]
        dev = self.sc["device"]
        self.check(p["back_buffer_width"] == self.sc["width"] and p["back_buffer_height"] == self.sc["height"],
                   f"device: back buffer {p['back_buffer_width']}x{p['back_buffer_height']}")
        self.check(p["back_buffer_format"] == D3DFMT[dev["backbuffer_format"]],
                   f"device: back buffer format {p['back_buffer_format']}")
        self.check(p["auto_depth_stencil"] and p["auto_depth_stencil_format"] == D3DFMT[dev["depth_stencil_format"]],
                   f"device: auto depth-stencil {p['auto_depth_stencil']} {p['auto_depth_stencil_format']}")
        self.check(p["windowed"], "device: not windowed")
        vp = CREATE_SOFTWARE_VP if dev["vertex_processing"] == "software" else CREATE_HARDWARE_VP
        self.check(d["behavior_flags"] & vp, f"device: behavior_flags 0x{d['behavior_flags']:x} lacks 0x{vp:x}")
        if self.expect_import:
            self.check(d["vulkan"]["imported"], "device: DXVK did not import the FUSE-created VkDevice")
        return d

    def map_resources(self, dev):
        implicit = {dev["back_buffer"], dev["auto_depth_stencil"]} if dev else set()
        self.bb = dev["back_buffer"] if dev else None
        self.auto_ds = dev["auto_depth_stencil"] if dev else None
        tap_tex = [e for e in self.ev if e["ev"] == "texture_create" and e["id"] not in implicit]
        tap_buf = [e for e in self.ev if e["ev"] == "buffer_create"]
        sc_tex = sorted(self.sc["textures"], key=lambda t: t["id"])
        sc_buf = sorted(self.sc["buffers"], key=lambda b: b["id"])
        self.tex = {}
        self.buf = {}
        if self.check(len(tap_tex) >= len(sc_tex), f"textures: tap created {len(tap_tex)}, sidecar has {len(sc_tex)}"):
            for s, t in zip(sc_tex, tap_tex):
                self.tex[s["id"]] = t["id"]
                w = f"texture {s['id']} (tap {t['id']})"
                self.check(t["width"] == s["width"] and t["height"] == s["height"],
                           f"{w}: {t['width']}x{t['height']} != {s['width']}x{s['height']}")
                self.check(t["format"] == s["format_value"], f"{w}: format {t['format']} != {s['format_value']}")
                self.check(t["pool"] == s["pool"], f"{w}: pool {t['pool']} != {s['pool']}")
                self.check(t["usage"] == s["usage"], f"{w}: usage 0x{t['usage']:x} != 0x{s['usage']:x}")
                if s["kind"] == "texture" and s["levels"] > 0:
                    self.check(t["levels"] == s["levels"], f"{w}: levels {t['levels']} != {s['levels']}")
        if self.check(len(tap_buf) >= len(sc_buf), f"buffers: tap created {len(tap_buf)}, sidecar has {len(sc_buf)}"):
            for s, t in zip(sc_buf, tap_buf):
                self.buf[s["id"]] = t["id"]
                w = f"buffer {s['id']} (tap {t['id']})"
                self.check(t["kind"] == s["kind"], f"{w}: kind {t['kind']} != {s['kind']}")
                self.check(t["size"] == s["size"], f"{w}: size {t['size']} != {s['size']}")
                self.check(t["usage"] == s["usage"], f"{w}: usage 0x{t['usage']:x} != 0x{s['usage']:x}")
                self.check(t["pool"] == s["pool"], f"{w}: pool {t['pool']} != {s['pool']}")
                if s["kind"] == "vertex":
                    self.check(t.get("fvf") == s.get("fvf", 0), f"{w}: fvf {t.get('fvf')} != {s.get('fvf')}")
                else:
                    self.check(t.get("format") == s.get("format"), f"{w}: format {t.get('format')} != {s.get('format')}")

    def buffer_versions(self):
        writes = {}
        for e in self.ev:
            if e["ev"] == "buffer_write":
                writes.setdefault(e["buffer"], []).append(e)
        for s in self.sc["buffers"]:
            tid = self.buf.get(s["id"])
            if tid is None:
                continue
            got = writes.get(tid, [])
            w = f"buffer {s['id']}"
            if not self.check(len(got) == len(s["versions"]),
                              f"{w}: {len(got)} tap writes != {len(s['versions'])} sidecar versions"):
                continue
            for i, (v, g) in enumerate(zip(s["versions"], got)):
                for k in ("offset", "size", "lock_flags"):
                    self.check(g[k] == v[k], f"{w} version {i}: {k} {g[k]} != {v[k]}")
                self.check(g["blob"] == v["blob"], f"{w} version {i}: content hash differs")

    def texture_uploads(self):
        per_tex = {}
        for e in self.ev:
            if e["ev"] == "texture_upload":
                per_tex.setdefault(e["texture"], []).append(e)
            elif e["ev"] == "texture_copy":
                per_tex.setdefault(e["destination"], []).append(e)
        inv_tex = {v: k for k, v in self.tex.items()}
        for s in self.sc["textures"]:
            tid = self.tex.get(s["id"])
            if tid is None:
                continue
            w = f"texture {s['id']}"
            got = list(per_tex.get(tid, []))
            ups = list(s["uploads"])
            i = 0
            while i < len(ups):
                u = ups[i]
                if not got:
                    self.err(f"{w}: sidecar upload {i} ({u['method']} level {u['level']}) has no tap event")
                    break
                g = got.pop(0)
                if u["method"] == "LockRect":
                    if self.check(g["ev"] == "texture_upload", f"{w}: upload {i}: tap {g['ev']} for LockRect"):
                        self.check(g["level"] == u["level"], f"{w}: upload {i}: level {g['level']} != {u['level']}")
                        self.check(g["full"], f"{w}: upload {i}: partial lock")
                        self.check(g["blob"] == u["blob"],
                                   f"{w}: upload {i} (level {u['level']}): locked bytes hash differs "
                                   f"(row_pitch {g['row_pitch']} x rows {g['rows']}; sidecar {u.get('row_bytes')} x {u.get('rows')})")
                    i += 1
                elif u["method"] == "UpdateTexture":
                    n = 1
                    while i + n < len(ups) and ups[i + n]["method"] == "UpdateTexture" and \
                            ups[i + n]["source"] == u["source"] and ups[i + n]["level"] > ups[i + n - 1]["level"]:
                        n += 1
                    if self.check(g["ev"] == "texture_copy" and g["method"] == "UpdateTexture",
                                  f"{w}: upload {i}: tap {g['ev']} for UpdateTexture"):
                        self.check(inv_tex.get(g["source"]) == u["source"],
                                   f"{w}: UpdateTexture source tap {g['source']} != sidecar {u['source']}")
                    i += n
                else:  # UpdateSurface / CopyRects
                    if self.check(g["ev"] == "texture_copy" and g["method"] == "UpdateSurface",
                                  f"{w}: upload {i}: tap {g['ev']} for {u['method']}"):
                        self.check(inv_tex.get(g["source"]) == u["source"],
                                   f"{w}: {u['method']} source tap {g['source']} != sidecar {u['source']}")
                        self.check(g["dest_level"] == u["level"] and g["source_level"] == u.get("source_level", 0),
                                   f"{w}: {u['method']} levels {g['source_level']}->{g['dest_level']}")
                    i += 1
            if got:
                self.err(f"{w}: {len(got)} tap upload/copy event(s) beyond the sidecar's")

    # ---- frames ----------------------------------------------------------------------------------
    def rt_ok(self, sc_rt, tap_rt, what):
        want = self.bb if sc_rt == "backbuffer" else self.tex.get(sc_rt)
        return self.check(tap_rt == want, f"{what}: render target tap {tap_rt} != {sc_rt} (tap {want})")

    def frames(self):
        presents = [e for e in self.ev if e["ev"] == "present"]
        self.check(len(presents) == self.sc["frames"], f"{len(presents)} presents != {self.sc['frames']} frames")
        self.blocks = {e["index"]: e["state"] for e in self.ev if e["ev"] == "state_block"}
        for f in self.sc["recorded_frames"]:
            want = sorted([("clear", c) for c in self.sc["clears"] if c["frame"] == f] +
                          [("draw", d) for d in self.sc["draws"] if d["frame"] == f], key=lambda x: x[1]["seq"])
            got = [e for e in self.ev if e["ev"] in ("clear", "draw") and e["frame"] == f]
            if not self.check(len(got) == len(want),
                              f"frame {f}: {len(got)} tap clears+draws != {len(want)} in the sidecar "
                              f"(tap: {[g['ev'] for g in got]}, sidecar: {[w[0] for w in want]})"):
                continue
            for (kind, s), g in zip(want, got):
                where = f"frame {f} seq {s['seq']} ({kind})"
                if not self.check(g["ev"] == kind, f"{where}: tap has {g['ev']}"):
                    continue
                if kind == "clear":
                    self.clear(s, g, where)
                else:
                    self.draw(s, g, where)

    def clear(self, s, g, where):
        self.check(g["flags"] == s["flags"], f"{where}: flags {g['flags']} != {s['flags']}")
        self.check(g["color"] == s["color"], f"{where}: color {g['color']} != {s['color']}")
        self.check(feq(g["z"], s["z"]), f"{where}: z {g['z']} != {s['z']}")
        self.check(g["stencil"] == s["stencil"], f"{where}: stencil {g['stencil']} != {s['stencil']}")
        self.rt_ok(s["render_target"], g["render_targets"][0], where)

    def version_blob(self, sc_buffer_id, version):
        for b in self.sc["buffers"]:
            if b["id"] == sc_buffer_id:
                return b["versions"][version]["blob"] if 0 <= version < len(b["versions"]) else None
        return None

    def draw(self, s, g, w):
        for k in ("call", "primitive", "prim_count"):
            self.check(g[k] == s[k], f"{w}: {k} {g[k]} != {s[k]}")
        for k in ("start_vertex", "vertex_count", "base_vertex", "min_index", "num_vertices", "start_index",
                  "index_count"):
            if k in s:
                self.check(g[k] == s[k], f"{w}: {k} {g[k]} != {s[k]}")
        ib = s["index_buffer"]
        if ib is None:
            self.check(g["index_buffer"] is None, f"{w}: tap reports an index buffer")
        elif self.check(g["index_buffer"] is not None, f"{w}: tap reports no index buffer"):
            gi = g["index_buffer"]
            self.check(gi["buffer"] == self.buf.get(ib["buffer"]), f"{w}: index buffer {gi['buffer']} != {ib['buffer']}")
            self.check(gi["format"] == ib["format"], f"{w}: index format {gi['format']} != {ib['format']}")
            self.check(gi["blob"] == self.version_blob(ib["buffer"], ib["version"]), f"{w}: index data hash differs")
        if "up" in s:
            if self.check("up" in g, f"{w}: tap has no UP data"):
                for k in ("vertex_stride", "vertex_blob", "index_blob", "index_format"):
                    self.check(g["up"][k] == s["up"][k], f"{w}: up.{k} {g['up'][k]} != {s['up'][k]}")
        vf = s["vertex_format"]
        # D3D8 programmable draws: d3d8 turns the D3D8 declaration's input registers (v0, v1, ...)
        # into D3D9 usages by register number, so only the layout is comparable.
        layout_only = self.d3d8 and self.state_of(s)["vertex_shader"] is not None

        def key(e):
            k = (e["stream"], e["offset"], e["type"], e["size"], e["method"])
            return k if layout_only else k + (e["usage"], e["usage_index"])
        want_e = [key(e) for e in vf["elements"]]
        got_e = [key(e) for e in g["elements"]]
        self.check(got_e == want_e, f"{w}: vertex elements {got_e} != {want_e}")
        if vf["fvf"] is not None and not self.d3d8:
            self.check(g["fvf"] == vf["fvf"], f"{w}: fvf {g['fvf']} != {vf['fvf']}")
        want_s = {st["stream"]: st for st in s["streams"]}
        got_s = {st["stream"]: st for st in g["streams"]}
        self.check(sorted(got_s) == sorted(want_s), f"{w}: bound streams {sorted(got_s)} != {sorted(want_s)}")
        for n, st in want_s.items():
            gs = got_s.get(n)
            if gs is None:
                continue
            self.check(gs["buffer"] == self.buf.get(st["buffer"]), f"{w}: stream {n} buffer {gs['buffer']} != {st['buffer']}")
            self.check(gs["offset"] == st["offset"] and gs["stride"] == st["stride"],
                       f"{w}: stream {n} offset/stride {gs['offset']}/{gs['stride']} != {st['offset']}/{st['stride']}")
            self.check(gs["blob"] == self.version_blob(st["buffer"], st["version"]), f"{w}: stream {n} data hash differs")
        for name, m in s["transforms"].items():
            self.check(feq(g["transforms"].get(name, IDENTITY), m), f"{w}: transform {name} differs")
        state = self.sc["state_blocks"][s["state"]]
        self.state(state, self.blocks.get(g["state"]), g, w)

    def state_of(self, draw):
        return self.sc["state_blocks"][draw["state"]]

    def shader(self, sc_id, tap_ref, what, w):
        if sc_id is None:
            self.check(tap_ref is None, f"{w}: tap reports a {what} shader")
            return
        if not self.check(tap_ref is not None, f"{w}: tap reports no {what} shader"):
            return
        if self.d3d8:
            return  # d3d8 hands d3d9 translated bytecode
        sc = next((x for x in self.sc["shaders"] if x["id"] == sc_id), None)
        if self.check(sc is not None, f"{w}: sidecar {what} shader {sc_id} missing"):
            self.check(tap_ref["blob"] == sc["blob"], f"{w}: {what} shader bytecode hash differs")
            self.check(tap_ref["version"] == sc["version"], f"{w}: {what} shader version {tap_ref['version']} != {sc['version']}")

    def consts(self, sc_list, tap_list, what, w, as_float=True):
        want = {c["register"]: c["value"] for c in sc_list}
        got = {c["register"]: c["value"] for c in tap_list}
        zero = False if not as_float and isinstance(next(iter(want.values()), 0), bool) else None
        for r in sorted(set(want) | set(got)):
            a, b = want.get(r), got.get(r)
            if as_float:
                a = a if a is not None else [0, 0, 0, 0]
                b = b if b is not None else [0, 0, 0, 0]
                self.check(feq(a, b), f"{w}: {what}[{r}] tap {b} != {a}")
            else:
                if zero is False:  # booleans
                    self.check(bool(a) == bool(b), f"{w}: {what}[{r}] tap {b} != {a}")
                else:
                    self.check((a or [0, 0, 0, 0]) == (b or [0, 0, 0, 0]), f"{w}: {what}[{r}] tap {b} != {a}")

    def state(self, s, t, g, w):
        if not self.check(t is not None, f"{w}: tap state block {g['state']} missing"):
            return
        for k, v in s["render_states"].items():
            self.check(t["render_states"].get(k) == v, f"{w}: render state {k} tap {t['render_states'].get(k)} != {v}")
        tstages = {x["stage"]: x["states"] for x in t["texture_stages"]}
        for st in s["texture_stages"]:
            for k, v in st["states"].items():
                got = tstages.get(st["stage"], {}).get(k)
                self.check(got == v, f"{w}: stage {st['stage']} {k} tap {got} != {v}")
        tsamp = {x["sampler"]: x["states"] for x in t["samplers"]}
        for sp in s["samplers"]:
            for k, v in sp["states"].items():
                got = tsamp.get(sp["sampler"], {}).get(k)
                self.check(got == v, f"{w}: sampler {sp['sampler']} {k} tap {got} != {v}")
        want_tex = {x["stage"]: self.tex.get(x["texture"]) for x in s["textures"]}
        got_tex = {x["stage"]: x["texture"] for x in t["textures"]}
        self.check(got_tex == want_tex, f"{w}: bound textures tap {got_tex} != {want_tex}")
        want_l = {l["index"]: l for l in s["lights"]}
        got_l = {l["index"]: l for l in t["lights"]}
        self.check(sorted(got_l) == sorted(want_l), f"{w}: lights tap {sorted(got_l)} != {sorted(want_l)}")
        for i, l in want_l.items():
            gl = got_l.get(i)
            if gl is None:
                continue
            for k in ("enabled", "type"):
                self.check(gl[k] == l[k], f"{w}: light {i} {k} {gl[k]} != {l[k]}")
            for k in ("diffuse", "specular", "ambient", "position", "direction", "range", "falloff", "attenuation",
                      "theta", "phi"):
                self.check(feq(gl[k], l[k]), f"{w}: light {i} {k} {gl[k]} != {l[k]}")
        if s["material"] is not None:
            for k, v in s["material"].items():
                self.check(feq(t["material"][k], v), f"{w}: material {k} {t['material'][k]} != {v}")
        vp = dict(s["viewport"])
        if f32(vp["min_z"]) >= f32(vp["max_z"]):
            # DXVK stores MaxZ = MinZ + 0.001 when the application sets MinZ >= MaxZ
            # (D3D9DeviceEx::SetViewport); the tap reports the device state.
            vp["max_z"] = f32(f32(vp["min_z"]) + f32(0.001))
        for k, v in vp.items():
            self.check(feq(t["viewport"][k], v), f"{w}: viewport {k} {t['viewport'][k]} != {v}")
        self.rt_ok(s["render_target"], t["render_targets"][0], w)
        want_ds = self.auto_ds if s["depth_stencil"] == "default" else self.tex.get(s["depth_stencil"])
        self.check(t["depth_stencil"] == want_ds, f"{w}: depth-stencil tap {t['depth_stencil']} != {s['depth_stencil']}")
        self.shader(s["vertex_shader"], g["vertex_shader"], "vertex", w)
        self.shader(s["pixel_shader"], g["pixel_shader"], "pixel", w)
        self.consts(s["vs_const_f"], t["vs_const_f"], "vs_const_f", w)
        self.consts(s["ps_const_f"], t["ps_const_f"], "ps_const_f", w)
        self.consts(s["vs_const_i"], t["vs_const_i"], "vs_const_i", w, as_float=False)
        self.consts(s["vs_const_b"], t["vs_const_b"], "vs_const_b", w, as_float=False)

    def run(self):
        if not self.ev or self.ev[0].get("schema") != "fuse.relight.tap_events/1":
            self.err("stream: missing fuse.relight.tap_events/1 header")
            return
        dev = self.device()
        self.map_resources(dev)
        self.buffer_versions()
        self.texture_uploads()
        self.frames()


def compare(sidecar, events, expect_import=False):
    c = Compare(sidecar, events, expect_import)
    c.run()
    draws = sum(1 for e in events if e.get("ev") == "draw")
    summary = (f"{len(sidecar['draws'])} sidecar draws / {draws} tap draws, {len(sidecar['textures'])} textures, "
               f"{len(sidecar['buffers'])} buffers, {len(sidecar['recorded_frames'])} recorded frame(s) match")
    return c.errors, summary


def compare_files(sidecar_path, stream_path, expect_import=False):
    with open(sidecar_path) as f:
        sidecar = json.load(f)
    return compare(sidecar, load_stream(stream_path), expect_import)


# ---- self-test ---------------------------------------------------------------------------------------
def _synthetic():
    blob_v = "a" * 64
    blob_i = "b" * 64
    blob_t = "c" * 64
    state = {"render_states": {"ZENABLE": 1, "LIGHTING": 0}, "render_states_float": {},
             "texture_stages": [{"stage": 0, "states": {"COLOROP": 4}}],
             "samplers": [{"sampler": 0, "states": {"MINFILTER": 2}}],
             "textures": [{"stage": 0, "texture": 2}], "lights": [], "material": None,
             "viewport": {"x": 0, "y": 0, "width": 128, "height": 96, "min_z": 0.0, "max_z": 1.0},
             "render_target": "backbuffer", "depth_stencil": "default", "vertex_shader": None, "pixel_shader": None,
             "vs_const_f": [], "vs_const_i": [], "vs_const_b": [], "ps_const_f": []}
    elem = {"stream": 0, "offset": 0, "type": "FLOAT3", "size": 12, "method": 0, "usage": "POSITION", "usage_index": 0}
    sidecar = {
        "schema": "fuse.relight.app_sidecar/1", "app": "synthetic", "api": "d3d9", "width": 128, "height": 96,
        "frames": 2, "recorded_frames": [1],
        "device": {"backbuffer_format": "X8R8G8B8", "depth_stencil_format": "D24S8", "vertex_processing": "hardware"},
        "textures": [{"id": 2, "kind": "texture", "width": 4, "height": 4, "levels": 1, "format": "A8R8G8B8",
                      "format_value": 21, "usage": 0, "pool": "MANAGED",
                      "uploads": [{"frame": -1, "level": 0, "method": "LockRect", "blob": blob_t}]}],
        "buffers": [{"id": 0, "kind": "vertex", "size": 36, "usage": 8, "pool": "MANAGED", "fvf": 2,
                     "versions": [{"frame": -1, "offset": 0, "size": 36, "lock_flags": 0, "blob": blob_v}]},
                    {"id": 1, "kind": "index", "size": 6, "usage": 8, "pool": "MANAGED", "format": "INDEX16",
                     "versions": [{"frame": -1, "offset": 0, "size": 6, "lock_flags": 0, "blob": blob_i}]}],
        "declarations": [], "shaders": [], "state_blocks": [state],
        "clears": [{"frame": 1, "seq": 0, "flags": 3, "color": "0xff000000", "z": 1.0, "stencil": 0,
                    "render_target": "backbuffer"}],
        "draws": [{"frame": 1, "seq": 1, "call": "DrawIndexedPrimitive", "primitive": "TRIANGLELIST", "prim_count": 1,
                   "tag": "world", "base_vertex": 0, "min_index": 0, "num_vertices": 3, "start_index": 0,
                   "index_count": 3, "index_buffer": {"buffer": 1, "version": 0, "format": "INDEX16"},
                   "vertex_format": {"fvf": 2, "declaration": None, "elements": [elem]},
                   "streams": [{"stream": 0, "buffer": 0, "version": 0, "offset": 0, "stride": 12}],
                   "transforms": {"WORLD": IDENTITY, "VIEW": [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1]},
                   "state": 0}],
    }
    tstate = {"render_states": {"ZENABLE": 1, "LIGHTING": 0, "CULLMODE": 1},
              "texture_stages": [{"stage": 0, "states": {"COLOROP": 4, "ALPHAOP": 2}}],
              "samplers": [{"sampler": 0, "states": {"MINFILTER": 2, "MAGFILTER": 2}}],
              "textures": [{"stage": 0, "texture": 3}], "lights": [],
              "material": {"diffuse": [0, 0, 0, 0], "ambient": [0, 0, 0, 0], "specular": [0, 0, 0, 0],
                           "emissive": [0, 0, 0, 0], "power": 0},
              "viewport": {"x": 0, "y": 0, "width": 128, "height": 96, "min_z": 0, "max_z": 1},
              "render_targets": [1, None, None, None], "depth_stencil": 2,
              "vs_const_f": [], "vs_const_i": [], "vs_const_b": [], "ps_const_f": [], "ps_const_i": [], "ps_const_b": []}
    telem = dict(elem)
    events = [
        {"ev": "header", "schema": "fuse.relight.tap_events/1", "interface_version": 1},
        {"ev": "texture_create", "frame": 0, "id": 1, "width": 128, "height": 96, "format": 22, "usage": 1,
         "pool": "DEFAULT", "levels": 1},
        {"ev": "texture_create", "frame": 0, "id": 2, "width": 128, "height": 96, "format": 75, "usage": 2,
         "pool": "DEFAULT", "levels": 1},
        {"ev": "device_create", "frame": 0, "behavior_flags": 0x42,
         "present": {"back_buffer_width": 128, "back_buffer_height": 96, "back_buffer_format": 22, "windowed": True,
                     "auto_depth_stencil": True, "auto_depth_stencil_format": 75},
         "back_buffer": 1, "auto_depth_stencil": 2, "vulkan": {"imported": True}},
        {"ev": "buffer_create", "frame": 0, "id": 1, "kind": "vertex", "size": 36, "usage": 8, "pool": "MANAGED", "fvf": 2},
        {"ev": "buffer_write", "frame": 0, "buffer": 1, "offset": 0, "size": 36, "lock_flags": 0, "blob": blob_v},
        {"ev": "buffer_create", "frame": 0, "id": 2, "kind": "index", "size": 6, "usage": 8, "pool": "MANAGED",
         "format": "INDEX16"},
        {"ev": "buffer_write", "frame": 0, "buffer": 2, "offset": 0, "size": 6, "lock_flags": 0, "blob": blob_i},
        {"ev": "texture_create", "frame": 0, "id": 3, "width": 4, "height": 4, "format": 21, "usage": 0,
         "pool": "MANAGED", "levels": 1},
        {"ev": "texture_upload", "frame": 0, "texture": 3, "level": 0, "full": True, "row_pitch": 16, "rows": 4,
         "blob": blob_t},
        {"ev": "present", "frame": 0},
        {"ev": "clear", "frame": 1, "flags": 3, "color": "0xff000000", "z": 1, "stencil": 0,
         "render_targets": [1, None, None, None]},
        {"ev": "state_block", "index": 0, "state": tstate},
        {"ev": "draw", "frame": 1, "call": "DrawIndexedPrimitive", "primitive": "TRIANGLELIST", "prim_count": 1,
         "start_vertex": 0, "vertex_count": 0, "base_vertex": 0, "min_index": 0, "num_vertices": 3, "start_index": 0,
         "index_count": 3, "index_buffer": {"buffer": 2, "format": "INDEX16", "blob": blob_i}, "elements": [telem],
         "fvf": 2, "streams": [{"stream": 0, "buffer": 1, "offset": 0, "stride": 12, "blob": blob_v}],
         "transforms": {"VIEW": [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1]},
         "vertex_shader": None, "pixel_shader": None, "state": 0},
        {"ev": "present", "frame": 1},
    ]
    return sidecar, events


def selftest():
    import copy
    sidecar, events = _synthetic()
    errors, _ = compare(sidecar, events, expect_import=True)
    if errors:
        print("FAIL: synthetic pair does not match:\n  " + "\n  ".join(errors))
        return 1
    mutations = {
        "index data": lambda e: e[13]["index_buffer"].update(blob="0" * 64),
        "render state": lambda e: e[12]["state"]["render_states"].update(ZENABLE=0),
        "transform": lambda e: e[13]["transforms"].update(VIEW=IDENTITY),
        "stray world": lambda e: e[13]["transforms"].update(WORLD=[3] + IDENTITY[1:]),
        "texture binding": lambda e: e[12]["state"]["textures"].clear(),
        "buffer write": lambda e: e[5].update(lock_flags=0x2000),
        "texture upload": lambda e: e[9].update(blob="1" * 64),
        "clear color": lambda e: e[11].update(color="0xff000001"),
        "missing draw": lambda e: e.pop(13),
        "present count": lambda e: e.pop(10),
        "not imported": lambda e: e[3]["vulkan"].update(imported=False),
        "vs constant": lambda e: e[12]["state"]["vs_const_f"].append({"register": 5, "value": [1, 0, 0, 0]}),
        "stream stride": lambda e: e[13]["streams"][0].update(stride=16),
    }
    failed = 0
    for name, mutate in mutations.items():
        ev = copy.deepcopy(events)
        mutate(ev)
        errs, _ = compare(sidecar, ev, expect_import=True)
        if not errs:
            print(f"FAIL: seeded difference '{name}' not detected")
            failed += 1
    if failed:
        return 1
    print(f"PASS: rl_tap_expect selftest: synthetic pair matches, {len(mutations)} seeded differences detected")
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "selftest":
        return selftest()
    if len(argv) >= 3 and argv[1] == "compare":
        errors, summary = compare_files(argv[2], argv[3], expect_import="--expect-import" in argv)
        for e in errors:
            print(e)
        print(("FAIL" if errors else "PASS") + ": " + summary)
        return 1 if errors else 0
    print("usage: rl_tap_expect.py selftest | compare <sidecar.json> <relight_tap.jsonl> [--expect-import]")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
