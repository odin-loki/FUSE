"""Stage `frames`: decode, keyframe selection (blur + parallax), static HUD/overlay masking.

Outputs (in <work>/frames/):
    key/f%06d.png        keyframes named by source frame index
    masks/f%06d.png.png  COLMAP-style masks (0 = ignore) when an overlay was found or given
    hud_mask.png         the full-resolution overlay mask (255 = valid pixel)
    frames.json          video metadata, per-keyframe scores, overlay boxes
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from ..common import WorldExtractError, write_json

LOG = logging.getLogger("worldextract.frames")


@dataclass
class FramesParams:
    max_frames: int = 0  # 0 = all
    stride: int = 1  # decode every n-th frame
    min_parallax_px: float = 0.0  # 0 = auto (4% of image width)
    max_gap: int = 12  # force a keyframe after this many decoded frames
    blur_percentile: float = 15.0  # reject the blurriest x% of candidates
    hud: str = "auto"  # auto | none | "x0,y0,x1,y1;..." boxes in pixels
    hud_var_threshold: float = 2.0  # temporal std (0..255) below which a pixel counts as static


def _decode(video: Path, p: FramesParams) -> tuple[list[np.ndarray], list[int], float]:
    import cv2

    cap = cv2.VideoCapture(str(video))
    if not cap.isOpened():
        raise WorldExtractError(f"cannot open video {video} (is it a valid MP4/H.264/MPEG-4 file?)")
    fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0) or 25.0
    frames, indices = [], []
    i = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if i % p.stride == 0:
            frames.append(frame)
            indices.append(i)
            if p.max_frames and len(frames) >= p.max_frames:
                break
        i += 1
    cap.release()
    if len(frames) < 3:
        raise WorldExtractError(f"video {video} decoded to {len(frames)} frames; need at least 3")
    return frames, indices, fps


def _parse_boxes(spec: str, w: int, h: int) -> list[tuple[int, int, int, int]]:
    boxes = []
    for part in spec.split(";"):
        part = part.strip()
        if not part:
            continue
        try:
            x0, y0, x1, y1 = (int(v) for v in part.split(","))
        except ValueError as exc:
            raise WorldExtractError(f"bad --hud box '{part}' (want x0,y0,x1,y1)") from exc
        boxes.append((max(0, x0), max(0, y0), min(w, x1), min(h, y1)))
    return boxes


def detect_static_overlay(frames: list[np.ndarray], threshold: float) -> tuple[np.ndarray, list[list[int]]]:
    """Pixels that stay constant while the rest of the image moves are HUD/overlay.

    Returns (mask uint8 255 = valid, boxes). Only static regions covering >= 0.2% of the image are
    kept (then padded), so compression noise does not produce speckle.
    """
    import cv2

    sample = frames[:: max(1, len(frames) // 60)]
    stack = np.stack([cv2.cvtColor(f, cv2.COLOR_BGR2GRAY).astype(np.float32) for f in sample])
    std = stack.std(axis=0)
    h, w = std.shape
    if float(np.median(std)) < 4 * threshold:
        LOG.warning("scene barely changes (median temporal std %.2f); skipping HUD detection", float(np.median(std)))
        return np.full((h, w), 255, np.uint8), []
    static = (std < threshold).astype(np.uint8)
    static = cv2.morphologyEx(static, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
    n, labels, stats, _ = cv2.connectedComponentsWithStats(static, connectivity=8)
    mask = np.full((h, w), 255, np.uint8)
    boxes: list[list[int]] = []
    min_area = 0.002 * h * w
    for lab in range(1, n):
        x, y, bw, bh, area = stats[lab]
        if area < min_area:
            continue
        pad = 4
        x0, y0, x1, y1 = max(0, x - pad), max(0, y - pad), min(w, x + bw + pad), min(h, y + bh + pad)
        mask[y0:y1, x0:x1] = 0
        boxes.append([int(x0), int(y0), int(x1), int(y1)])
    return mask, boxes


def _blur_score(gray: np.ndarray, mask: np.ndarray) -> float:
    import cv2

    lap = cv2.Laplacian(gray, cv2.CV_32F)
    return float(lap[mask > 0].var())


def _parallax(prev_gray: np.ndarray, gray: np.ndarray, mask: np.ndarray) -> float:
    """Median LK flow magnitude of corners tracked from the last keyframe (rotation included)."""
    import cv2

    pts = cv2.goodFeaturesToTrack(prev_gray, maxCorners=400, qualityLevel=0.01, minDistance=7, mask=mask)
    if pts is None or len(pts) < 8:
        return float("inf")  # nothing to track: force a keyframe
    nxt, status, _ = cv2.calcOpticalFlowPyrLK(prev_gray, gray, pts, None, winSize=(21, 21), maxLevel=3)
    ok = status.reshape(-1) == 1
    if ok.sum() < 8:
        return float("inf")
    return float(np.median(np.linalg.norm((nxt - pts).reshape(-1, 2)[ok], axis=1)))


def run(video: Path, out: Path, p: FramesParams) -> dict[str, Any]:
    import cv2

    frames, indices, fps = _decode(video, p)
    h, w = frames[0].shape[:2]
    LOG.info("decoded %d frames (%dx%d @ %.2f fps) from %s", len(frames), w, h, fps, video)

    boxes: list[list[int]]
    if p.hud == "none":
        mask, boxes = np.full((h, w), 255, np.uint8), []
    elif p.hud == "auto":
        mask, boxes = detect_static_overlay(frames, p.hud_var_threshold)
    else:
        boxes = [list(b) for b in _parse_boxes(p.hud, w, h)]
        mask = np.full((h, w), 255, np.uint8)
        for x0, y0, x1, y1 in boxes:
            mask[y0:y1, x0:x1] = 0
    if boxes:
        LOG.info("masking %d overlay region(s): %s", len(boxes), boxes)

    min_par = p.min_parallax_px or 0.04 * w
    grays = [cv2.cvtColor(f, cv2.COLOR_BGR2GRAY) for f in frames]
    blur = np.array([_blur_score(g, mask) for g in grays])
    blur_cut = float(np.percentile(blur, p.blur_percentile)) if p.blur_percentile > 0 else -1.0

    # Greedy selection: from the last keyframe, take the sharpest frame among those that reached the
    # parallax threshold (look-ahead window of 3 decoded frames), or force one after max_gap.
    selected = [0]
    parallax_of = {0: 0.0}
    last = 0
    i = 1
    while i < len(frames):
        par = _parallax(grays[last], grays[i], mask)
        if par >= min_par or i - last >= p.max_gap:
            window = [j for j in range(i, min(len(frames), i + 3)) if blur[j] > blur_cut] or [i]
            best = max(window, key=lambda j: blur[j])
            selected.append(best)
            parallax_of[best] = par
            last = best
            i = best + 1
        else:
            i += 1
    if selected[-1] != len(frames) - 1 and len(frames) - 1 - selected[-1] >= p.max_gap // 2:
        selected.append(len(frames) - 1)  # keep the end frame for loop closure
        parallax_of[len(frames) - 1] = _parallax(grays[last], grays[-1], mask)

    key_dir = out / "key"
    mask_dir = out / "masks"
    key_dir.mkdir(parents=True, exist_ok=True)
    for stale in list(key_dir.glob("*.png")) + list(mask_dir.glob("*.png")):
        stale.unlink()
    have_mask = bool(boxes)
    if have_mask:
        mask_dir.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out / "hud_mask.png"), mask)
    records = []
    for j in selected:
        name = f"f{indices[j]:06d}.png"
        cv2.imwrite(str(key_dir / name), frames[j])
        if have_mask:
            cv2.imwrite(str(mask_dir / (name + ".png")), mask)
        records.append({
            "name": name,
            "source_index": int(indices[j]),
            "blur": round(float(blur[j]), 3),
            "parallax_px": None if not np.isfinite(parallax_of[j]) else round(float(parallax_of[j]), 3),
        })
    info = {
        "video": str(video),
        "width": w,
        "height": h,
        "fps": fps,
        "decoded_frames": len(frames),
        "stride": p.stride,
        "min_parallax_px": min_par,
        "hud_boxes": boxes,
        "has_masks": have_mask,
        "keyframes": records,
    }
    write_json(out / "frames.json", info)
    LOG.info("selected %d keyframes of %d frames", len(records), len(frames))
    return {"keyframes": len(records), "decoded": len(frames), "hud_boxes": len(boxes)}
