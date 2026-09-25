"""Stage `export`: glTF 2.0 (.glb), FUSE POCO store + manifest, licence-lock fragment, .fuselevel stub.

Layout (in <work>/export/, REMASTER_PLAN §2.1):
    world.glb, collision.glb                   glTF 2.0 binary, +Y up (glTF convention), metres
    poco/<kind>/<poco_id>.poco.json            POCO headers (schema fuse.poco/1, +Z up canonical)
    blobs/sha256/<ab>/<sha256>                 content-addressed payloads (mesh streams, PNG atlas)
    licences.fragment.json                     records to merge into Content/licences.lock.json
    world.fuselevel                            SceneSerialiser v1 stub (+Y up, runtime camera convention)
    worldextract.manifest.json                 every output with sha256, provenance, units, metrics
"""

from __future__ import annotations

import json
import logging
import math
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from .. import __version__
from ..common import WorldExtractError, read_json, repo_revision, sha256_bytes, sha256_file, write_json
from ..geometry import ZUP_TO_YUP
from . import poses as poses_stage

LOG = logging.getLogger("worldextract.export")

MANIFEST_SCHEMA = "fuse.worldextract.manifest/1"
POCO_SCHEMA = "fuse.poco/1"
FUSELEVEL_MAGIC = 0x45535546  # 'FUSE' (Source/FUSE/Scene/include/fuse/scene/serialiser.hpp)
FUSELEVEL_VERSION = 1


@dataclass
class ExportParams:
    name: str = "world"
    seed_image_origin: str = ""  # own | library | original ('' = from generate manifest, else own)
    reviewer: str = ""


# --------------------------------------------------------------------------------------------- #
# glTF 2.0 binary writer (no dependency; deterministic bytes)
# --------------------------------------------------------------------------------------------- #


def write_glb(path: Path, positions: np.ndarray, faces: np.ndarray, *, normals: np.ndarray | None = None,
              uvs: np.ndarray | None = None, colors: np.ndarray | None = None, png: bytes | None = None,
              name: str = "mesh", extras: dict[str, Any] | None = None) -> None:
    """Write one-mesh GLB. Inputs are +Z-up; converted to glTF +Y-up here."""
    pos = (ZUP_TO_YUP @ positions.astype(np.float64).T).T.astype("<f4")
    blobs: list[bytes] = []
    views: list[dict[str, Any]] = []
    accessors: list[dict[str, Any]] = []
    offset = 0

    def add_view(data: bytes, target: int | None) -> int:
        nonlocal offset
        pad = (-offset) % 4
        if pad:
            blobs.append(b"\0" * pad)
            offset += pad
        view: dict[str, Any] = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target:
            view["target"] = target
        views.append(view)
        blobs.append(data)
        offset += len(data)
        return len(views) - 1

    def add_accessor(arr: np.ndarray, comp: int, typ: str, target: int, normalized: bool = False,
                     minmax: bool = False) -> int:
        acc: dict[str, Any] = {"bufferView": add_view(arr.tobytes(), target), "componentType": comp,
                               "count": int(arr.shape[0]), "type": typ}
        if normalized:
            acc["normalized"] = True
        if minmax:
            acc["min"] = [float(x) for x in arr.min(0)]
            acc["max"] = [float(x) for x in arr.max(0)]
        accessors.append(acc)
        return len(accessors) - 1

    attrs = {"POSITION": add_accessor(pos, 5126, "VEC3", 34962, minmax=True)}
    if normals is not None:
        nrm = (ZUP_TO_YUP @ normals.astype(np.float64).T).T
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-12)
        attrs["NORMAL"] = add_accessor(nrm.astype("<f4"), 5126, "VEC3", 34962)
    if uvs is not None:
        attrs["TEXCOORD_0"] = add_accessor(uvs.astype("<f4"), 5126, "VEC2", 34962)
    if colors is not None:
        c8 = np.clip(np.round(colors[:, :3] * 255), 0, 255).astype(np.uint8)
        c8 = np.concatenate([c8, np.full((len(c8), 1), 255, np.uint8)], 1)
        attrs["COLOR_0"] = add_accessor(c8, 5121, "VEC4", 34962, normalized=True)
    idx = add_accessor(faces.reshape(-1).astype("<u4"), 5125, "SCALAR", 34963)
    material: dict[str, Any] = {"name": f"{name}_mat", "doubleSided": True,
                                "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
    doc: dict[str, Any] = {
        "asset": {"version": "2.0", "generator": "FUSE Tools/FUSE/WorldExtract"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [{"name": name, "primitives": [{"attributes": attrs, "indices": idx, "material": 0, "mode": 4}]}],
        "materials": [material],
    }
    if png is not None:
        img_view = add_view(png, None)
        doc["images"] = [{"bufferView": img_view, "mimeType": "image/png"}]
        doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]
        doc["textures"] = [{"sampler": 0, "source": 0}]
        material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0, "texCoord": 0}
    if extras:
        doc["asset"]["extras"] = extras
    bin_chunk = b"".join(blobs)
    bin_chunk += b"\0" * ((-len(bin_chunk)) % 4)
    doc["buffers"] = [{"byteLength": len(bin_chunk)}]
    doc["bufferViews"] = views
    doc["accessors"] = accessors
    js = json.dumps(doc, separators=(",", ":"), sort_keys=True).encode("utf-8")
    js += b" " * ((-len(js)) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_chunk)
    with open(path, "wb") as fh:
        fh.write(struct.pack("<III", 0x46546C67, 2, total))
        fh.write(struct.pack("<II", len(js), 0x4E4F534A))
        fh.write(js)
        fh.write(struct.pack("<II", len(bin_chunk), 0x004E4942))
        fh.write(bin_chunk)


# --------------------------------------------------------------------------------------------- #
# .fuselevel v1 (Source/FUSE/Scene/src/serialiser.cpp)
# --------------------------------------------------------------------------------------------- #


def write_fuselevel(path: Path, scene_name: str, camera: dict[str, float],
                    entities: list[tuple[str, tuple[float, ...]]]) -> None:
    """Header(64) + camera(40) + name + entity names + transform table (pos xyz, quat xyzw, scale xyz)."""
    header = struct.pack("<6I", FUSELEVEL_MAGIC, FUSELEVEL_VERSION, len(entities), 0, 0, 0)
    reserved = bytearray(40)
    reserved[0] = ord("C")  # camera block present
    reserved[1] = ord("T")  # transform table present
    out = bytearray(header + bytes(reserved))
    out += struct.pack("<9fI", camera["fov_deg"], camera["near"], camera["far"], camera["aspect"],
                       camera["x"], camera["y"], camera["z"], camera["yaw_deg"], camera["pitch_deg"], 1)

    def s(text: str) -> bytes:
        b = text.encode("utf-8")
        return struct.pack("<I", len(b)) + b

    out += s(scene_name)
    out += struct.pack("<I", len(entities))
    for name, _ in entities:
        out += s(name)
    out += struct.pack("<I", len(entities))
    for _, tr in entities:
        out += struct.pack("<10f", *tr)
    path.write_bytes(bytes(out))


def read_fuselevel(path: Path) -> dict[str, Any]:
    """Reader mirroring SceneSerialiser::load (v1/v2) - used by the test to validate the stub."""
    data = path.read_bytes()
    if len(data) < 64 + 40:
        raise ValueError("fuselevel truncated")
    magic, version, count = struct.unpack_from("<3I", data, 0)
    if magic != FUSELEVEL_MAGIC:
        raise ValueError("bad fuselevel magic")
    if version not in (1, 2) or data[24] != ord("C"):
        raise ValueError("unsupported fuselevel version / missing camera marker")
    cam = struct.unpack_from("<9fI", data, 64)
    cur = 64 + 40

    def rs() -> str:
        nonlocal cur
        (n,) = struct.unpack_from("<I", data, cur)
        cur += 4
        txt = data[cur:cur + n].decode("utf-8")
        cur += n
        return txt

    name = rs()
    (n_obj,) = struct.unpack_from("<I", data, cur)
    cur += 4
    if n_obj != count:
        raise ValueError("entity count mismatch")
    names = [rs() for _ in range(n_obj)]
    transforms = []
    if data[25] == ord("T"):
        (n_tr,) = struct.unpack_from("<I", data, cur)
        cur += 4
        if n_tr != n_obj:
            raise ValueError("transform count mismatch")
        for _ in range(n_tr):
            transforms.append(struct.unpack_from("<10f", data, cur))
            cur += 40
    if cur != len(data):
        raise ValueError("trailing bytes in fuselevel")
    return {"name": name, "camera": cam, "entities": names, "transforms": transforms}


# --------------------------------------------------------------------------------------------- #
# POCO store
# --------------------------------------------------------------------------------------------- #


class BlobStore:
    def __init__(self, root: Path) -> None:
        self.root = root

    def put(self, data: bytes, media: str) -> dict[str, Any]:
        digest = sha256_bytes(data)
        path = self.root / "blobs" / "sha256" / digest[:2] / digest
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            path.write_bytes(data)
        return {"sha256": digest, "size": len(data), "media": media}


def _poco(kind: str, poco_id: str, name: str, payload: dict[str, Any], prov: dict[str, Any],
          distribution: str, tags: list[str], reviewer: str) -> dict[str, Any]:
    return {
        "schema": POCO_SCHEMA,
        "kind": kind,
        "id": poco_id,
        "name": name,
        "units": {"length": "m", "up": "+Z", "handedness": "right"},
        "payload": payload,
        "provenance": prov,
        "licence_id": poco_id,
        "distribution": distribution,
        "replaces": [],
        "tags": tags,
        "review": {"state": "pending", "by": reviewer or None, "date": None},
    }


def _poco_path(root: Path, kind: str, poco_id: str) -> Path:
    return root / "poco" / kind / (poco_id + ".poco.json")


def _mat16(pos: np.ndarray) -> list[float]:
    m = np.eye(4)
    m[:3, 3] = pos
    return [round(float(x), 6) for x in m.T.reshape(-1)]  # column-major like glTF


def _generation_context(work: Path) -> dict[str, Any]:
    ctx_path = work / "inputs.json"
    return read_json(ctx_path) if ctx_path.exists() else {}


def run(work: Path, out: Path, p: ExportParams) -> dict[str, Any]:
    for sub in ("poco", "blobs"):
        if (out / sub).exists():
            shutil.rmtree(out / sub)
    info, poses, k = poses_stage.load(work)
    mesh = np.load(work / "fuse" / "mesh.npz")
    col = np.load(work / "collision" / "collision.npz")
    creport = read_json(work / "collision" / "collision.json")
    freport = read_json(work / "fuse" / "fuse.json")
    ctx = _generation_context(work)
    action_log = ctx.get("action_log") or {}
    gen = ctx.get("generation") or {}
    model = action_log.get("model") or {}
    safe = "".join(ch if (ch.isalnum() or ch in "_-") else "_" for ch in p.name.lower()) or "world"

    seed_origin = p.seed_image_origin or str((action_log.get("seed_image") or {}).get("origin") or "own")
    mode_review = str(model.get("licence_review", ""))
    distribution = "recipe-only" if seed_origin == "original" else "ship"
    if mode_review.startswith("REVIEW REQUIRED"):
        distribution = "never"  # game-footage fine-tunes: blocked until a human review clears it
    tool_rev = repo_revision()
    derived = [f"sha256:{h}" for h in (ctx.get("video_sha256"), ctx.get("action_log_sha256"),
                                        (action_log.get("seed_image") or {}).get("sha256")) if h]
    prov: dict[str, Any] = {
        "origin": "generated",
        "derived_from": derived,
        "recipe": f"recipe:worldextract/{ctx.get('recipe_hash', 'unknown')}",
        "tool": f"Tools/FUSE/WorldExtract/extract.py@{tool_rev}",
        "ai": {
            "provider": "local",
            "model": model.get("hf_repo", "unknown (no action log / generation manifest supplied)"),
            "model_name": model.get("name"),
            "model_version": model.get("revision"),
            "model_sha256": model.get("checkpoint_sha256"),
            "model_licence": model.get("licence"),
            "mode": action_log.get("mode"),
            "seed": action_log.get("seed"),
            "action_log_sha256": ctx.get("action_log_sha256"),
            "video_sha256": ctx.get("video_sha256"),
            "reconstruction": {"poses": info["backend"], "depth": read_json(work / "depth" / "depth.json")["backend"]},
        },
        "human_authorship": "selection",
    }
    tags = ["worldextract", "blockout", "ai-generated"]
    store = BlobStore(out)
    ids = {"mesh": f"mesh/worldextract/{safe}", "collision": f"mesh/worldextract/{safe}_collision",
           "material": f"mat/worldextract/{safe}", "texture": f"tex/worldextract/{safe}",
           "level": f"level/worldextract/{safe}"}

    pos, nrm, uvs, cols, faces = (mesh["positions"], mesh["normals"], mesh["uvs"], mesh["colors"], mesh["faces"])
    c8 = np.concatenate([np.clip(np.round(cols * 255), 0, 255).astype(np.uint8),
                         np.full((len(cols), 1), 255, np.uint8)], 1)
    streams = [
        {"semantic": "Position", "format": "F32x3", "data": store.put(pos.astype("<f4").tobytes(), "fuse/mesh-stream")},
        {"semantic": "Normal", "format": "F32x3", "data": store.put(nrm.astype("<f4").tobytes(), "fuse/mesh-stream")},
        {"semantic": "Uv0", "format": "F32x2", "data": store.put(uvs.astype("<f4").tobytes(), "fuse/mesh-stream")},
        {"semantic": "Color0", "format": "Unorm8x4", "data": store.put(c8.tobytes(), "fuse/mesh-stream")},
    ]
    bounds = [float(x) for x in np.r_[pos.min(0), pos.max(0)]]
    mesh_payload = {"streams": streams, "indices32": store.put(faces.astype("<u4").tobytes(), "fuse/mesh-stream"),
                    "submeshes": [{"indexOffset": 0, "indexCount": int(faces.size), "materialSlot": ids["material"]}],
                    "bounds": bounds, "skeletonId": "", "lodIds": []}
    cpos, cfaces = col["positions"], col["faces"]
    col_payload = {"streams": [{"semantic": "Position", "format": "F32x3",
                                "data": store.put(cpos.astype("<f4").tobytes(), "fuse/mesh-stream")}],
                   "indices32": store.put(cfaces.astype("<u4").tobytes(), "fuse/mesh-stream"),
                   "submeshes": [{"indexOffset": 0, "indexCount": int(cfaces.size), "materialSlot": ""}],
                   "bounds": [float(x) for x in np.r_[cpos.min(0), cpos.max(0)]], "skeletonId": "", "lodIds": []}

    png = None
    atlas_path = work / "fuse" / "atlas.png"
    tex_payload = None
    if atlas_path.exists():
        png = atlas_path.read_bytes()
        tex_payload = {"albedo": store.put(png, "image/png"), "normal": None, "orm": None, "height": None,
                       "emissive": None, "opacity": None, "normalConvention": "GL", "texelsPerMetre": 0.0,
                       "albedoSrgb": True}
    mat_payload = {"model": "Opaque", "textureSetId": ids["texture"] if tex_payload else "",
                   "baseColor": [1.0, 1.0, 1.0, 1.0], "roughness": 0.9, "metallic": 0.0, "emissiveNits": 0.0,
                   "alphaCutoff": 0.5, "twoSided": True, "physicalCategory": "generic", "layerRecipe": "",
                   "vertexColor": "COLOR_0 multiplies baseColor (baked TSDF colour)"}

    spawn = np.asarray(creport["spawn"]["position"])
    entities_poco = [
        {"archetype": "StaticMesh", "transform": _mat16(np.zeros(3)), "assetRefs": [ids["mesh"], ids["material"]],
         "components": [], "legacyClass": "", "legacyName": "WorldMesh"},
        {"archetype": "StaticCollider", "transform": _mat16(np.zeros(3)), "assetRefs": [ids["collision"]],
         "components": [{"type": "collision", "json": json.dumps({"walkable_max_slope_deg": creport["walkable"]["max_slope_deg"],
                                                                   "kill_z": creport["kill_z"]})}],
         "legacyClass": "", "legacyName": "WorldCollision"},
        {"archetype": "PlayerSpawn", "transform": _mat16(spawn), "assetRefs": [],
         "components": [{"type": "spawn", "json": json.dumps({"yaw_deg": creport["spawn"]["yaw_deg"]})}],
         "legacyClass": "", "legacyName": "PlayerSpawn"},
    ]
    level_payload = {"entities": entities_poco, "lightIds": [], "lookId": "", "skyId": "",
                     "groundPlane": creport["ground_plane"]}

    pocos: list[tuple[str, str, str, dict[str, Any]]] = [("mesh", ids["mesh"], f"{p.name} world mesh", mesh_payload),
             ("mesh", ids["collision"], f"{p.name} collision", col_payload),
             ("material", ids["material"], f"{p.name} material", mat_payload),
             ("level", ids["level"], f"{p.name} level", level_payload)]
    if tex_payload:
        pocos.append(("texture_set", ids["texture"], f"{p.name} albedo atlas", tex_payload))
    poco_files = []
    for kind, pid, nm, payload in pocos:
        path = _poco_path(out, kind, pid)
        write_json(path, _poco(kind, pid, nm, payload, prov, distribution, tags, p.reviewer))
        poco_files.append(path)

    extras = {"fuse_worldextract": {"units": "m", "source_up": "+Z (converted to glTF +Y)",
                                    "scale": info["scale"], "provenance_model": prov["ai"]["model"]}}
    write_glb(out / "world.glb", pos, faces, normals=nrm, uvs=uvs, colors=cols, png=png, name=safe, extras=extras)
    write_glb(out / "collision.glb", cpos, cfaces, name=f"{safe}_collision", extras=extras)

    # .fuselevel stub: +Y up, runtime Camera yaw/pitch (forward = sin(y)cos(p), sin(p), cos(y)cos(p)).
    c0 = poses[0]
    eye = ZUP_TO_YUP @ c0[:3, 3]
    fwd = ZUP_TO_YUP @ c0[:3, 2]
    yaw = math.degrees(math.atan2(fwd[0], fwd[2]))
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, fwd[1]))))
    w, h = info["intrinsics"]["width"], info["intrinsics"]["height"]
    vfov = math.degrees(2 * math.atan(0.5 * h / info["intrinsics"]["fy"]))
    extent = float(np.linalg.norm(pos.max(0) - pos.min(0)))
    sp = ZUP_TO_YUP @ spawn
    half = math.radians(yaw) / 2
    ident = (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0)
    entities: list[tuple[str, tuple[float, ...]]] = [
        (f"WorldExtract_{safe}", ident),
        ("WorldMesh", ident),
        ("WorldCollision", ident),
        ("PlayerSpawn", (float(sp[0]), float(sp[1]), float(sp[2]), 0.0, math.sin(half), 0.0, math.cos(half), 1.0, 1.0, 1.0)),
        (f"__fuse.wire|material|WorldMesh|{ids['material']}", ident),
        ("__fuse.wire|datablock|PlayerSpawn|PlayerSpawn", ident),
    ]
    write_fuselevel(out / "world.fuselevel", f"worldextract_{safe}",
                    {"fov_deg": vfov, "near": 0.1, "far": max(100.0, 2 * extent), "aspect": w / h,
                     "x": float(eye[0]), "y": float(eye[1]), "z": float(eye[2]), "yaw_deg": yaw, "pitch_deg": pitch},
                    entities)

    licence_records = [{
        "id": pid,
        "origin": "generated",
        "generator": f"Tools/FUSE/WorldExtract/extract.py@{tool_rev}",
        "seed": action_log.get("seed"),
        "licence": "LicenseRef-FUSE-Generated",
        "ai": {"provider": "local", "model": prov["ai"]["model"], "model_licence": prov["ai"]["model_licence"],
               "tier": "world-model-blockout", "recipe": prov["recipe"]},
        "distribution": distribution,
        "derived_from": derived,
        "files": [],
        "review": {"required": True, "by": p.reviewer or None, "date": None,
                   "reason": "REMASTER_PLAN §4.7: world-model geometry is a blockout; rebuild/edit before shipping"
                             + (f"; {mode_review}" if mode_review.startswith("REVIEW") else "")},
    } for _, pid, _, _ in pocos]
    write_json(out / "licences.fragment.json", {"schema": 1, "assets": licence_records})

    files = sorted([out / "world.glb", out / "collision.glb", out / "world.fuselevel", out / "licences.fragment.json"]
                   + poco_files + sorted((out / "blobs").rglob("*")), key=str)
    file_list = [{"path": str(f.relative_to(out)), "sha256": sha256_file(f), "size": f.stat().st_size}
                 for f in files if f.is_file()]
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "name": p.name,
        "tool": {"path": "Tools/FUSE/WorldExtract", "revision": tool_rev,
                 "version": __version__},
        "provenance": prov,
        "distribution": distribution,
        "licence": {"output": "LicenseRef-FUSE-Generated",
                    "world_model": {"name": model.get("name"), "licence": model.get("licence"),
                                    "note": model.get("licence_note")},
                    "review": licence_records[0]["review"]},
        "units": {
            "length": "m", "up_poco": "+Z", "up_gltf": "+Y", "up_fuselevel": "+Y (runtime Camera convention)",
            "scale": info["scale"],
            "note": "Monocular reconstruction has no true scale; metres come from the assumed eye height "
                    "(or action-speed prior). Treat absolute size as +/-20% until checked in the editor.",
        },
        "pocos": {k_: v for k_, v in ids.items() if k_ != "texture" or tex_payload},
        "fuselevel": {"path": "world.fuselevel", "format": "SceneSerialiser v1 stub",
                      "limitations": "v1 stores entity names + transforms only; the mesh is referenced by a "
                                     "__fuse.wire|material stub and the POCO level. Binding geometry needs the "
                                     "POCO mesh reader in mesh_cook (REMASTER_PLAN §2.4)."},
        "metrics": {"poses": info.get("stats"), "loop_closure": info.get("loop_closure"),
                    "calibration": info.get("calibration"), "fuse": freport, "collision": creport},
        "generation": gen,
        "files": file_list,
    }
    write_json(out / "worldextract.manifest.json", manifest)
    LOG.info("export: %d files, distribution=%s, %s", len(file_list), distribution, out)
    return {"files": len(file_list), "distribution": distribution}


def check_outputs(out: Path) -> None:
    """Re-hash every manifest entry (used by tests and `extract.py verify`)."""
    man = read_json(out / "worldextract.manifest.json")
    for f in man["files"]:
        path = out / f["path"]
        if not path.exists() or sha256_file(path) != f["sha256"]:
            raise WorldExtractError(f"manifest hash mismatch: {f['path']}")
