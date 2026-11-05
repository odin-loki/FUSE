"""HTTP transports for the REST providers (``gemini``, ``local-llm``), with recorded-fixture replay (W4.1, W4.6).

A provider never opens a socket itself; it hands an :class:`HttpRequest` to a :class:`Transport`:

* :class:`ReplayTransport` - serves responses from a hand-written or recorded fixture file
  (``fuse.ai-http-fixture/1``). Every request must match a recorded one exactly (method, path, JSON body),
  which is what makes the fixtures *contract tests* of the REST shape. Never touches the network.
* :class:`UrllibTransport` - the real client (``urllib.request``). Only built by :func:`live_transport`, which
  refuses when ``FUSE_AI_OFFLINE=1`` (set by every ctest) and, for local servers, for any non-loopback host.
* :class:`RecordingTransport` - wraps a live transport and writes the exchanges as a fixture, with the
  credential headers redacted, so a fixture can be recorded on the authoring machine.

Fixture file::

    {"schema": "fuse.ai-http-fixture/1", "provider": "gemini", "description": "...",
     "exchanges": [{"request":  {"method": "POST", "path": "/v1beta/models/m:generateContent", "body": {...}},
                    "response": {"status": 200, "headers": {"retry-after": "1"}, "body": {...}}}]}

:func:`send_with_retries` implements the retry policy shared by the providers: 408/429/500/502/503/504 are
retried with exponential backoff (``Retry-After`` honoured, capped), everything else fails at once.
"""

from __future__ import annotations

import copy
import ipaddress
import json
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Protocol
from urllib.parse import urlsplit

from .errors import PolicyError, ProviderUnavailable, TransportError

FIXTURE_SCHEMA = "fuse.ai-http-fixture/1"
REDACTED = "<redacted>"
SECRET_HEADERS = ("x-goog-api-key", "authorization")
RETRY_STATUSES = (408, 429, 500, 502, 503, 504)


@dataclass
class HttpRequest:
    method: str
    path: str                      # path + query, relative to the transport's base URL
    body: Optional[Any] = None     # JSON-serialisable
    headers: Dict[str, str] = field(default_factory=dict)


@dataclass
class HttpResponse:
    status: int
    body: Any = None
    headers: Dict[str, str] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return 200 <= self.status < 300


class Transport(Protocol):
    def send(self, req: HttpRequest) -> HttpResponse: ...


def _canon(obj: Any) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


class ReplayTransport:
    """Serves recorded exchanges; a request that matches none is a :class:`TransportError` (contract drift)."""

    offline = True

    def __init__(self, exchanges: List[Dict[str, Any]], *, name: str = "fixture",
                 require_headers: tuple = ()):
        self.exchanges = exchanges
        self.used = [False] * len(exchanges)
        self.name = name
        self.require_headers = require_headers
        self.calls: List[HttpRequest] = []

    @classmethod
    def load(cls, *paths: str | Path, require_headers: tuple = ()) -> "ReplayTransport":
        exchanges: List[Dict[str, Any]] = []
        names = []
        for p in paths:
            p = Path(p)
            try:
                d = json.loads(p.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as e:
                raise TransportError(f"cannot read HTTP fixture {p}: {e}") from None
            if d.get("schema") != FIXTURE_SCHEMA or not isinstance(d.get("exchanges"), list):
                raise TransportError(f"{p} is not a {FIXTURE_SCHEMA} fixture")
            exchanges += d["exchanges"]
            names.append(p.name)
        return cls(exchanges, name="+".join(names), require_headers=require_headers)

    def send(self, req: HttpRequest) -> HttpResponse:
        self.calls.append(req)
        lower = {k.lower(): v for k, v in req.headers.items()}
        for h in self.require_headers:
            if not lower.get(h):
                raise TransportError(f"request {req.method} {req.path} is missing the {h!r} header")
        body = _canon(req.body)
        same_path = []
        for i, ex in enumerate(self.exchanges):
            if self.used[i]:
                continue
            r = ex["request"]
            if r["method"] != req.method or r["path"] != req.path:
                continue
            same_path.append(r)
            if _canon(r.get("body")) == body:
                self.used[i] = True
                resp = ex["response"]
                return HttpResponse(int(resp["status"]), copy.deepcopy(resp.get("body")),
                                    {k.lower(): str(v) for k, v in (resp.get("headers") or {}).items()})
        hint = ""
        if same_path:
            hint = "; first differing key: " + _first_diff(same_path[0].get("body"), req.body)
        raise TransportError(f"no recorded exchange in {self.name} for {req.method} {req.path}{hint} "
                             "(the request shape changed: re-record or update the fixture)")

    def unused(self) -> List[Dict[str, Any]]:
        return [ex for ex, u in zip(self.exchanges, self.used) if not u]


def _first_diff(a: Any, b: Any, path: str = "$") -> str:
    if type(a) is not type(b):
        return f"{path} ({type(a).__name__} vs {type(b).__name__})"
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                return f"{path}.{k} (only in {'recorded' if k in a else 'request'})"
            if _canon(a[k]) != _canon(b[k]):
                return _first_diff(a[k], b[k], f"{path}.{k}")
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path} (length {len(a)} vs {len(b)})"
        for i, (x, y) in enumerate(zip(a, b)):
            if _canon(x) != _canon(y):
                return _first_diff(x, y, f"{path}[{i}]")
    return path


def is_loopback(url: str) -> bool:
    host = urlsplit(url).hostname or ""
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


class UrllibTransport:
    """The live client. Construct it through :func:`live_transport` only."""

    offline = False

    def __init__(self, base_url: str, timeout_s: float = 120.0):
        self.base_url = base_url.rstrip("/")
        self.timeout_s = timeout_s

    def send(self, req: HttpRequest) -> HttpResponse:  # pragma: no cover - never runs in CI (no network)
        import urllib.error
        import urllib.request

        data = None if req.body is None else json.dumps(req.body).encode("utf-8")
        headers = dict(req.headers)
        if data is not None:
            headers.setdefault("content-type", "application/json")
        r = urllib.request.Request(self.base_url + req.path, data=data, headers=headers, method=req.method)
        try:
            with urllib.request.urlopen(r, timeout=self.timeout_s) as resp:
                raw = resp.read()
                status, hdrs = resp.status, {k.lower(): v for k, v in resp.headers.items()}
        except urllib.error.HTTPError as e:
            raw, status, hdrs = e.read(), e.code, {k.lower(): v for k, v in e.headers.items()}
        except (urllib.error.URLError, OSError) as e:
            raise TransportError(f"{req.method} {self.base_url}{req.path}: {e}") from None
        try:
            body = json.loads(raw.decode("utf-8")) if raw else None
        except (UnicodeDecodeError, json.JSONDecodeError):
            body = {"raw": raw.decode("utf-8", errors="replace")[:2000]}
        return HttpResponse(status, body, hdrs)


class RecordingTransport:
    """Wraps a live transport and appends every exchange to a fixture file (secrets redacted)."""

    def __init__(self, inner: Transport, path: str | Path, provider: str, description: str = ""):
        self.inner = inner
        self.path = Path(path)
        self.provider = provider
        self.description = description
        self.exchanges: List[Dict[str, Any]] = []

    def send(self, req: HttpRequest) -> HttpResponse:  # pragma: no cover - needs a live transport
        resp = self.inner.send(req)
        self.exchanges.append({"request": {"method": req.method, "path": req.path, "body": req.body},
                               "response": {"status": resp.status, "headers": _keep_headers(resp.headers),
                                            "body": resp.body}})
        write_fixture(self.path, self.provider, self.exchanges, self.description)
        return resp


def _keep_headers(h: Dict[str, str]) -> Dict[str, str]:
    return {k: v for k, v in h.items() if k in ("retry-after", "content-type")}


def write_fixture(path: str | Path, provider: str, exchanges: List[Dict[str, Any]], description: str = "") -> None:
    doc = {"schema": FIXTURE_SCHEMA, "provider": provider, "description": description, "exchanges": exchanges}
    # Request headers (which carry the API key) are never recorded; bodies never contain credentials.
    text = json.dumps(doc, indent=1, sort_keys=True, ensure_ascii=False) + "\n"
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def live_transport(base_url: str, *, local_only: bool = False) -> UrllibTransport:
    """The only way to get a network transport. Refuses under ``FUSE_AI_OFFLINE=1`` (CI) and, for local
    servers (§3.5: workers listen only on 127.0.0.1), any non-loopback URL."""
    if local_only and not is_loopback(base_url):
        raise PolicyError(f"local provider URL {base_url!r} is not a loopback address (§3.5: local servers "
                          "listen only on 127.0.0.1)")
    if os.environ.get("FUSE_AI_OFFLINE", "") not in ("", "0"):
        raise ProviderUnavailable("FUSE_AI_OFFLINE is set: no live HTTP transport (use recorded fixtures)")
    return UrllibTransport(base_url)


def send_with_retries(transport: Transport, req: HttpRequest, *, max_retries: int = 4, base_delay_s: float = 1.0,
                      max_delay_s: float = 30.0, sleep: Callable[[float], None] = time.sleep,
                      log: Optional[List[Dict[str, Any]]] = None) -> HttpResponse:
    """Send with the shared retry policy. Returns the first non-retryable response; raises after the retries."""
    attempt = 0
    while True:
        resp = transport.send(req)
        if log is not None:
            log.append({"path": req.path, "status": resp.status, "attempt": attempt})
        if resp.status not in RETRY_STATUSES:
            return resp
        if attempt >= max_retries:
            raise TransportError(f"{req.method} {req.path}: HTTP {resp.status} after {attempt + 1} attempts")
        delay = min(max_delay_s, base_delay_s * (2 ** attempt))
        ra = resp.headers.get("retry-after")
        if ra:
            try:
                delay = min(max_delay_s, max(delay, float(ra)))
            except ValueError:
                pass
        sleep(delay)
        attempt += 1
