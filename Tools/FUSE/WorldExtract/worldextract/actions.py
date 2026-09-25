"""Matrix-Game 2.0 action format, reconstruction-friendly trajectory presets, and pose priors.

Verified against SkyworkAI/Matrix-Game @ main, Matrix-Game-2/ (accessed 2026-09-23):
    * `inference.py`: input image resize-crop to 352x640 (H x W); `--num_output_frames` counts *latent*
      frames; the decoded video has `(latent - 1) * 4 + 1` frames; latents are generated in blocks of
      `num_frame_per_block = 3` (configs/inference_yaml/*.yaml), so latent % 3 == 0.
    * `utils/conditions.py` + `pipeline/causal_inference.py::get_current_action`: per *video* frame
      conditions. universal: keyboard [forward, back, left, right] (4, binary), mouse [pitch, yaw]
      with CAM_VALUE = 0.1 (`camera_up` = [+0.1, 0], `camera_r` = [0, +0.1]). gta_drive: keyboard
      [forward, back], mouse yaw only. templerun: keyboard 7-dim one-hot
      [nomove, jump, slide, turnleft, turnright, leftside, rightside], no mouse.

The mapping from mouse units to degrees and from a held key to metres per frame is NOT published.
The defaults below are assumptions; `extract.py poses` estimates both from the video and writes
them to `poses/poses.json::calibration`, which `generate.py --calibration` reads back.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass, field
from typing import Any, Callable

import numpy as np

from .geometry import yaw_pitch_to_cam2world

ACTION_SCHEMA = "fuse.worldextract.actions/1"

MG2_HEIGHT = 352
MG2_WIDTH = 640
MG2_FRAMES_PER_LATENT = 4
MG2_LATENT_BLOCK = 3
MG2_CAM_VALUE = 0.1


@dataclass(frozen=True)
class ModeSpec:
    name: str
    keyboard_labels: tuple[str, ...]
    has_mouse: bool
    can_strafe: bool
    can_steer: bool
    config_yaml: str
    checkpoint: str
    checkpoint_sha256: str  # git-LFS oid on huggingface.co/Skywork/Matrix-Game-2.0 @ f1729d99
    licence_review: str


MG2_HF_REPO = "Skywork/Matrix-Game-2.0"
MG2_HF_REVISION = "f1729d99a80e0f07993a77d7dad4a3190e23c2c8"
MG2_SHARED_FILES = {
    "Wan2.1_VAE.pth": "38071ab59bd94681c686fa51d75a1968f64e470262043be31f7a094e442fd981",
    "models_clip_open-clip-xlm-roberta-large-vit-huge-14.pth":
        "628c9998b613391f193eb67ff68da9667d75f492911e4eb3decf23460a158c38",
}

MODES: dict[str, ModeSpec] = {
    "universal": ModeSpec(
        "universal", ("forward", "back", "left", "right"), True, True, True,
        "configs/inference_yaml/inference_universal.yaml",
        "base_distilled_model/base_distill.safetensors",
        "55133515cf15a1ddc3d49e1f1102d79d536a1fca5459fdb2498b82a9d1b7a7e7",
        "none: trained on Unreal Engine renders plus other video (report arXiv:2508.13009)",
    ),
    "gta_drive": ModeSpec(
        "gta_drive", ("forward", "back"), True, False, True,
        "configs/inference_yaml/inference_gta_drive.yaml",
        "gta_distilled_model/gta_keyboard2dim.safetensors",
        "5472e1035b20bd58afc287791e2c715bf962dfc2fb48ab3b71dfd012fe2cb65f",
        "REVIEW REQUIRED: fine-tuned on GTA V footage; outputs imitate a third-party game (REMASTER_PLAN §0.1)",
    ),
    "templerun": ModeSpec(
        "templerun", ("nomove", "jump", "slide", "turnleft", "turnright", "leftside", "rightside"),
        False, False, False,
        "configs/inference_yaml/inference_templerun.yaml",
        "templerun_distilled_model/templerun_7dim_onlykey.safetensors",
        "a1dbe951e143fb86babf119da1ac29791f597b903e649537a3f8d348f631a245",
        "REVIEW REQUIRED: fine-tuned on Temple Run footage; outputs imitate a third-party game",
    ),
}

PRESETS = ("walkthrough", "orbit", "strafe_scan", "loop", "calibrate")


@dataclass
class Gains:
    """Action-to-motion gains. Defaults are UNVERIFIED assumptions for Matrix-Game 2.0."""

    deg_per_mouse_unit: float = 15.0  # yaw/pitch degrees per frame per 1.0 mouse unit (0.1 -> 1.5 deg/frame)
    m_per_frame: float = 0.06  # metres per frame with a movement key held (1.5 m/s at 25 fps)
    strafe_ratio: float = 1.0  # strafe speed / forward speed
    source: str = "assumed-default"


@dataclass
class TrajectoryParams:
    preset: str = "walkthrough"
    num_frames: int = 597
    speed: float = 0.5  # fraction of frames with the movement key held (sigma-delta duty cycle)
    max_turn_deg: float = 1.0  # max heading change per frame (slow turns only)
    weave_deg: float = 8.0  # heading weave amplitude on straight legs (parallax, zero net turn)
    strafe_weave: float = 0.35  # strafe duty amplitude on straight legs (zero net drift)
    rows: int = 4  # strafe_scan rows (even)
    key_hold: int = MG2_FRAMES_PER_LATENT  # frames a key decision is held (one latent frame)


@dataclass
class Intent:
    """Per-frame continuous intent. fwd/strafe in [-1, 1] (strafe +1 = right); turns in degrees."""

    fwd: np.ndarray
    strafe: np.ndarray
    yaw_left_deg: np.ndarray
    pitch_up_deg: np.ndarray
    closes_loop: bool
    notes: list[str] = field(default_factory=list)


def mg2_video_frames(latent_frames: int) -> int:
    return (latent_frames - 1) * MG2_FRAMES_PER_LATENT + 1


def mg2_latent_frames_for(video_frames: int) -> int:
    """Smallest valid latent count (multiple of 3) whose video has >= `video_frames` frames."""
    latent = max(1, math.ceil((video_frames - 1) / MG2_FRAMES_PER_LATENT) + 1)
    return int(math.ceil(latent / MG2_LATENT_BLOCK) * MG2_LATENT_BLOCK)


def _arc_frames(total_deg: float, p: TrajectoryParams) -> int:
    return int(math.ceil(abs(total_deg) / (0.8 * p.max_turn_deg)))


def _weave(n: int, amp_deg: float, periods: int) -> np.ndarray:
    """Per-frame yaw deltas whose integral is amp*sin(): zero net heading change over the leg."""
    t = np.arange(n + 1) / max(n, 1)
    heading = amp_deg * np.sin(2 * math.pi * periods * t)
    return np.diff(heading)


def _strafe_weave(n: int, amp: float, periods: int) -> np.ndarray:
    t = (np.arange(n) + 0.5) / max(n, 1)
    return amp * np.sin(2 * math.pi * periods * t)


def _leg(n: int, p: TrajectoryParams) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    fwd = np.full(n, p.speed)
    yaw = _weave(n, p.weave_deg, 2)
    yaw = np.clip(yaw, -p.max_turn_deg, p.max_turn_deg)
    strafe = _strafe_weave(n, p.strafe_weave, 3)
    return fwd, strafe, yaw


def _arc(n: int, total_deg: float, p: TrajectoryParams) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    return np.full(n, p.speed), np.zeros(n), np.full(n, total_deg / n)


def _cat(parts: list[tuple[np.ndarray, np.ndarray, np.ndarray]], n_total: int) -> tuple[np.ndarray, ...]:
    fwd = np.concatenate([x[0] for x in parts])
    strafe = np.concatenate([x[1] for x in parts])
    yaw = np.concatenate([x[2] for x in parts])
    pad = n_total - fwd.size
    if pad < 0:
        raise ValueError("internal: trajectory longer than frame budget")
    z = np.zeros(pad)
    return np.concatenate([fwd, z]), np.concatenate([strafe, z]), np.concatenate([yaw, z])


def _need(n: int, minimum: int, preset: str) -> None:
    if n < minimum:
        raise ValueError(
            f"preset '{preset}' needs at least {minimum} frames at max_turn_deg; got {n}. "
            "Increase --frames or --max-turn-deg (keep turns slow for reconstruction)."
        )


def build_intent(p: TrajectoryParams) -> Intent:
    """Reconstruction-friendly paths. Every preset closes its loop in heading and position
    (under the assumed gains), never rotates in place, and keeps turns <= max_turn_deg per frame."""
    n = p.num_frames - 1  # frame 0 is the seed image; motion happens on frames 1..n
    if not 0.05 <= p.speed <= 1.0:
        raise ValueError("--speed must be in [0.05, 1]")
    notes: list[str] = []
    if p.preset == "walkthrough":
        # Racetrack: weaving forward leg, 180 deg arc, identical leg back, 180 deg arc -> start pose.
        n_arc = _arc_frames(180.0, p)
        n_leg = (n - 2 * n_arc) // 2
        _need(n, 2 * n_arc + 2 * 40, p.preset)
        parts = [_leg(n_leg, p), _arc(n_arc, 180.0, p), _leg(n_leg, p), _arc(n_arc, 180.0, p)]
        notes.append("racetrack: leg / 180 arc / leg / 180 arc")
    elif p.preset == "loop":
        # One full circle while walking forward (tangent view), plus a zero-net strafe weave.
        _need(n, _arc_frames(360.0, p), p.preset)
        fwd, _, yaw = _arc(n, 360.0, p)
        parts = [(fwd, _strafe_weave(n, p.strafe_weave, 4), yaw)]
        notes.append("circle: forward + constant slow yaw, 360 deg")
    elif p.preset == "orbit":
        # Circle-strafe: strafe right while yawing left keeps the camera aimed at the orbit centre.
        _need(n, _arc_frames(360.0, p), p.preset)
        parts = [(np.zeros(n), np.full(n, p.speed), np.full(n, 360.0 / n))]
        notes.append("orbit: strafe right + yaw left, 360 deg around a point ahead")
    elif p.preset == "strafe_scan":
        if p.rows < 2 or p.rows % 2:
            raise ValueError("--rows must be an even number >= 2")
        n_arc = _arc_frames(180.0, p)
        budget = n - 2 * n_arc
        # rows*row + 2*(rows-1)*step with step = row/4 (scan steps + matching return leg).
        n_row = int(budget / (p.rows + 2 * (p.rows - 1) / 4.0))
        n_step = max(4, n_row // 4)
        _need(n, 2 * n_arc + p.rows * 24 + 2 * (p.rows - 1) * 6, p.preset)
        parts = []
        for r in range(p.rows):
            direction = 1.0 if r % 2 == 0 else -1.0
            parts.append((np.zeros(n_row), np.full(n_row, direction * p.speed), _weave(n_row, 4.0, 1)))
            if r < p.rows - 1:
                parts.append((np.full(n_step, p.speed), np.zeros(n_step), np.zeros(n_step)))
        n_back = (p.rows - 1) * n_step
        parts += [_arc(n_arc, 180.0, p), (np.full(n_back, p.speed), np.zeros(n_back), np.zeros(n_back)),
                  _arc(n_arc, 180.0, p)]
        notes.append(f"lawnmower {p.rows} rows, return via 180 arc / leg / 180 arc")
    elif p.preset == "calibrate":
        # Straight dolly, one slow 90 deg arc, straight dolly: enough to fit both gains.
        n_arc = _arc_frames(90.0, p)
        _need(n, n_arc + 60, p.preset)
        n_leg = (n - n_arc) // 2
        parts = [(np.full(n_leg, p.speed), np.zeros(n_leg), np.zeros(n_leg)), _arc(n_arc, 90.0, p),
                 (np.full(n_leg, p.speed), np.zeros(n_leg), np.zeros(n_leg))]
        notes.append("calibration: dolly / 90 arc / dolly (does not close the loop)")
    else:
        raise ValueError(f"unknown preset '{p.preset}' (choose from {', '.join(PRESETS)})")
    fwd, strafe, yaw = _cat(parts, n)
    z = np.zeros(1)
    return Intent(
        fwd=np.concatenate([z, fwd]),
        strafe=np.concatenate([z, strafe]),
        yaw_left_deg=np.concatenate([z, yaw]),
        pitch_up_deg=np.zeros(p.num_frames),
        closes_loop=p.preset != "calibrate",
        notes=notes,
    )


def _sigma_delta(values: np.ndarray, hold: int) -> np.ndarray:
    """Binary +/-1/0 key presses whose running mean tracks `values`, decided every `hold` frames."""
    out = np.zeros(values.size, dtype=np.int8)
    acc = 0.0
    for start in range(1, values.size, hold):
        block = values[start:start + hold]
        acc += float(block.sum())
        press = int(round(acc / block.size)) if block.size else 0
        press = max(-1, min(1, press))
        out[start:start + hold] = press
        acc -= press * block.size
    return out


def quantise(intent: Intent, mode: ModeSpec, gains: Gains, hold: int) -> tuple[np.ndarray, np.ndarray, list[str]]:
    """Intent -> (keyboard (N, K) int8, mouse (N, 2) float32 [pitch, yaw]) in Matrix-Game 2.0 format."""
    warnings: list[str] = []
    n = intent.fwd.size
    kb = np.zeros((n, len(mode.keyboard_labels)), dtype=np.int8)
    mouse = np.zeros((n, 2), dtype=np.float32)
    if mode.name == "templerun":
        raise ValueError("templerun mode has no free camera control; reconstruction presets are unsupported")
    fwd = _sigma_delta(intent.fwd, hold)
    kb[:, 0] = fwd > 0
    kb[:, 1] = fwd < 0
    if np.any(np.abs(intent.strafe) > 1e-9):
        if not mode.can_strafe:
            raise ValueError(f"mode '{mode.name}' cannot strafe; use preset walkthrough or loop")
        st = _sigma_delta(intent.strafe, hold)
        kb[:, 2] = st < 0
        kb[:, 3] = st > 0
    yaw_units = -intent.yaw_left_deg / gains.deg_per_mouse_unit  # mouse yaw +: turn right
    pitch_units = intent.pitch_up_deg / gains.deg_per_mouse_unit
    for name, arr in (("yaw", yaw_units), ("pitch", pitch_units)):
        if np.any(np.abs(arr) > MG2_CAM_VALUE + 1e-9):
            warnings.append(f"{name} rate clipped to +/-{MG2_CAM_VALUE} mouse units")
    mouse[:, 0] = np.clip(pitch_units, -MG2_CAM_VALUE, MG2_CAM_VALUE)
    mouse[:, 1] = np.clip(yaw_units, -MG2_CAM_VALUE, MG2_CAM_VALUE)
    if not mode.has_mouse:
        mouse[:] = 0.0
    return kb, mouse, warnings


def integrate(
    keyboard: np.ndarray,
    mouse: np.ndarray,
    labels: tuple[str, ...] | list[str],
    gains: Gains,
    eye_height: float = 1.7,
) -> np.ndarray:
    """Action log -> prior cam2world poses (N, 4, 4). Frame 0 at (0, 0, eye_height) facing +X."""
    labels = list(labels)
    idx = {name: labels.index(name) for name in labels}

    def key(t: int, name: str) -> float:
        return float(keyboard[t, idx[name]]) if name in idx else 0.0

    n = keyboard.shape[0]
    poses = np.zeros((n, 4, 4))
    yaw, pitch = 0.0, 0.0
    pos = np.array([0.0, 0.0, eye_height])
    step = gains.m_per_frame
    for t in range(n):
        if t > 0:
            yaw -= math.radians(float(mouse[t, 1]) * gains.deg_per_mouse_unit)
            pitch += math.radians(float(mouse[t, 0]) * gains.deg_per_mouse_unit)
            pitch = max(-1.4, min(1.4, pitch))
            heading = np.array([math.cos(yaw), math.sin(yaw), 0.0])
            right = np.array([math.sin(yaw), -math.cos(yaw), 0.0])
            fwd = key(t, "forward") - key(t, "back")
            side = key(t, "right") - key(t, "left")
            pos = pos + step * fwd * heading + step * gains.strafe_ratio * side * right
        poses[t] = yaw_pitch_to_cam2world(yaw, pitch, pos)
    return poses


def make_action_log(
    *,
    mode: ModeSpec,
    params: TrajectoryParams,
    gains: Gains,
    keyboard: np.ndarray,
    mouse: np.ndarray,
    intent: Intent,
    seed: int,
    fps: float,
    width: int,
    height: int,
    model: dict[str, Any],
    seed_image: dict[str, Any] | None,
    camera: dict[str, Any] | None = None,
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    log: dict[str, Any] = {
        "schema": ACTION_SCHEMA,
        "model": model,
        "mode": mode.name,
        "keyboard_labels": list(mode.keyboard_labels),
        "mouse_labels": ["pitch_up", "yaw_right"],
        "cam_value": MG2_CAM_VALUE,
        "seed": seed,
        "seed_image": seed_image,
        "video": {"width": width, "height": height, "fps": fps, "num_frames": int(keyboard.shape[0])},
        "camera": camera or {"hfov_deg": None, "eye_height_m": None},
        "preset": asdict(params),
        "preset_notes": intent.notes,
        "gains": asdict(gains),
        "closes_loop": intent.closes_loop,
        "keyboard": keyboard.astype(int).tolist(),
        "mouse": [[round(float(a), 6), round(float(b), 6)] for a, b in mouse],
    }
    if extra:
        log.update(extra)
    return log


def load_action_log(data: dict[str, Any]) -> tuple[np.ndarray, np.ndarray, list[str], Gains]:
    if data.get("schema") != ACTION_SCHEMA:
        raise ValueError(f"unsupported action log schema {data.get('schema')!r} (want {ACTION_SCHEMA})")
    kb = np.asarray(data["keyboard"], dtype=np.float64)
    mouse = np.asarray(data["mouse"], dtype=np.float64)
    if kb.shape[0] != mouse.shape[0]:
        raise ValueError("action log keyboard/mouse lengths differ")
    g = data.get("gains", {})
    gains = Gains(
        deg_per_mouse_unit=float(g.get("deg_per_mouse_unit", Gains.deg_per_mouse_unit)),
        m_per_frame=float(g.get("m_per_frame", Gains.m_per_frame)),
        strafe_ratio=float(g.get("strafe_ratio", Gains.strafe_ratio)),
        source=str(g.get("source", "action-log")),
    )
    return kb, mouse, list(data["keyboard_labels"]), gains


PresetFn = Callable[[TrajectoryParams], Intent]
