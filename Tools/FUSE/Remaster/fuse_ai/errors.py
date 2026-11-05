"""Exception hierarchy for fuse_ai (plan §3.3, §0.1.5). Every error carries the CLI exit code it maps to."""


class FuseAIError(Exception):
    """Base class for every fuse_ai error."""

    exit_code = 1


class RecipeError(FuseAIError):
    """A recipe, project file or budget file is malformed (schema validation, bad JSON, missing input)."""

    exit_code = 2


class ReplayMiss(FuseAIError):
    """``replay`` mode found no cached result for the recipe hash (a hard failure in CI, §3.3)."""

    exit_code = 3


class BudgetExceeded(FuseAIError):
    """An estimate exceeds the remaining per-run / monthly / GPU-hour / rate budget (§3.3)."""

    exit_code = 4


class PolicyError(FuseAIError):
    """Base class of the cloud opt-in guard refusals (§0.1.5)."""

    exit_code = 5


class CloudOptInRequired(PolicyError):
    """An original (or original-derived) input would go to a cloud provider without opt-in + confirmation."""


class PaidTierRequired(PolicyError):
    """Originals may only go to a cloud provider on a paid-tier key (§0.1.5: the unpaid tier trains on prompts)."""


class ProviderNotImplemented(FuseAIError):
    """The provider id is in the §3.1 table but is a Wave 0 stub (only ``mock`` and ``replay`` are real)."""

    exit_code = 6


class VramExceeded(FuseAIError):
    """The request's peak VRAM estimate exceeds the 22 GB model budget of the 3090 profile (§3.5)."""

    exit_code = 7


class CacheCorrupt(FuseAIError):
    """A cache entry points at a missing blob or a blob whose sha256 does not match."""

    exit_code = 8


class ProviderUnavailable(ProviderNotImplemented):
    """The provider is implemented but not configured here (no API key, no recorded HTTP fixtures, no local
    server URL). It shares the stub's exit code, so CI behaves the same with or without the provider code."""


class SchemaViolation(FuseAIError):
    """A model output (live or replayed) does not parse, or does not match the prompt's JSON schema (W4.2)."""

    exit_code = 9


class TransportError(FuseAIError):
    """An HTTP exchange failed after its retries, or a replayed exchange does not match the recorded request."""

    exit_code = 10


class GateFailure(FuseAIError):
    """A CPU quality gate failed (collision vs render, rebuild tolerance, ...)."""

    exit_code = 11
