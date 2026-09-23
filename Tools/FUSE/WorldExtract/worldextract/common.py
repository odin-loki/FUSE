"""Shared helpers: logging, hashing, JSON IO, resumable stage cache, dependency checks."""

from __future__ import annotations

import hashlib
import importlib
import json
import logging
import os
import random
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping

LOG = logging.getLogger("worldextract")

TOOL_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = TOOL_DIR.parent.parent.parent

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_FAILED = 1
EXIT_SKIP = 77  # ctest SKIP_RETURN_CODE convention used across FUSE


class WorldExtractError(RuntimeError):
    """A user-facing error: printed without a traceback."""


def setup_logging(level: str = "INFO", log_file: Path | None = None) -> None:
    root = logging.getLogger("worldextract")
    root.handlers.clear()
    root.setLevel(getattr(logging, level.upper(), logging.INFO))
    fmt = logging.Formatter("%(asctime)s %(levelname)-7s %(name)s: %(message)s", "%H:%M:%S")
    console = logging.StreamHandler(sys.stderr)
    console.setFormatter(fmt)
    root.addHandler(console)
    if log_file is not None:
        log_file.parent.mkdir(parents=True, exist_ok=True)
        fh = logging.FileHandler(log_file, encoding="utf-8")
        fh.setFormatter(fmt)
        root.addHandler(fh)


def seed_everything(seed: int) -> None:
    """Deterministic seeds for every RNG a stage may touch."""
    random.seed(seed)
    os.environ.setdefault("PYTHONHASHSEED", str(seed))
    try:
        import numpy as np

        np.random.seed(seed % (2**32))
    except ImportError:
        pass
    try:
        import cv2

        cv2.setRNGSeed(seed)
    except ImportError:
        pass
    try:
        import open3d as o3d

        o3d.utility.random.seed(seed)
    except (ImportError, AttributeError):
        pass
    try:
        import pycolmap

        pycolmap.set_random_seed(seed)
    except (ImportError, AttributeError):
        pass
    if "torch" in sys.modules:
        import torch

        torch.manual_seed(seed)
        if torch.cuda.is_available():
            torch.cuda.manual_seed_all(seed)


def sha256_file(path: Path, chunk: int = 1 << 22) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            block = fh.read(chunk)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def sha256_file_cached(path: Path) -> str:
    """sha256 of a (possibly multi-GB) weights file, cached in `<file>.sha256` keyed by size+mtime."""
    st = path.stat()
    sidecar = path.with_name(path.name + ".sha256")
    key = f"{st.st_size}:{int(st.st_mtime)}"
    if sidecar.exists():
        parts = sidecar.read_text(encoding="utf-8").split()
        if len(parts) == 2 and parts[1] == key:
            return parts[0]
    digest = sha256_file(path)
    try:
        sidecar.write_text(f"{digest} {key}\n", encoding="utf-8")
    except OSError:
        pass  # read-only weights dir: just do not cache
    return digest


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def write_json(path: Path, obj: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")
    tmp.replace(path)


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise WorldExtractError(f"missing file: {path}") from exc
    except json.JSONDecodeError as exc:
        raise WorldExtractError(f"invalid JSON in {path}: {exc}") from exc


def repo_revision() -> str:
    """FUSE git revision read from .git files (no git subprocess); 'unknown' outside a checkout."""
    git = REPO_ROOT / ".git"
    try:
        if git.is_file():  # worktree: "gitdir: <path>"
            git = Path(git.read_text(encoding="utf-8").split(":", 1)[1].strip())
        head = (git / "HEAD").read_text(encoding="utf-8").strip()
        if head.startswith("ref:"):
            ref = head.split(" ", 1)[1]
            ref_file = git / ref
            if ref_file.exists():
                return ref_file.read_text(encoding="utf-8").strip()
            packed = git / "packed-refs"
            if packed.exists():
                for line in packed.read_text(encoding="utf-8").splitlines():
                    if line.endswith(" " + ref):
                        return line.split(" ", 1)[0]
            return "unknown"
        return head
    except (OSError, IndexError):
        return "unknown"


def require(modules: Iterable[str], purpose: str) -> None:
    """Fail with one clear message listing every missing python module."""
    missing = []
    for name in modules:
        try:
            importlib.import_module(name)
        except ImportError:
            missing.append(name)
    if missing:
        raise WorldExtractError(
            f"{purpose} needs python modules that are not installed: {', '.join(missing)}. "
            f"Install with: pip install -r {TOOL_DIR / 'requirements.txt'}"
        )


def missing_modules(modules: Iterable[str]) -> list[str]:
    out = []
    for name in modules:
        try:
            importlib.import_module(name)
        except ImportError:
            out.append(name)
    return out


def cuda_available() -> bool:
    if missing_modules(["torch"]):
        return False
    import torch

    return bool(torch.cuda.is_available())


@dataclass
class Stage:
    """Resumable stage bookkeeping.

    A stage is complete when `<work>/<name>/stage.json` exists and its `params_hash` matches the
    current parameters and upstream hashes. `--force` recomputes regardless.
    """

    work: Path
    name: str
    params: Mapping[str, Any]
    force: bool = False

    @property
    def dir(self) -> Path:
        return self.work / self.name

    @property
    def marker(self) -> Path:
        return self.dir / "stage.json"

    def params_hash(self) -> str:
        return sha256_bytes(canonical_json(dict(self.params)))[:16]

    def is_done(self) -> bool:
        if self.force or not self.marker.exists():
            return False
        try:
            info = json.loads(self.marker.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return False
        return info.get("params_hash") == self.params_hash() and info.get("status") == "ok"

    def begin(self) -> Path:
        self.dir.mkdir(parents=True, exist_ok=True)
        if self.marker.exists():
            self.marker.unlink()
        return self.dir

    def finish(self, summary: Mapping[str, Any]) -> None:
        write_json(
            self.marker,
            {
                "stage": self.name,
                "status": "ok",
                "params": dict(self.params),
                "params_hash": self.params_hash(),
                "summary": dict(summary),
            },
        )

    def summary(self) -> dict[str, Any]:
        return dict(read_json(self.marker).get("summary", {}))


def upstream_hash(work: Path, stage_name: str) -> str:
    marker = work / stage_name / "stage.json"
    if not marker.exists():
        raise WorldExtractError(
            f"stage '{stage_name}' has not been run in {work}; run `extract.py {stage_name}` (or `all`) first"
        )
    return str(read_json(marker).get("params_hash", ""))
