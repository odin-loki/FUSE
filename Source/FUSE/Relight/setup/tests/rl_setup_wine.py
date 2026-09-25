#!/usr/bin/env python3
"""FUSE Relight RL-6.3: setup assistant and per-game profiles on the RL-0.4 test apps under Wine.

  1. baseline   the app through our d3d9.dll / d3d8.dll in capture mode (relight.tap.mode = capture), no profile.
  2. assistant  fuse_relight_setup analyze on that capture record: the proposed profile must contain, by the app's
                sidecar tags (recorded frame), the applied and pending categories / options of
                expectations/<scene>.json; with --content, the sky_ui_hud profile must equal the shipped one.
  3. runs       per expectations "runs": the profile (optionally with accepted suggestions, matched by exe name
                or only by the exe's XXH3 hash, or written for another exe) is put in <exe dir>/relight/profiles,
                where d3d9.dll's discovery finds it, and the app runs again. The capture record's classification
                per tag must match the run's expectations, or equal the baseline ("same_as_baseline"), and the
                run log must say whether a profile was loaded.

  rl_setup_wine.py --exe app.exe --app NAME --d3d9 d3d9.dll --d3d8 d3d8.dll --runner run.sh --prefix-root DIR
                   --tool fuse_relight_setup.exe [--emulator 'wine-run.sh|prefix'] --expect DIR [--content DIR] --out DIR

Exit codes: 0 pass, 1 fail, 77 skip (no Wine / Xvfb).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

SKIP = 77


def read_jsonl(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def run_app(args, run_dir, profile=None, profile_name=None):
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    for src in (args.exe, args.d3d9, args.d3d8):
        dst = os.path.join(run_dir, os.path.basename(src))
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)
    if profile is not None:
        pdir = os.path.join(run_dir, "relight", "profiles")
        os.makedirs(pdir)
        shutil.copy2(profile, os.path.join(pdir, profile_name or os.path.basename(profile)))
    env = dict(os.environ)
    env.setdefault("DXVK_LOG_LEVEL", "warn")
    env["DXVK_LOG_PATH"] = "none"
    for k in ("FUSE_RELIGHT_PROFILE", "FUSE_RELIGHT_PROFILE_PATH", "DXVK_RTX_CONFIG_FILE", "DXVK_CONFIG_FILE"):
        env.pop(k, None)
    env.update({"FUSE_RELIGHT": "1", "FUSE_RELIGHT_TAP_MODE": "capture",
                "FUSE_RELIGHT_TAP_CAPTURE_PATH": "relight_capture.jsonl"})
    local_exe = os.path.join(run_dir, os.path.basename(args.exe))
    cmd = ["bash", args.runner, "--native-d3d", args.prefix_root, local_exe, "--out", ".", "--quiet"]
    proc = subprocess.run(cmd, cwd=run_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode(errors="replace")
    with open(os.path.join(run_dir, "run.log"), "w") as f:
        f.write(text)
    return proc.returncode, text


def tool(args, argv, cwd):
    emu = [p for p in args.emulator.split("|") if p] if args.emulator else []
    proc = subprocess.run(emu + [args.tool] + argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.returncode, proc.stdout.decode(errors="replace"), proc.stderr.decode(errors="replace")


def tagged_draws(run_dir, app):
    """[(tag, capture draw record)] of the sidecar's recorded frames, in draw order."""
    with open(os.path.join(run_dir, app + ".json")) as f:
        sidecar = json.load(f)
    capture = [r for r in read_jsonl(os.path.join(run_dir, "relight_capture.jsonl")) if r["ev"] == "draw"]
    out, errors = [], []
    for frame in sidecar["recorded_frames"]:
        side = [d for d in sidecar["draws"] if d["frame"] == frame]
        got = sorted((r for r in capture if r["frame"] == frame), key=lambda r: r["di"])
        if len(side) != len(got):
            errors.append("frame %d: sidecar has %d draw(s), the capture %d" % (frame, len(side), len(got)))
            continue
        out += [(d.get("tag", ""), r) for d, r in zip(side, got)]
    return out, errors


def tag_hashes(draws, tag, descriptor=False):
    """Colour-texture hashes (or render-target descriptor hashes) of a tag's draws."""
    out = set()
    for t, r in draws:
        if t != tag:
            continue
        color = int(r["classification"].get("color_texture", "0x0"), 16)
        if color == 0:
            continue
        if not descriptor:
            out.add(color)
            continue
        for tex in r.get("textures", []):
            if int(tex["hash"], 16) == color and int(tex.get("desc", "0"), 16):
                out.add(int(tex["desc"], 16))
    return out


def check_proposals(profile, draws, exp):
    errors = []
    textures = {k: {int(h, 16) for h in v if not h.startswith("-")} for k, v in profile["textures"].items()}
    pending = {}
    for s in profile["suggestions"]:
        if s["kind"] == "texture" and not s["applied"]:
            pending.setdefault(s["category"], set()).add(int(s["hash"], 16))
    applied_opts = profile["options"]
    pending_opts = {s["key"]: s["value"] for s in profile["suggestions"] if s["kind"] == "option" and not s["applied"]}

    def want_hashes(category, tags):
        want = set()
        for tag in tags:
            hs = tag_hashes(draws, tag, descriptor=(category == "raytracedRenderTarget"))
            if not hs:
                errors.append("tag %r has no textured draw for category %s" % (tag, category))
            want |= hs
        return want

    for section, have, have_opts in (("applied", textures, applied_opts), ("pending", pending, pending_opts)):
        spec = exp.get(section, {})
        for category, tags in spec.get("textures", {}).items():
            want = want_hashes(category, tags)
            missing = want - have.get(category, set())
            if missing:
                errors.append("%s %s: missing %s (tags %s)" % (section, category,
                                                             ", ".join("0x%016X" % h for h in sorted(missing)), tags))
        for key, value in spec.get("options", {}).items():
            if have_opts.get(key) != value:
                errors.append("%s option %s: %r, expected %r" % (section, key, have_opts.get(key), value))
        # exact: nothing else applied
        if section == "applied" and spec.get("exact", False):
            want_all = {c: want_hashes(c, t) for c, t in spec.get("textures", {}).items()}
            for category, hs in have.items():
                extra = hs - want_all.get(category, set())
                if extra:
                    errors.append("applied %s: unexpected %s" % (category, ", ".join("0x%016X" % h for h in sorted(extra))))
            extra_opts = set(have_opts) - set(spec.get("options", {}))
            if extra_opts:
                errors.append("applied options: unexpected %s" % sorted(extra_opts))
    for tag in exp.get("never_applied_tags", []):
        for category, hs in textures.items():
            both = hs & tag_hashes(draws, tag)
            if both:
                errors.append("tag %r texture(s) %s applied as %s" % (tag, sorted("0x%016X" % h for h in both), category))
    return errors


def check_run(draws, baseline, run):
    errors = []
    if run.get("same_as_baseline"):
        for (tag, r), (_, b) in zip(draws, baseline):
            if r["classification"] != b["classification"]:
                errors.append("%s draw %d: classification %s differs from the baseline %s"
                              % (tag, r["di"], r["classification"], b["classification"]))
        return errors
    seen = {}
    for tag, r in draws:
        exp = run.get("tags", {}).get(tag)
        if exp is None:
            continue
        c = r["classification"]
        n = seen.get(tag, 0)
        seen[tag] = n + 1
        if isinstance(exp, list):
            exp = exp[min(n, len(exp) - 1)]
        for key, want in exp.items():
            if key == "categories_include":
                if want not in c.get("categories", "").split("|"):
                    errors.append("%s #%d: categories %r lack %s" % (tag, n, c.get("categories"), want))
            elif c.get(key) != want:
                errors.append("%s #%d: %s = %r, expected %r" % (tag, n, key, c.get(key), want))
    for tag in run.get("tags", {}):
        if tag not in seen:
            errors.append("expected tag %r has no draw" % tag)
    return errors


def main():
    ap = argparse.ArgumentParser()
    for a in ("--exe", "--app", "--d3d9", "--d3d8", "--runner", "--prefix-root", "--tool", "--expect", "--out"):
        ap.add_argument(a, required=True)
    ap.add_argument("--emulator", default="")
    ap.add_argument("--content", default="")
    args = ap.parse_args()
    app = args.app
    exe_name = os.path.basename(args.exe)
    os.makedirs(args.out, exist_ok=True)

    base_dir = os.path.join(args.out, "baseline")
    rc, text = run_app(args, base_dir)
    if rc == SKIP:
        print(text.strip())
        return SKIP
    if rc != 0 or not os.path.isfile(os.path.join(base_dir, "relight_capture.jsonl")):
        print(text.strip()[-4000:])
        print("FAIL: %s: baseline run exited with %d" % (app, rc))
        return 1
    if "fuse-relight: profile '" in text:
        print("FAIL: %s: the baseline run loaded a profile" % app)
        return 1
    with open(os.path.join(base_dir, app + ".json")) as f:
        scene = json.load(f).get("scene", app)
    with open(os.path.join(args.expect, scene + ".json")) as f:
        expect = json.load(f)
    baseline, errors = tagged_draws(base_dir, app)
    if errors:
        print("\n".join("FAIL: %s: %s" % (app, e) for e in errors))
        return 1

    # ---- assistant
    work = os.path.join(args.out, "assistant")
    os.makedirs(work, exist_ok=True)
    shutil.copy2(os.path.join(base_dir, "relight_capture.jsonl"), os.path.join(work, "capture.jsonl"))
    shutil.copy2(args.exe, os.path.join(work, exe_name))
    rc, out, err = tool(args, ["analyze", "capture.jsonl", "--exe", exe_name, "--out", "profile.json"], work)
    with open(os.path.join(work, "analyze.log"), "w") as f:
        f.write(out + err)
    if rc != 0:
        print(err[-3000:])
        print("FAIL: %s: fuse_relight_setup analyze exited with %d" % (app, rc))
        return 1
    rc, exe_hash, err = tool(args, ["hash-exe", exe_name], work)
    exe_hash = exe_hash.strip()
    if rc != 0 or not exe_hash.startswith("0x"):
        print("FAIL: %s: hash-exe: %s" % (app, err))
        return 1
    with open(os.path.join(work, "profile.json")) as f:
        profile_text = f.read()
    profile = json.loads(profile_text)
    failed = False
    for e in check_proposals(profile, baseline, expect.get("proposals", {})):
        print("FAIL: %s: assistant: %s" % (app, e))
        failed = True
    rc, out, err = tool(args, ["check", "profile.json"], work)
    if rc != 0:
        print("FAIL: %s: the generated profile is not canonical: %s" % (app, err))
        failed = True
    golden = os.path.join(args.content, app + ".json") if args.content else ""
    if golden and os.path.isfile(golden):
        with open(golden) as f:
            if f.read() != profile_text:
                print("FAIL: %s: the generated profile differs from %s (regenerate it with fuse_relight_setup analyze)"
                      % (app, golden))
                failed = True
    # rtx.conf export / import round trip of the generated profile
    rc1, conf, _ = tool(args, ["export-conf", "profile.json", "--out", "rtx_export.conf"], work)
    rc2, _, _ = tool(args, ["import-conf", "rtx_export.conf", "--exe", exe_name, "--out", "reimported.json"], work)
    rc3, _, _ = tool(args, ["export-conf", "reimported.json", "--out", "rtx_reexport.conf"], work)
    if rc1 or rc2 or rc3:
        print("FAIL: %s: export-conf / import-conf failed" % app)
        failed = True
    else:
        with open(os.path.join(work, "rtx_export.conf")) as a, open(os.path.join(work, "rtx_reexport.conf")) as b:
            if a.read() != b.read():
                print("FAIL: %s: rtx.conf export -> import -> export is not byte-stable" % app)
                failed = True

    # ---- runs with profiles
    for run in expect.get("runs", []):
        name = run["name"]
        pfile = os.path.join(work, "profile_%s.json" % name)
        shutil.copy2(os.path.join(work, "profile.json"), pfile)
        for selector in run.get("accept", []):
            rc, _, err = tool(args, ["accept", os.path.basename(pfile), selector], work)
            if rc != 0:
                print("FAIL: %s: %s: accept %s: %s" % (app, name, selector, err))
                failed = True
        with open(pfile) as f:
            p = json.load(f)
        match = run.get("match", "name")
        if match == "hash":
            p["match"] = {"exe": ["not_%s" % exe_name], "xxh3": [exe_hash], "require_hash": True}
        elif match == "other":
            p["match"] = {"exe": ["other_game.exe"], "xxh3": [], "require_hash": False}
        with open(pfile, "w") as f:
            json.dump(p, f, indent=2)
        run_dir = os.path.join(args.out, name)
        rc, text = run_app(args, run_dir, pfile, "game_profile.json")
        if rc != 0:
            print(text.strip()[-3000:])
            print("FAIL: %s: %s: run exited with %d" % (app, name, rc))
            failed = True
            continue
        loaded = "fuse-relight: profile '" in text
        want_loaded = match != "other"
        if loaded != want_loaded:
            print("FAIL: %s: %s: profile %s" % (app, name, "loaded" if loaded else "not loaded"))
            failed = True
        if want_loaded and match == "hash" and "matched by exe hash" not in text:
            print("FAIL: %s: %s: the profile was not matched by hash" % (app, name))
            failed = True
        draws, errors = tagged_draws(run_dir, app)
        errors += check_run(draws, baseline, run)
        for e in errors:
            print("FAIL: %s: %s: %s" % (app, name, e))
            failed = True
        if not errors:
            print("ok: %s: %s" % (app, name))
    if failed:
        return 1
    print("PASS: %s: assistant proposals match expectations/%s.json; %d profile run(s) changed the capture record as "
          "expected" % (app, scene, len(expect.get("runs", []))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
