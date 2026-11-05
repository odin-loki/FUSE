"""Provider protocol, request/result types, the provider table and the registry (plan §3.1, §3.2, §3.5).

The ``Provider`` protocol is the one in §3.1::

    id: str
    capabilities() -> set[Task]
    vram_mb(req) -> int          # peak VRAM estimate, used by the 3090 scheduler (§3.5)
    estimate(req) -> Cost
    run(req) -> Result           # pure function of (req, model version, seed) as far as the provider allows

Wave 0 (W0.5) ships only the ``mock`` provider (see :mod:`fuse_ai.mock`) as a real implementation;
every other id of the §3.1 table is a stub whose ``run()`` raises :class:`ProviderNotImplemented`
*without* touching the network. Their ``estimate()`` and ``vram_mb()`` already work (pure arithmetic
on the §3.2 price list and the §3.5 VRAM figures) so budgets and the scheduler can be tested.
"""

from __future__ import annotations

import enum
import math
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Protocol, Set, runtime_checkable

from .errors import FuseAIError, ProviderNotImplemented, RecipeError


class Task(str, enum.Enum):
    """The task vocabulary of §3.1."""

    TAG_IMAGE = "TAG_IMAGE"
    INFER_MATERIAL = "INFER_MATERIAL"
    UPSCALE = "UPSCALE"
    PBR_MAPS = "PBR_MAPS"
    EDIT_IMAGE = "EDIT_IMAGE"
    GEN_IMAGE = "GEN_IMAGE"
    SUMMARISE_CODE = "SUMMARISE_CODE"
    PORT_SCRIPT = "PORT_SCRIPT"
    GEN_VIDEO = "GEN_VIDEO"
    SEGMENT = "SEGMENT"
    TRANSCRIBE = "TRANSCRIBE"
    TTS_SYNTH = "TTS_SYNTH"
    WORLD_ROLLOUT = "WORLD_ROLLOUT"
    IMAGE_TO_3D = "IMAGE_TO_3D"
    SEPARATE_STEMS = "SEPARATE_STEMS"
    AUDIO_SR = "AUDIO_SR"

    @classmethod
    def parse(cls, value: str) -> "Task":
        try:
            return cls(value)
        except ValueError:
            raise RecipeError(f"unknown task {value!r}; expected one of {', '.join(t.value for t in cls)}") from None


ALL_TASKS: frozenset = frozenset(Task)

# Provenance origins of an input (W0.8 / §0.1.5, §2.2 of the plan).
ORIGINS = ("original", "library", "generated", "derived", "authored")


@dataclass
class Cost:
    """Estimated or actual cost of one request; serialises to the recipe ``cost`` block (§3.3)."""

    usd: float = 0.0
    tokens_in: int = 0
    tokens_out: int = 0
    seconds: float = 0.0
    gpu: Optional[str] = None
    peak_vram_mb: int = 0
    kwh: float = 0.0

    @property
    def gpu_hours(self) -> float:
        return self.seconds / 3600.0 if self.gpu else 0.0

    def to_json(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {
            "usd": round(self.usd, 6),
            "tokens_in": int(self.tokens_in),
            "tokens_out": int(self.tokens_out),
            "seconds": round(self.seconds, 3),
            "peak_vram_mb": int(self.peak_vram_mb),
            "kwh": round(self.kwh, 6),
        }
        if self.gpu:
            out["gpu"] = self.gpu
        return out

    @classmethod
    def from_json(cls, d: Optional[Dict[str, Any]]) -> "Cost":
        d = d or {}
        return cls(usd=float(d.get("usd", 0.0)), tokens_in=int(d.get("tokens_in", 0)),
                   tokens_out=int(d.get("tokens_out", 0)), seconds=float(d.get("seconds", 0.0)),
                   gpu=d.get("gpu"), peak_vram_mb=int(d.get("peak_vram_mb", 0)), kwh=float(d.get("kwh", 0.0)))


@dataclass
class InputRef:
    """One recipe input: a content reference plus its provenance (§3.3 ``inputs``; §0.1.5 for ``origin``).

    ``origin`` is one of :data:`ORIGINS`. A missing origin is treated as ``original`` (fail closed).
    ``derived_from_original`` marks a ``derived`` input with an original anywhere in its chain.
    ``path`` is only a locator (relative to the recipe file); it is not part of the recipe hash.
    """

    oaid: Optional[str] = None
    canonical_sha256: Optional[str] = None
    sha256: Optional[str] = None
    path: Optional[str] = None
    name: Optional[str] = None
    media_type: Optional[str] = None
    origin: str = "original"
    derived_from_original: bool = False

    @property
    def content_key(self) -> Optional[str]:
        return self.canonical_sha256 or self.sha256

    @property
    def is_original_content(self) -> bool:
        """True when the bytes are an original asset or derived from one (the §0.1.2/§0.1.5 test)."""
        return self.origin == "original" or bool(self.derived_from_original)


@dataclass
class Request:
    """A resolved recipe, ready for a provider. ``input_data[i]`` holds the bytes of ``inputs[i]`` (or None)."""

    task: Task
    provider_id: str
    model: Dict[str, Any] = field(default_factory=dict)
    params: Dict[str, Any] = field(default_factory=dict)
    seed: int = 0
    inputs: List[InputRef] = field(default_factory=list)
    input_data: List[Optional[bytes]] = field(default_factory=list)
    purpose: str = ""
    recipe_hash: str = ""
    # Set only by fuse_ai.policy.check_request(); cloud providers refuse to run without a valid one.
    clearance: Optional[str] = None

    @property
    def model_id(self) -> str:
        return str(self.model.get("id", "")) if self.model else ""


@dataclass
class Output:
    name: str
    media_type: str
    data: bytes


@dataclass
class Result:
    """Provider output: one or more named byte blobs plus metadata. ``outputs[0]`` is the primary output."""

    outputs: List[Output]
    meta: Dict[str, Any] = field(default_factory=dict)
    cost: Cost = field(default_factory=Cost)
    provider_id: str = ""
    model_id: str = ""
    from_cache: bool = False

    @property
    def primary(self) -> Output:
        return self.outputs[0]


@runtime_checkable
class Provider(Protocol):
    """The §3.1 provider protocol."""

    id: str

    def capabilities(self) -> Set[Task]: ...

    def vram_mb(self, req: Request) -> int: ...

    def estimate(self, req: Request) -> Cost: ...

    def run(self, req: Request) -> Result: ...


# ---- the §3.1 provider table ----------------------------------------------------------------------------------


@dataclass(frozen=True)
class ProviderInfo:
    id: str
    tasks: frozenset
    where: str
    cloud: bool
    default_for: frozenset
    notes: str
    human: bool = False          # reference: human-operated, never automated
    implemented: bool = False    # W0: only mock


def _ts(*names: str) -> frozenset:
    return frozenset(Task(n) for n in names)


PROVIDER_TABLE: Dict[str, ProviderInfo] = {
    p.id: p
    for p in (
        ProviderInfo("mock", ALL_TASKS, "CI (CPU)", False, frozenset(),
                     "Deterministic CPU stand-ins; test plumbing, not quality.", implemented=True),
        ProviderInfo("local-onnx", _ts("UPSCALE", "PBR_MAPS", "SEGMENT"), "CPU or the 3090 (CUDA EP)", False,
                     _ts("UPSCALE", "PBR_MAPS", "SEGMENT"),
                     "ONNX Runtime (MIT) with pinned models from §3.4.1 (PBRify_Remix CC0)."),
        ProviderInfo("local-torch", _ts("IMAGE_TO_3D", "GEN_IMAGE", "EDIT_IMAGE", "GEN_VIDEO", "AUDIO_SR",
                                        "SEPARATE_STEMS", "TTS_SYNTH", "TRANSCRIBE"),
                     "the 3090, one fuse_ai worker per model", False,
                     _ts("IMAGE_TO_3D", "AUDIO_SR", "SEPARATE_STEMS", "TTS_SYNTH", "TRANSCRIBE"),
                     "PyTorch (BSD-3) workers for the FOSS models of §3.4.1."),
        ProviderInfo("worldmodel", _ts("WORLD_ROLLOUT"), "the 3090", False, _ts("WORLD_ROLLOUT"),
                     "Matrix-Game 2.0 (MIT) by default; outputs are reference assets (§4.7)."),
        ProviderInfo("gemini", _ts("TAG_IMAGE", "INFER_MATERIAL", "SUMMARISE_CODE", "PORT_SCRIPT", "EDIT_IMAGE",
                                   "GEN_IMAGE", "GEN_VIDEO", "TRANSCRIBE", "TTS_SYNTH"),
                     "cloud, paid key", True,
                     _ts("TAG_IMAGE", "INFER_MATERIAL", "SUMMARISE_CODE", "PORT_SCRIPT", "EDIT_IMAGE", "GEN_IMAGE",
                         "GEN_VIDEO"),
                     "The user-approved exception to §0.1.0; originals only with the per-game opt-in (§0.1.5).",
                     implemented=True),
        ProviderInfo("local-llm", _ts("TAG_IMAGE", "INFER_MATERIAL", "SUMMARISE_CODE", "PORT_SCRIPT"),
                     "the 3090, llama.cpp llama-server on 127.0.0.1", False, frozenset(),
                     "Offline fallback (Qwen3-Coder-30B-A3B, Qwen3-VL-8B GGUF).", implemented=True),
        ProviderInfo("comfyui", _ts("UPSCALE", "PBR_MAPS", "GEN_IMAGE"), "the 3090, user-run (GPL: HTTP API only)",
                     False, frozenset(), "Separate process over its local HTTP API; pinned workflow JSON."),
        ProviderInfo("remix-toolkit", frozenset(), "Windows + Omniverse Toolkit (local REST)", False, frozenset(),
                     "Optional live link for users who already run the Toolkit; not FOSS at the Kit layer."),
        ProviderInfo("reference", frozenset(), "human", True, frozenset(),
                     "Project Genie / World Labs Marble outputs logged by hand; never given originals.",
                     human=True),
    )
}

# §3.1 "Default for" column, plus SEGMENT -> local-onnx (the only non-mock provider that lists it).
DEFAULT_PROVIDER: Dict[Task, str] = {}
for _info in PROVIDER_TABLE.values():
    for _t in _info.default_for:
        DEFAULT_PROVIDER[_t] = _info.id
assert set(DEFAULT_PROVIDER) == set(Task), "every task needs a default provider"


def default_provider_for(task: Task) -> str:
    return DEFAULT_PROVIDER[task]


# ---- stubs: estimates work, run() refuses -----------------------------------------------------------------------

# Peak VRAM (MB) per task on the 3090 for the default local models (§3.5; SPECULATIVE until W3.8/W9.1).
_LOCAL_VRAM_MB = {
    Task.UPSCALE: 2100, Task.PBR_MAPS: 2100, Task.SEGMENT: 1500, Task.IMAGE_TO_3D: 6000,
    Task.GEN_IMAGE: 16000, Task.EDIT_IMAGE: 18000, Task.GEN_VIDEO: 21000, Task.AUDIO_SR: 6000,
    Task.SEPARATE_STEMS: 3000, Task.TTS_SYNTH: 1000, Task.TRANSCRIBE: 4000, Task.WORLD_ROLLOUT: 20000,
    Task.TAG_IMAGE: 9000, Task.INFER_MATERIAL: 9000, Task.SUMMARISE_CODE: 20000, Task.PORT_SCRIPT: 20000,
}
# Rough seconds per request on the 3090 (§3.5 throughput table).
_LOCAL_SECONDS = {
    Task.UPSCALE: 2.0, Task.PBR_MAPS: 2.0, Task.SEGMENT: 1.0, Task.IMAGE_TO_3D: 2.0, Task.GEN_IMAGE: 10.0,
    Task.EDIT_IMAGE: 30.0, Task.GEN_VIDEO: 900.0, Task.AUDIO_SR: 60.0, Task.SEPARATE_STEMS: 30.0,
    Task.TTS_SYNTH: 2.0, Task.TRANSCRIBE: 20.0, Task.WORLD_ROLLOUT: 180.0, Task.TAG_IMAGE: 3.0,
    Task.INFER_MATERIAL: 3.0, Task.SUMMARISE_CODE: 120.0, Task.PORT_SCRIPT: 300.0,
}
RTX3090_LOAD_KW = 0.45  # §7.4


class StubProvider:
    """A §3.1 provider id that is not implemented in Wave 0. ``run()`` never touches the network."""

    def __init__(self, info: ProviderInfo):
        self.info = info
        self.id = info.id

    def capabilities(self) -> Set[Task]:
        return set(self.info.tasks)

    def vram_mb(self, req: Request) -> int:
        if self.info.cloud or self.info.human or self.id == "remix-toolkit":
            return 0
        return _LOCAL_VRAM_MB.get(req.task, 0)

    def estimate(self, req: Request) -> Cost:
        if self.info.cloud or self.info.human:
            return Cost()
        seconds = float(req.params.get("est_seconds", _LOCAL_SECONDS.get(req.task, 1.0)))
        return Cost(seconds=seconds, gpu="rtx3090", peak_vram_mb=self.vram_mb(req),
                    kwh=seconds / 3600.0 * RTX3090_LOAD_KW)

    def run(self, req: Request) -> Result:
        if self.info.human:
            raise ProviderNotImplemented(
                f"provider {self.id!r} is human-operated (reference assets are logged by hand, §4.7); "
                "it never runs automatically")
        raise ProviderNotImplemented(
            f"provider {self.id!r} is not implemented in W0 (only 'mock' and the 'replay' cache mode are); "
            "use --mode mock, or --mode replay against a recorded cache")


# Gemini list prices (§3.2, paid tier, USD per 1M tokens in/out) and per-image / per-second prices.
GEMINI_TOKEN_PRICES = {
    "gemini-3.8-flash": (0.75, 3.75),
    "gemini-3.7-flash": (0.75, 3.75),
    "gemini-3.6-flash": (0.75, 3.75),
    "gemini-3.5-flash-lite": (0.30, 2.50),
    "gemini-3.1-pro-preview": (2.00, 12.00),
}
GEMINI_IMAGE_PRICES = {
    "gemini-3.1-flash-image": {"0.5K": 0.045, "1K": 0.067, "2K": 0.101, "4K": 0.151},
    "gemini-3-pro-image": {"1K": 0.134, "2K": 0.134, "4K": 0.24},
}
VEO_PRICE_PER_SECOND = {"veo-3.1-generate-preview": 0.40, "veo-3.1-lite-generate-preview": 0.08}
GEMINI_DEFAULT_MODEL = {
    Task.SUMMARISE_CODE: "gemini-3.1-pro-preview", Task.PORT_SCRIPT: "gemini-3.1-pro-preview",
    Task.EDIT_IMAGE: "gemini-3.1-flash-image", Task.GEN_IMAGE: "gemini-3.1-flash-image",
    Task.GEN_VIDEO: "veo-3.1-generate-preview",
}


class GeminiStub(StubProvider):
    """The cloud agent (§3.1). W0: estimates from the §3.2 price list; ``run()`` checks the guard, then refuses."""

    def estimate(self, req: Request) -> Cost:
        model = req.model_id or GEMINI_DEFAULT_MODEL.get(req.task, "gemini-3.8-flash")
        p = req.params
        discount = 0.5 if p.get("batch") else 1.0
        if model in GEMINI_IMAGE_PRICES:
            size = str(p.get("size", "1K"))
            table = GEMINI_IMAGE_PRICES[model]
            price = table.get(size, max(table.values()))
            return Cost(usd=price * int(p.get("n_images", 1)) * discount)
        if model in VEO_PRICE_PER_SECOND:
            return Cost(usd=VEO_PRICE_PER_SECOND[model] * float(p.get("duration_s", 8)) * discount)
        price_in, price_out = GEMINI_TOKEN_PRICES.get(model, max(GEMINI_TOKEN_PRICES.values()))
        data_tokens = sum(math.ceil(len(d) / 4) for d in req.input_data if d)
        tokens_in = int(p.get("est_tokens_in", data_tokens + 1024))
        tokens_out = int(p.get("max_output_tokens", 8192))
        usd = (tokens_in * price_in + tokens_out * price_out) / 1e6 * discount
        return Cost(usd=usd, tokens_in=tokens_in, tokens_out=tokens_out)

    def run(self, req: Request) -> Result:
        from . import policy  # local import: policy imports this module

        policy.verify_clearance(req)  # defence in depth: refuse if the runner's guard did not clear this request
        return super().run(req)


# ---- registry ---------------------------------------------------------------------------------------------------

_REGISTRY: Dict[str, Provider] = {}


def register(provider: Provider, *, replace: bool = False) -> None:
    if not replace and provider.id in _REGISTRY:
        raise FuseAIError(f"provider {provider.id!r} already registered")
    _REGISTRY[provider.id] = provider


def unregister(provider_id: str) -> None:
    _REGISTRY.pop(provider_id, None)


def get(provider_id: str) -> Provider:
    _ensure_builtin()
    try:
        return _REGISTRY[provider_id]
    except KeyError:
        raise RecipeError(f"unknown provider {provider_id!r}; known: {', '.join(sorted(_REGISTRY))}") from None


def info(provider_id: str) -> ProviderInfo:
    """The §3.1 table row, or a synthetic local, non-cloud row for providers registered by tests/plugins."""
    if provider_id in PROVIDER_TABLE:
        return PROVIDER_TABLE[provider_id]
    p = get(provider_id)
    cloud = bool(getattr(p, "cloud", False))
    return ProviderInfo(provider_id, frozenset(p.capabilities()), "plugin", cloud, frozenset(), "registered plugin",
                        implemented=True)


def registered_ids() -> List[str]:
    _ensure_builtin()
    return sorted(_REGISTRY)


_BUILTIN_DONE = False


def _ensure_builtin() -> None:
    global _BUILTIN_DONE
    if _BUILTIN_DONE:
        return
    _BUILTIN_DONE = True
    from .gemini import GeminiProvider
    from .local_llm import LocalLLMProvider
    from .mock import MockProvider

    # W4.1 / W4.6: real REST clients; unconfigured (no key, no fixtures) they refuse like the W0 stubs did.
    factories: Dict[str, Callable[[ProviderInfo], Provider]] = {"gemini": lambda _row: GeminiProvider(),
                                                                "local-llm": lambda _row: LocalLLMProvider()}
    for pid, row in PROVIDER_TABLE.items():
        if pid in _REGISTRY:
            continue
        if pid == "mock":
            _REGISTRY[pid] = MockProvider()
        else:
            _REGISTRY[pid] = factories.get(pid, StubProvider)(row)
