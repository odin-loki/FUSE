"""Provider routing for recipes that do not name a provider (plan §3.1 "Default for", A3; W4.6).

The §3.1 default for the agent tasks is ``gemini``. A3: originals go to Gemini only after the per-game
opt-in (``cloud_upload: opt-in`` + the recorded confirmation + a paid-tier key, the same three conditions
the W0.8 guard checks). Without it, a recipe with an original (or original-derived) input is routed to the
local fallback instead - ``local-llm`` for the text/vision agent tasks, ``local-torch`` (Qwen-Image-Edit,
FLUX.1-schnell, Wan, Whisper, Kokoro) for the media tasks - so the original never reaches the cloud.

A recipe that *names* ``gemini`` explicitly is not rerouted: the guard refuses it instead (exit 5).
"""

from __future__ import annotations

from typing import Any, Dict, Iterable, Optional

from . import providers
from .policy import ProjectPolicy
from .providers import InputRef, Task

LOCAL_FALLBACK: Dict[Task, str] = {
    Task.TAG_IMAGE: "local-llm", Task.INFER_MATERIAL: "local-llm",
    Task.SUMMARISE_CODE: "local-llm", Task.PORT_SCRIPT: "local-llm",
    Task.EDIT_IMAGE: "local-torch", Task.GEN_IMAGE: "local-torch", Task.GEN_VIDEO: "local-torch",
    Task.TRANSCRIBE: "local-torch", Task.TTS_SYNTH: "local-torch",
}


def cloud_allows_originals(project: ProjectPolicy, provider_id: str) -> bool:
    return (project.cloud_upload == "opt-in" and project.is_confirmed
            and project.cloud_tier.get(provider_id) == "paid")


def route(task: Task, inputs: Iterable[InputRef], project: ProjectPolicy) -> str:
    default = providers.default_provider_for(task)
    info = providers.PROVIDER_TABLE.get(default)
    if info is None or not info.cloud:
        return default
    if any(i.is_original_content for i in inputs) and not cloud_allows_originals(project, default):
        return LOCAL_FALLBACK.get(task, default)
    return default


def default_model(provider_id: str, task: Task) -> Optional[Dict[str, Any]]:
    """The pinned model a routed recipe gets when it names none (written into the resolved recipe)."""
    if provider_id == "gemini":
        return {"id": providers.GEMINI_DEFAULT_MODEL.get(task, "gemini-3.8-flash")}
    if provider_id == "local-llm":
        from .local_llm import DEFAULT_MODEL, PINNED_MODELS

        mid = DEFAULT_MODEL.get(task)
        return {"id": mid, "licence": PINNED_MODELS[mid]["licence"]} if mid else None
    return None
