"""Geometry helpers (numpy only): conventions, Sim(3) alignment, plane RANSAC, metrics.

Conventions used throughout WorldExtract:
    * World frame: right-handed, +Z up, metres (FUSE POCO canonical units, REMASTER_PLAN §2.2).
    * Camera frame: OpenCV (+X right, +Y down, +Z forward). Poses are stored as 4x4 cam2world.
    * glTF export converts +Z-up to glTF's +Y-up with `ZUP_TO_YUP`.
"""

from __future__ import annotations

import math

import numpy as np

ZUP_TO_YUP = np.array([[1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, -1.0, 0.0]])


def yaw_pitch_to_cam2world(yaw_rad: float, pitch_rad: float, position: np.ndarray) -> np.ndarray:
    """Player yaw (about +Z, 0 = looking along +X, positive = turn left) and pitch (positive = up)."""
    cp, sp = math.cos(pitch_rad), math.sin(pitch_rad)
    cy, sy = math.cos(yaw_rad), math.sin(yaw_rad)
    forward = np.array([cp * cy, cp * sy, sp])
    right = np.array([sy, -cy, 0.0])
    down = np.cross(forward, right)
    t = np.eye(4)
    t[:3, 0], t[:3, 1], t[:3, 2], t[:3, 3] = right, down, forward, position
    return t


def invert_pose(t: np.ndarray) -> np.ndarray:
    out = np.eye(4)
    r = t[:3, :3]
    out[:3, :3] = r.T
    out[:3, 3] = -r.T @ t[:3, 3]
    return out


def umeyama(src: np.ndarray, dst: np.ndarray, with_scale: bool = True) -> tuple[float, np.ndarray, np.ndarray]:
    """Least-squares Sim(3) with dst ~= s * R @ src + t (Umeyama 1991). Inputs are (N, 3)."""
    if src.shape != dst.shape or src.shape[0] < 3:
        raise ValueError("umeyama needs >= 3 matching points")
    mu_s, mu_d = src.mean(0), dst.mean(0)
    xs, xd = src - mu_s, dst - mu_d
    cov = xd.T @ xs / src.shape[0]
    u, d, vt = np.linalg.svd(cov)
    s_fix = np.eye(3)
    if np.linalg.det(u) * np.linalg.det(vt) < 0:
        s_fix[2, 2] = -1.0
    r = u @ s_fix @ vt
    var_s = (xs**2).sum() / src.shape[0]
    scale = float(np.trace(np.diag(d) @ s_fix) / var_s) if with_scale and var_s > 0 else 1.0
    t = mu_d - scale * r @ mu_s
    return scale, r, t


def pose_anchor_points(cam2world: np.ndarray, reach: float) -> np.ndarray:
    """Camera centres plus points one `reach` along each camera's forward/down axes.

    Aligning these instead of centres alone keeps Sim(3) well-posed for straight-line trajectories.
    """
    c = cam2world[:, :3, 3]
    f = c + reach * cam2world[:, :3, 2]
    d = c + reach * cam2world[:, :3, 1]
    return np.concatenate([c, f, d], axis=0)


def apply_sim3_to_poses(poses: np.ndarray, s: float, r: np.ndarray, t: np.ndarray) -> np.ndarray:
    out = poses.copy()
    out[:, :3, :3] = r @ poses[:, :3, :3]
    out[:, :3, 3] = (s * (r @ poses[:, :3, 3].T)).T + t
    return out


def ate_rmse(est_centres: np.ndarray, gt_centres: np.ndarray) -> tuple[float, float]:
    """Absolute trajectory error after Sim(3) alignment. Returns (rmse, sim3 scale est->gt)."""
    s, r, t = umeyama(est_centres, gt_centres, with_scale=True)
    aligned = (s * (r @ est_centres.T)).T + t
    return float(np.sqrt(((aligned - gt_centres) ** 2).sum(1).mean())), s


def fit_plane_ransac(
    points: np.ndarray,
    threshold: float,
    rng: np.random.Generator,
    iterations: int = 500,
    up_hint: np.ndarray | None = None,
    max_tilt_deg: float = 30.0,
) -> tuple[np.ndarray, float, np.ndarray]:
    """Plane n.x + d = 0 (unit n, oriented towards `up_hint`). Returns (n, d, inlier mask).

    With `up_hint`, candidate planes tilted more than `max_tilt_deg` from it are rejected, so walls
    cannot win over a smaller ground plane.
    """
    n_pts = points.shape[0]
    if n_pts < 3:
        raise ValueError("plane fit needs >= 3 points")
    best_count, best = -1, None
    cos_tol = math.cos(math.radians(max_tilt_deg))
    for _ in range(iterations):
        idx = rng.choice(n_pts, 3, replace=False)
        p0, p1, p2 = points[idx]
        n = np.cross(p1 - p0, p2 - p0)
        norm = np.linalg.norm(n)
        if norm < 1e-12:
            continue
        n /= norm
        if up_hint is not None:
            if n @ up_hint < 0:
                n = -n
            if n @ up_hint < cos_tol:
                continue
        d = -n @ p0
        count = int((np.abs(points @ n + d) < threshold).sum())
        if count > best_count:
            best_count, best = count, (n, d)
    if best is None:
        raise ValueError("no plane candidate satisfied the orientation constraint")
    n, d = best
    inliers = np.abs(points @ n + d) < threshold
    # Least-squares refine on inliers.
    p = points[inliers]
    centroid = p.mean(0)
    _, _, vt = np.linalg.svd(p - centroid)
    n_ref = vt[2]
    if (up_hint is not None and n_ref @ up_hint < 0) or (up_hint is None and n_ref @ n < 0):
        n_ref = -n_ref
    d_ref = -n_ref @ centroid
    inliers = np.abs(points @ n_ref + d_ref) < threshold
    return n_ref, float(d_ref), inliers


def rotation_between(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Smallest rotation taking unit vector a to unit vector b."""
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(a @ b)
    if c < -1 + 1e-9:
        axis = np.cross(a, [1.0, 0.0, 0.0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0.0, 1.0, 0.0])
        axis /= np.linalg.norm(axis)
        return 2.0 * np.outer(axis, axis) - np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx / (1.0 + c)


def angle_deg(a: np.ndarray, b: np.ndarray) -> float:
    c = float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b)))
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def yaw_of(cam2world: np.ndarray) -> float:
    """Heading (rad) of a camera's forward axis projected on the XY plane (Z-up world)."""
    f = cam2world[:3, 2]
    return math.atan2(f[1], f[0])


def chamfer(a: np.ndarray, b: np.ndarray) -> tuple[float, float]:
    """Mean nearest-neighbour distances (a->b accuracy, b->a completeness)."""
    from scipy.spatial import cKDTree

    d_ab, _ = cKDTree(b).query(a, k=1, workers=-1)
    d_ba, _ = cKDTree(a).query(b, k=1, workers=-1)
    return float(d_ab.mean()), float(d_ba.mean())


def backproject(depth: np.ndarray, k: np.ndarray, cam2world: np.ndarray, stride: int = 1) -> np.ndarray:
    h, w = depth.shape
    vs, us = np.mgrid[0:h:stride, 0:w:stride]
    z = depth[::stride, ::stride]
    valid = np.isfinite(z) & (z > 0)
    x = (us[valid] + 0.5 - k[0, 2]) / k[0, 0] * z[valid]
    y = (vs[valid] + 0.5 - k[1, 2]) / k[1, 1] * z[valid]
    pc = np.stack([x, y, z[valid]], 1)
    return (cam2world[:3, :3] @ pc.T).T + cam2world[:3, 3]
