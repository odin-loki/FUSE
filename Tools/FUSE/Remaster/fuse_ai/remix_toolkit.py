"""The ``remix-toolkit`` provider: an optional live link to a locally running RTX Remix Toolkit (plan W2.5, §0.1.7).

The Remix Toolkit (Apache-2.0 source on NVIDIA Omniverse Kit, which is not FOSS) exposes a local REST service. FUSE
never depends on it: the FOSS path is ``fuse_remaster export-remix`` writing the mod layer itself. Users who already
run the Toolkit can push an export-remix manifest into the open project with :meth:`RemixToolkitClient.push_mod`.

**Localhost only.** The client refuses any base URL that is not ``http://`` on a loopback host (127.0.0.0/8, ``::1``,
``localhost``), re-checks the host on every request and never follows redirects. Nothing here talks to the
internet, and originals never leave the machine (the Toolkit runs on the same machine).

**Routes.** All routes live in :data:`ROUTES`, the single place to fix them. They are modelled on the Toolkit's
REST services (toolkit-remix ``lightspeed.trex.*service*`` extensions: ``/stagecraft/...`` for project, layers,
assets and textures, ``/ingestcraft/...`` for ingestion; default port 8011). Every route, its method, the query
flags and the response shape are **[VERIFY against toolkit-remix]**: the tree holds no Toolkit source and this was
written without network access. The mock (:class:`MockRemixToolkit`) implements exactly this table, so the
contract tests pin *our* contract; fixing a route means editing ``ROUTES`` and the mock's matching handler.

Transport: ``transport(method, path, query, body) -> (status, json_or_None)``; ``path`` is already expanded,
``query`` a dict, ``body`` a JSON-able object or None. The default transport is urllib (loopback only); tests
inject :class:`MockRemixToolkit` and run with the network blocked.
"""

from __future__ import annotations

import ipaddress
import json
import threading
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

from .errors import FuseAIError
from .providers import Cost, Request, Result, Task

DEFAULT_BASE_URL = "http://127.0.0.1:8011"
MANIFEST_SCHEMA = "fuse.remaster.remix-export/1"

# name -> (method, path template). [VERIFY against toolkit-remix] for every row (see the module docstring).
ROUTES: Dict[str, Tuple[str, str]] = {
    "get_project": ("GET", "/stagecraft/project/"),
    "list_layers": ("GET", "/stagecraft/layers/"),
    "create_layer": ("POST", "/stagecraft/layers/"),
    "get_edit_target": ("GET", "/stagecraft/layers/target/"),
    "set_edit_target": ("PUT", "/stagecraft/layers/target/{layer_id}"),
    "save_layer": ("POST", "/stagecraft/layers/{layer_id}/save"),
    "list_assets": ("GET", "/stagecraft/assets/"),
    "asset_textures": ("GET", "/stagecraft/assets/{asset_path}/textures"),
    "set_texture": ("PUT", "/stagecraft/assets/{asset_path}/file-paths"),
    "set_mesh_reference": ("PUT", "/stagecraft/assets/{asset_path}/reference"),
    "ingest_texture": ("POST", "/ingestcraft/mass-validator/queue/material"),
}

# Remix texture input names per POCO texture slot (AperturePBR_Opacity, plan §2.5).
TEXTURE_INPUTS: Dict[str, str] = {
    "albedo": "inputs:diffuse_texture",
    "normal": "inputs:normalmap_texture",
    "roughness": "inputs:reflectionroughness_texture",
    "metallic": "inputs:metallic_texture",
    "emissive": "inputs:emissive_mask_texture",
    "height": "inputs:height_texture",
}


class RemixToolkitError(FuseAIError):
    """A refused URL, an HTTP error status or a malformed response from the Toolkit link."""

    exit_code = 9


Transport = Callable[[str, str, Dict[str, Any], Any], Tuple[int, Any]]


# ---- loopback guard ---------------------------------------------------------------------------------------------


def is_loopback_host(host: Optional[str]) -> bool:
    if not host:
        return False
    h = host.strip("[]").lower()
    if h == "localhost":
        return True
    try:
        ip = ipaddress.ip_address(h)
    except ValueError:
        return False
    mapped = getattr(ip, "ipv4_mapped", None)
    if mapped is not None:
        ip = mapped
    return ip.is_loopback


def check_base_url(base_url: str) -> str:
    """Returns the normalised base URL (no trailing slash) or raises :class:`RemixToolkitError`."""
    try:
        u = urllib.parse.urlsplit(base_url)
        host = u.hostname
        _ = u.port  # raises ValueError on a bad port
    except ValueError as e:
        raise RemixToolkitError(f"remix-toolkit: malformed base URL {base_url!r}: {e}") from None
    if u.scheme != "http":
        raise RemixToolkitError(f"remix-toolkit: only http:// on loopback is allowed (got scheme {u.scheme!r})")
    if u.username or u.password:
        raise RemixToolkitError("remix-toolkit: credentials in the base URL are refused")
    if not is_loopback_host(host):
        raise RemixToolkitError(f"remix-toolkit: host {host!r} is not loopback; the Toolkit link is localhost only")
    if u.query or u.fragment:
        raise RemixToolkitError("remix-toolkit: the base URL must not carry a query or fragment")
    return urllib.parse.urlunsplit((u.scheme, u.netloc, u.path.rstrip("/"), "", ""))


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D401
        raise RemixToolkitError(f"remix-toolkit: redirect to {newurl!r} refused")


def urllib_transport(base_url: str, timeout: float = 10.0) -> Transport:
    """The default transport: urllib against a loopback base URL, no proxies, no redirects."""
    base = check_base_url(base_url)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())

    def transport(method: str, path: str, query: Dict[str, Any], body: Any) -> Tuple[int, Any]:
        qs = urllib.parse.urlencode(sorted((k, _qv(v)) for k, v in (query or {}).items()))
        url = base + path + ("?" + qs if qs else "")
        if not is_loopback_host(urllib.parse.urlsplit(url).hostname):  # re-check per request
            raise RemixToolkitError(f"remix-toolkit: refusing non-loopback URL {url!r}")
        data = None if body is None else json.dumps(body, sort_keys=True).encode("utf-8")
        req = urllib.request.Request(url, data=data, method=method,
                                     headers={"Content-Type": "application/json", "Accept": "application/json"})
        try:
            with opener.open(req, timeout=timeout) as resp:
                status, raw = resp.status, resp.read()
        except urllib.error.HTTPError as e:
            status, raw = e.code, e.read()
        except urllib.error.URLError as e:
            raise RemixToolkitError(f"remix-toolkit: cannot reach {base}: {e.reason}") from None
        return status, _decode(raw)

    return transport


class _NotJson:
    def __init__(self, raw: bytes):
        self.raw = raw


def _decode(raw: bytes) -> Any:
    if not raw:
        return None
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return _NotJson(raw)


def _qv(v: Any) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


# ---- client -----------------------------------------------------------------------------------------------------


def _need(obj: Any, key: str, typ: type, what: str) -> Any:
    if not isinstance(obj, dict) or key not in obj or not isinstance(obj[key], typ):
        raise RemixToolkitError(f"remix-toolkit: malformed {what} response: missing or non-{typ.__name__} '{key}'")
    return obj[key]


def _str_list(obj: Any, key: str, what: str) -> List[str]:
    items = _need(obj, key, list, what)
    if not all(isinstance(x, str) for x in items):
        raise RemixToolkitError(f"remix-toolkit: malformed {what} response: '{key}' must hold strings")
    return list(items)


class RemixToolkitClient:
    def __init__(self, base_url: str = DEFAULT_BASE_URL, transport: Optional[Transport] = None,
                 timeout: float = 10.0):
        self.base_url = check_base_url(base_url)
        self._transport = transport if transport is not None else urllib_transport(self.base_url, timeout)
        self.calls: List[Tuple[str, str]] = []  # (method, path), in order

    # -- plumbing
    def _call(self, route: str, params: Optional[Dict[str, str]] = None, query: Optional[Dict[str, Any]] = None,
              body: Any = None) -> Any:
        method, template = ROUTES[route]
        enc = {k: urllib.parse.quote(v, safe="") for k, v in (params or {}).items()}
        path = template.format(**enc)
        self.calls.append((method, path))
        status, payload = self._transport(method, path, dict(query or {}), body)
        if isinstance(payload, _NotJson):
            raise RemixToolkitError(f"remix-toolkit: {method} {path}: HTTP {status} with a non-JSON body")
        if not 200 <= int(status) < 300:
            detail = payload.get("detail") if isinstance(payload, dict) else None
            raise RemixToolkitError(f"remix-toolkit: {method} {path}: HTTP {status}" + (f": {detail}" if detail else ""))
        return payload

    # -- routes
    def get_project(self) -> Dict[str, Any]:
        r = self._call("get_project")
        _need(r, "layer_id", str, "project")
        return r

    def list_layers(self) -> List[str]:
        return _str_list(self._call("list_layers"), "layer_ids", "layers")

    def create_layer(self, layer_path: str, parent_layer_id: str, *, set_edit_target: bool = False) -> str:
        r = self._call("create_layer", body={"layer_path": layer_path, "parent_layer_id": parent_layer_id,
                                             "set_edit_target": bool(set_edit_target), "create_or_insert": True})
        return _need(r, "layer_id", str, "create layer")

    def get_edit_target(self) -> str:
        return _need(self._call("get_edit_target"), "layer_id", str, "edit target")

    def set_edit_target(self, layer_id: str) -> None:
        self._call("set_edit_target", {"layer_id": layer_id})

    def save_layer(self, layer_id: str) -> None:
        self._call("save_layer", {"layer_id": layer_id})

    def list_assets(self, *, selection: bool = False, filter_session_assets: bool = False,
                    asset_hashes: bool = True) -> List[str]:
        r = self._call("list_assets", query={"selection": selection, "filter_session_assets": filter_session_assets,
                                             "asset_hashes": asset_hashes})
        return _str_list(r, "asset_paths", "assets")

    def asset_textures(self, asset_path: str) -> Dict[str, str]:
        r = self._call("asset_textures", {"asset_path": asset_path})
        tex = _need(r, "textures", list, "textures")
        out: Dict[str, str] = {}
        for t in tex:
            if not (isinstance(t, list) and len(t) == 2 and all(isinstance(x, str) for x in t)):
                raise RemixToolkitError("remix-toolkit: malformed textures response: entries are [input, path]")
            out[t[0]] = t[1]
        return out

    def set_texture(self, asset_path: str, texture_input: str, file_path: str) -> None:
        self._call("set_texture", {"asset_path": asset_path},
                   body={"asset_file_paths": [[asset_path + "/Shader." + texture_input, file_path]]})

    def set_mesh_reference(self, asset_path: str, reference_path: str) -> str:
        r = self._call("set_mesh_reference", {"asset_path": asset_path},
                       body={"asset_file_path": reference_path, "force": False})
        refs = _str_list(r, "reference_paths", "reference")
        if not refs:
            raise RemixToolkitError("remix-toolkit: malformed reference response: empty 'reference_paths'")
        return refs[0]

    # -- high level
    def push_mod(self, manifest: Dict[str, Any], layer_name: Optional[str] = None) -> Dict[str, Any]:
        """Pushes an export-remix manifest into the open Toolkit project; returns a deterministic summary."""
        if not isinstance(manifest, dict) or manifest.get("schema") != MANIFEST_SCHEMA:
            raise RemixToolkitError(f"remix-toolkit: manifest schema must be {MANIFEST_SCHEMA!r}")
        reps = manifest.get("replacements")
        if not isinstance(reps, list):
            raise RemixToolkitError("remix-toolkit: manifest 'replacements' must be a list")
        mod = manifest.get("mod", "mod.usda")
        if not isinstance(mod, str) or not mod:
            raise RemixToolkitError("remix-toolkit: manifest 'mod' must be a layer path")
        project = self.get_project()
        layers = self.list_layers()
        target = layer_name or mod
        layer_id = target if target in layers else self.create_layer(target, project["layer_id"])
        self.set_edit_target(layer_id)
        known = set(self.list_assets(asset_hashes=True))
        applied: List[Dict[str, Any]] = []
        missing: List[str] = []
        for rep in sorted(reps, key=lambda r: (str(r.get("prim", "")), str(r.get("oaid", "")))):
            prim, kind = rep.get("prim"), rep.get("kind")
            if not isinstance(prim, str) or kind not in ("mesh", "material"):
                raise RemixToolkitError(f"remix-toolkit: bad replacement entry {rep!r}")
            if prim not in known:
                missing.append(prim)
                continue
            done: Dict[str, Any] = {"prim": prim, "kind": kind, "oaid": rep.get("oaid")}
            if kind == "mesh":
                ml = rep.get("mesh_layer")
                if not isinstance(ml, str):
                    raise RemixToolkitError(f"remix-toolkit: mesh replacement {prim} needs 'mesh_layer'")
                done["reference"] = self.set_mesh_reference(prim, ml)
            textures = rep.get("textures") or {}
            if not isinstance(textures, dict):
                raise RemixToolkitError(f"remix-toolkit: 'textures' of {prim} must be an object")
            done["textures"] = []
            for slot in sorted(textures):
                if slot not in TEXTURE_INPUTS:
                    raise RemixToolkitError(f"remix-toolkit: unknown texture slot {slot!r} on {prim}")
                self.set_texture(prim, TEXTURE_INPUTS[slot], textures[slot])
                done["textures"].append(slot)
            applied.append(done)
        self.save_layer(layer_id)
        return {"layer_id": layer_id, "applied": applied, "missing": sorted(missing),
                "calls": len(self.calls)}


# ---- provider ---------------------------------------------------------------------------------------------------


class RemixToolkitProvider:
    """§3.1 row ``remix-toolkit``: a live link, not an AI task provider."""

    id = "remix-toolkit"
    cloud = False

    def __init__(self, info: Any = None):
        self.info = info

    def capabilities(self) -> Set[Task]:
        return set()

    def vram_mb(self, req: Request) -> int:
        return 0

    def estimate(self, req: Request) -> Cost:
        return Cost()

    def run(self, req: Request) -> Result:
        raise RemixToolkitError(
            "provider 'remix-toolkit' is a live link to a local RTX Remix Toolkit, not an AI task provider; "
            "use RemixToolkitProvider.client(...).push_mod(manifest)")

    def client(self, base_url: str = DEFAULT_BASE_URL, transport: Optional[Transport] = None,
               timeout: float = 10.0) -> RemixToolkitClient:
        return RemixToolkitClient(base_url, transport, timeout)


# ---- mock Toolkit -----------------------------------------------------------------------------------------------


class MockRemixToolkit:
    """In-process stand-in for the Toolkit's REST service, implementing exactly :data:`ROUTES`.

    Usable directly as a transport. State: a project root layer, sublayers, the edit target, captured assets
    (prim paths), texture inputs per material and mesh references, saved layers. ``fail`` maps a route name to a
    forced (status, payload) for error-path tests.
    """

    def __init__(self, assets: Optional[List[str]] = None, project_layer: str = "project.usda"):
        self.root = project_layer
        self.layers: List[str] = [project_layer, "capture/capture.usda"]
        self.edit_target = project_layer
        self.assets: List[str] = sorted(assets or [])
        self.textures: Dict[str, Dict[str, Dict[str, str]]] = {}  # layer -> "<prim>/Shader.<input>" -> path
        self.references: Dict[str, Dict[str, str]] = {}           # layer -> prim -> reference
        self.saved: List[str] = []
        self.requests: List[Tuple[str, str, Dict[str, Any], Any]] = []
        self.fail: Dict[str, Tuple[int, Any]] = {}
        self._lock = threading.Lock()

    def _match(self, method: str, path: str) -> Tuple[Optional[str], Dict[str, str]]:
        segs = path.split("/")
        for name, (m, template) in ROUTES.items():
            tsegs = template.split("/")
            if m != method or len(tsegs) != len(segs):
                continue
            params: Dict[str, str] = {}
            for t, s in zip(tsegs, segs):
                if t.startswith("{") and t.endswith("}"):
                    if not s:
                        break
                    params[t[1:-1]] = urllib.parse.unquote(s)
                elif t != s:
                    break
            else:
                return name, params
        return None, {}

    def __call__(self, method: str, path: str, query: Dict[str, Any], body: Any) -> Tuple[int, Any]:
        with self._lock:
            self.requests.append((method, path, dict(query or {}), body))
            name, p = self._match(method, path)
            if name is None:
                return 404, {"detail": f"no route {method} {path}"}
            if name in self.fail:
                return self.fail[name]
            return getattr(self, "_h_" + name)(p, query or {}, body)

    def _bad(self, msg: str, status: int = 422) -> Tuple[int, Any]:
        return status, {"detail": msg}

    def _h_get_project(self, p, q, b):
        return 200, {"layer_id": self.root}

    def _h_list_layers(self, p, q, b):
        return 200, {"layer_ids": list(self.layers)}

    def _h_create_layer(self, p, q, b):
        if not isinstance(b, dict) or not isinstance(b.get("layer_path"), str) or not b["layer_path"]:
            return self._bad("layer_path required")
        if b.get("parent_layer_id") not in self.layers:
            return self._bad("unknown parent layer", 404)
        if b["layer_path"] in self.layers:
            return self._bad("layer exists", 400)
        self.layers.append(b["layer_path"])
        if b.get("set_edit_target"):
            self.edit_target = b["layer_path"]
        return 200, {"layer_id": b["layer_path"]}

    def _h_get_edit_target(self, p, q, b):
        return 200, {"layer_id": self.edit_target}

    def _h_set_edit_target(self, p, q, b):
        if p["layer_id"] not in self.layers:
            return self._bad("unknown layer", 404)
        if p["layer_id"].startswith("capture/"):
            return self._bad("the capture layer cannot be an edit target", 400)
        self.edit_target = p["layer_id"]
        return 200, {}

    def _h_save_layer(self, p, q, b):
        if p["layer_id"] not in self.layers:
            return self._bad("unknown layer", 404)
        self.saved.append(p["layer_id"])
        return 200, {}

    def _h_list_assets(self, p, q, b):
        for k in ("selection", "filter_session_assets", "asset_hashes"):
            if k in q and _qv(q[k]) not in ("true", "false"):
                return self._bad(f"{k} must be a bool")
        return 200, {"asset_paths": [] if _qv(q.get("selection", False)) == "true" else list(self.assets)}

    def _known(self, prim: str) -> bool:
        return prim in self.assets

    def _h_asset_textures(self, p, q, b):
        prim = p["asset_path"]
        if not self._known(prim):
            return self._bad("unknown asset", 404)
        out = []
        for layer in self.layers:
            for k, v in sorted(self.textures.get(layer, {}).items()):
                if k.startswith(prim + "/Shader."):
                    out.append([k.split("/Shader.", 1)[1], v])
        return 200, {"textures": out}

    def _h_set_texture(self, p, q, b):
        prim = p["asset_path"]
        if not self._known(prim):
            return self._bad("unknown asset", 404)
        if not prim.startswith("/RootNode/Looks/mat_") and not prim.startswith("/RootNode/meshes/mesh_"):
            return self._bad("not a material or mesh prim", 400)
        fps = b.get("asset_file_paths") if isinstance(b, dict) else None
        if not isinstance(fps, list) or not fps:
            return self._bad("asset_file_paths required")
        for e in fps:
            if not (isinstance(e, list) and len(e) == 2 and all(isinstance(x, str) for x in e)):
                return self._bad("asset_file_paths entries are [attribute, path]")
            if not e[0].startswith(prim + "/Shader.inputs:") or not e[1].lower().endswith(".dds"):
                return self._bad("texture attribute must be <prim>/Shader.inputs:* and the file a .dds")
        for attr, path in fps:
            self.textures.setdefault(self.edit_target, {})[attr] = path
        return 200, {}

    def _h_set_mesh_reference(self, p, q, b):
        prim = p["asset_path"]
        if not self._known(prim):
            return self._bad("unknown asset", 404)
        if not prim.startswith("/RootNode/meshes/mesh_"):
            return self._bad("not a mesh prim", 400)
        ref = b.get("asset_file_path") if isinstance(b, dict) else None
        if not isinstance(ref, str) or not ref.lower().endswith((".usd", ".usda", ".usdc")):
            return self._bad("asset_file_path must be a USD layer")
        self.references.setdefault(self.edit_target, {})[prim] = ref
        return 200, {"reference_paths": [prim + "/ref_" + str(len(self.references[self.edit_target]))]}

    def _h_ingest_texture(self, p, q, b):
        return self._bad("ingestion is not modelled by the mock", 400)


def serve_mock(mock: Optional[MockRemixToolkit] = None, host: str = "127.0.0.1", port: int = 0):
    """Serves ``mock`` over HTTP on a loopback address. Returns (server, thread, base_url); call server.shutdown()."""
    import http.server

    if not is_loopback_host(host):
        raise RemixToolkitError(f"serve_mock: {host!r} is not loopback")
    mock = mock or MockRemixToolkit()

    class Handler(http.server.BaseHTTPRequestHandler):
        def _go(self):
            u = urllib.parse.urlsplit(self.path)
            query = dict(urllib.parse.parse_qsl(u.query))
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n) if n else b""
            body = None
            if raw:
                try:
                    body = json.loads(raw.decode("utf-8"))
                except ValueError:
                    status, payload = 400, {"detail": "body is not JSON"}
                    return self._send(status, payload)
            status, payload = mock(self.command, u.path, query, body)
            self._send(status, payload)

        def _send(self, status, payload):
            data = json.dumps(payload, sort_keys=True).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        do_GET = do_POST = do_PUT = do_DELETE = _go

        def log_message(self, *args):  # quiet
            pass

    server = http.server.ThreadingHTTPServer((host, port), Handler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    h = server.server_address[0]
    base = f"http://{'[' + h + ']' if ':' in h else h}:{server.server_address[1]}"
    return server, t, base
