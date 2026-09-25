# WorldExtract: playable world-model video to a FUSE 3D world

WorldExtract drives an open world model with scripted keyboard and mouse input, records the
"playable video" and the exact actions, and reconstructs a 3D world from the video. The output is
FUSE assets: a glTF mesh, a collision mesh, POCO records with provenance, a licence-lock fragment
and a `.fuselevel` stub.

```
generate.py  seed image + preset --> Matrix-Game 2.0 (GPU) --> run.mp4 + run.actions.json + run.gen.json
extract.py   frames -> poses -> depth -> fuse -> [splat] -> collision -> export
             keyframes  SfM or     aligned  TSDF    gsplat    RANSAC      .glb, POCO, manifest,
             + HUD mask MapAnything depth   mesh    (opt.)    ground      licences, .fuselevel
```

Everything here is FOSS under OSI licences, for both code and weights (see the table). Per
REMASTER_PLAN §4.7, world-model geometry is a **blockout**: it enters FUSE as POCO `Mesh`
records with `review.state: pending` and is rebuilt or edited by hand before anything ships.

## Contents

| File | Purpose |
|---|---|
| `generate.py` | Wraps the upstream Matrix-Game 2.0 inference code with reconstruction-friendly presets. `--dry-run` works without a GPU. |
| `extract.py` | Pipeline CLI: `frames`, `poses`, `depth`, `fuse`, `splat`, `collision`, `export`, `all`, `verify`, `download-models`. |
| `worldextract/` | Library: `actions.py` (MG2 action format, presets, pose prior), `geometry.py`, `stages/*.py`. |
| `tests/synthetic_scene.py` | CPU ray-caster that stands in for the world model: MP4, action log, ground-truth poses and depth. |
| `tests/test_synthetic.py` | The `fuse_world_extract_synthetic` ctest (labels `tools;python`). It SKIPs with exit code 77 when deps are missing. |
| `requirements.txt` / `requirements-gpu.txt` | `core` (CPU, the ctest) and `gpu` extras. |

## 1. Model choice

**Default: Matrix-Game 2.0** (Skywork; code and weights MIT; about 1.8B parameters; 25 fps
real time; keyboard and mouse action conditioning). I verified these facts against
[SkyworkAI/Matrix-Game](https://github.com/SkyworkAI/Matrix-Game/tree/main/Matrix-Game-2) and
[Skywork/Matrix-Game-2.0 @ f1729d99](https://huggingface.co/Skywork/Matrix-Game-2.0):

| Fact | Where it is verified | Value |
|---|---|---|
| Entry points | `inference.py` (random bench actions), `inference_streaming.py` (stdin actions) | `generate.py` imports `inference.InteractiveGameInference` for model loading and calls `pipeline.inference()` with its own tensors |
| Resolution | `inference.py` `_resizecrop(352, 640)` | 640x352 |
| Frame count | `--num_output_frames` counts latent frames; `num_frame_per_block: 3` | video frames = (latent − 1) × 4 + 1, latent % 3 == 0 (default 150 latents, 597 frames) |
| Actions: `universal` | `utils/conditions.py`, `pipeline/causal_inference.py` | keyboard `[forward, back, left, right]` (binary), mouse `[pitch, yaw]`, `CAM_VALUE = 0.1` |
| Actions: `gta_drive` | same | keyboard `[forward, back]`, mouse yaw only (no strafe) |
| Actions: `templerun` | same | 7-dim one-hot `[nomove, jump, slide, turnleft, turnright, leftside, rightside]`, no mouse. Reconstruction presets refuse this mode. |
| Checkpoints | HF file list, git-LFS sha256 pinned in `worldextract/actions.py` | `base_distilled_model/base_distill.safetensors` (universal), `gta_distilled_model/…`, `templerun_distilled_model/…`, `Wan2.1_VAE.pth`, CLIP `models_clip_open-clip-xlm-roberta-large-vit-huge-14.pth` |
| VRAM | upstream README: "at least 24 GB (A100 and H100 are tested)" | the 3090 (24 GB) is at the lower limit and **untested upstream** |
| Upstream caveat | README tips | upward camera motion can cause brief black frames. The presets keep the pitch at 0. |

The gains that map mouse units to degrees and a held key to metres per frame are **not
published**. The defaults (15 °/unit, 0.06 m/frame) are assumptions. Run the `calibrate` preset
once, then `extract.py poses`, which fits both gains, and pass `--calibration poses/poses.json`
to later `generate.py` runs.

**Licence note:** the MG2 weights card lists `Skywork/SkyReels-V2-I2V-1.3B-540P` (Skywork
community licence, not OSI) as `base_model`. Skywork holds the rights to both and published the
derived MG2 weights under MIT. The `gta_drive` and `templerun` checkpoints are fine-tuned on
third-party game footage, so WorldExtract marks their outputs `distribution: never` until a human
review clears them. Use `universal`.

**Alternatives considered** (all accessed 2026-09-23):

| Model | Licence (code / weights) | Verdict |
|---|---|---|
| Matrix-Game 3.0 (Mar 2026) | Apache-2.0 / Apache-2.0 (base Wan2.2-TI2V-5B, Apache-2.0) | **Best follow-up.** 720p, camera-aware long-horizon memory, which directly attacks the multi-view consistency problem. Its interactive mode reads the same `i/k/j/l/u` + `w/s/a/d/q` keys per 40-frame iteration. Upstream only tests A/H-series GPUs, and its example uses 5B + int8 with FlashAttention 3, which needs Hopper. `--fa_version 2` exists but is untested on Ampere, so it is not the 3090 default. |
| LingBot-World base-cam | Apache-2.0 / Apache-2.0 | Quality alternative. On 24 GB it needs a community 4-bit quant (e.g. `cahlen/lingbot-world-base-cam-nf4`), so check each quant's licence. |
| `robbyant/lingbot-world-v2-1.3b-causal-fast` | CC-BY-NC-SA-4.0 | **Forbidden** (NC). |
| Genie / Veo | proprietary | Out of scope (REMASTER_PLAN §4.7). |

## 2. Reconstruction stack (and why)

World-model video is not a real camera. Textures "breathe", geometry drifts over tens of
seconds, and exact loop closure is rare. The stack below is chosen for robustness to that,
within the OSI-only rule:

| Stage | Default | Why | Fallback |
|---|---|---|---|
| frames | Laplacian-variance blur rejection, LK-flow parallax keyframing, static-overlay (HUD) detection from temporal variance | Deterministic and cheap. It removes the frames that hurt SfM most. | `--hud x0,y0,x1,y1` manual boxes |
| poses (GPU) | **MapAnything, Apache-2.0 weights `facebook/map-anything-apache`** | Feed-forward multi-view metric reconstruction degrades gracefully when features are inconsistent, and it gives poses, intrinsics and dense depth in one pass. Its memory-efficient mode is built for many views (upstream: up to 2000 views on 140 GB); 80–150 keyframes on 24 GB is an estimate, not a measurement. | `--pose-backend colmap` |
| poses (CPU / fallback) | **COLMAP 4.2 via pycolmap**: SIFT, sequential matching, action-prior loop pairs, incremental mapper (`--mapper global` = GLOMAP-style global SfM, now inside COLMAP) | Bundle-adjusted accuracy. In the synthetic test: ATE 2 cm on a 20 m path. | |
| depth | **MoGe-3 ViT-L** (MIT, 2026-08-18). The SfM focal length goes in as `fov_x`, then per-frame scale is aligned to the SfM points. | Metric, sharp geometry. It is one network per frame, and the alignment keeps it consistent. | `moge2`, `da2-small` (Depth Anything V2 **Small** only), `sfm` (no network) |
| fuse | Open3D TSDF (`VoxelBlockGrid`), connected-component floater removal, quadric decimation, xatlas UVs, best-view texture bake with depth test | Averages away per-frame hallucination. The atlas comes from real frames. | `--texture vertex` |
| splat | gsplat (Apache-2.0), mesh-initialised, DefaultStrategy densification | Optional view-dependent preview | |
| collision | quadric-decimated collision mesh, RANSAC ground plane, slope-classified walkable faces, spawn point | | |

The action log serves as a **pose prior**. It proposes loop-closure image pairs for matching,
fills keyframes that SfM could not register, gives gravity (the model's camera never rolls),
removes residual loop-closure drift (the observed start/end gap minus the gap the actions
predict, spread along the path's arc length), and calibrates the action gains. Scale is
arbitrary in monocular video: by default the median camera height above the RANSAC ground is
set to `--eye-height` (1.7 m). `--scale-mode action-speed` uses the gains instead.

**Excluded after a licence check** (weights are not OSI): VGGT (`facebook/VGGT-1B`
CC-BY-NC-4.0; `VGGT-1B-Commercial` uses a custom "VGGT licence"), MASt3R / DUSt3R
(CC-BY-NC-SA), Apple Depth Pro (Apple AMLR), `facebook/map-anything` (CC-BY-NC-4.0: only the
`-apache` variant is allowed), Depth Anything V2 Base/Large and Depth Anything 3
Large/Giant/Nested (CC-BY-NC-4.0), and the original INRIA 3DGS code (non-commercial).
**Allowed but not integrated:** Depth Anything 3 Small/Base/Metric-Large (Apache-2.0) and π³
(Pi3, BSD-2-Clause code and weights). Both are candidates for a second feed-forward backend.

## 3. Licence table (every component; all accessed 2026-09-23)

| Component | Role | Code licence | Weights licence | Source |
|---|---|---|---|---|
| Matrix-Game 2.0 | world model | MIT | MIT (base: SkyReels-V2, Skywork licence; relicensed by the rights holder) | https://github.com/SkyworkAI/Matrix-Game · https://huggingface.co/Skywork/Matrix-Game-2.0 |
| Wan2.1 VAE (bundled in the MG2 repo) | MG2 VAE | Apache-2.0 | Apache-2.0 | https://huggingface.co/Wan-AI/Wan2.1-I2V-14B-480P |
| OpenCLIP XLM-R ViT-H/14 (bundled) | MG2 image encoder | MIT | MIT | https://huggingface.co/laion/CLIP-ViT-H-14-frozen-xlm-roberta-large-laion5B-s13B-b90k |
| Matrix-Game 3.0 (follow-up) | world model | Apache-2.0 | Apache-2.0 | https://github.com/SkyworkAI/Matrix-Game/tree/main/Matrix-Game-3 · https://huggingface.co/Skywork/Matrix-Game-3.0 |
| LingBot-World base-cam (alt.) | world model | Apache-2.0 | Apache-2.0 | https://github.com/Robbyant/lingbot-world · https://huggingface.co/robbyant/lingbot-world-base-cam |
| COLMAP 4.2 / pycolmap | SfM, GLOMAP global mapper | BSD-3-Clause | – | https://github.com/colmap/colmap/blob/main/LICENSE · https://pypi.org/project/pycolmap/ |
| GLOMAP (standalone, historical) | global SfM | BSD-3-Clause | – | https://github.com/colmap/glomap/blob/main/LICENSE |
| MapAnything | poses + depth (GPU) | Apache-2.0 | Apache-2.0 (`facebook/map-anything-apache` **only**) | https://github.com/facebookresearch/map-anything · https://huggingface.co/facebook/map-anything-apache |
| MoGe-3 / MoGe-2 | monocular geometry | MIT (DINOv2 parts Apache-2.0) | MIT (`Ruicheng/moge-3-vitl`, `moge-2-vitl-normal`) | https://github.com/microsoft/MoGe · https://huggingface.co/Ruicheng/moge-3-vitl |
| FlexGEMM (MoGe-3 dependency) | sparse conv kernels | MIT | – | https://github.com/JeffreyXiang/FlexGEMM |
| Depth Anything V2 **Small** | fallback depth | Apache-2.0 | Apache-2.0 (Small **only**) | https://github.com/DepthAnything/Depth-Anything-V2 · https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf |
| gsplat | Gaussian splats | Apache-2.0 | – | https://github.com/nerfstudio-project/gsplat |
| Open3D | TSDF, mesh processing | MIT | – | https://github.com/isl-org/Open3D |
| xatlas (python) | UV atlas | MIT | – | https://pypi.org/project/xatlas/ |
| OpenCV (opencv-python-headless) | video IO, features, imaging | Apache-2.0 | – | https://github.com/opencv/opencv-python |
| NumPy / SciPy | numerics | BSD-3-Clause | – | https://numpy.org · https://scipy.org |
| pygltflib | glTF validation (test only) | MIT | – | https://gitlab.com/dodgyville/pygltflib |
| PyTorch / torchvision | GPU runtime | BSD-3-Clause | – | https://github.com/pytorch/pytorch |
| transformers / huggingface_hub | DA-V2 loader, `download-models` | Apache-2.0 | – | https://github.com/huggingface/transformers |
| meshoptimizer | not used: Open3D quadric decimation covers simplification. `fuse_cook` generates LODs downstream. | MIT | – | https://github.com/zeux/meshoptimizer |

**Output licence:** `LicenseRef-FUSE-Generated` with an `ai` block (model, model licence MIT,
seed, recipe hash). The MIT licence places no restriction on generated output. Seed images marked
`--seed-image-origin original` make the outputs `recipe-only` (REMASTER_PLAN §0.1).

## 4. RTX 3090 (24 GB) instructions

Keep weights, caches and outputs **outside the repo** (for example `~/models`, `~/.cache`, or
`build/worldextract/`, which git ignores).

```bash
# A. Matrix-Game 2.0 environment (upstream instructions; Python 3.10, CUDA 12.x, FlashAttention 2)
git clone https://github.com/SkyworkAI/Matrix-Game.git ~/src/Matrix-Game
conda create -n mg2 python=3.10 -y && conda activate mg2
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu124
pip install flash-attn --no-build-isolation          # FA2 supports Ampere (sm_86)
cd ~/src/Matrix-Game/Matrix-Game-2 && pip install -r requirements.txt && python setup.py develop
huggingface-cli download Skywork/Matrix-Game-2.0 --revision f1729d99a80e0f07993a77d7dad4a3190e23c2c8 \
    --local-dir ~/models/Matrix-Game-2.0 --include "base_distilled_model/*" "Wan2.1_VAE.pth" \
    "models_clip_open-clip-xlm-roberta-large-vit-huge-14.pth" "xlm-roberta-large/*"   # ~11.8 GB (universal only; all modes ~28 GB)
pip install -r /path/to/FUSE/Tools/FUSE/WorldExtract/requirements.txt   # opencv etc. for generate.py

# B. Generate (optionally calibrate the gains once first)
cd /path/to/FUSE/Tools/FUSE/WorldExtract
python generate.py --seed-image my_concept.png --preset calibrate --out ~/wx/cal \
    --mg-repo ~/src/Matrix-Game/Matrix-Game-2 --weights ~/models/Matrix-Game-2.0
python extract.py frames --work ~/wx/cal/work --video ~/wx/cal/calibrate_s42.mp4
python extract.py poses  --work ~/wx/cal/work --actions ~/wx/cal/calibrate_s42.actions.json --pose-backend colmap
python generate.py --seed-image my_concept.png --preset walkthrough --out ~/wx/run \
    --calibration ~/wx/cal/work/poses/poses.json --mg-repo ~/src/Matrix-Game/Matrix-Game-2 --weights ~/models/Matrix-Game-2.0

# C. Extraction environment (can be the same env)
pip install -r requirements-gpu.txt
python extract.py download-models --what moge3,mapanything   # the only networked step
python extract.py all --work ~/wx/run/work --video ~/wx/run/walkthrough_s42.mp4 \
    --actions ~/wx/run/walkthrough_s42.actions.json --gen-manifest ~/wx/run/walkthrough_s42.gen.json --name my_area
```

Presets (`--preset`): `walkthrough` (racetrack: a weaving forward dolly with zero-net strafe,
then a 180° arc, the same leg back, and another 180° arc back to the start pose); `orbit`
(circle-strafe 360° around a point ahead); `strafe_scan` (lawnmower strafe rows that return via
a racetrack); `loop` (360° circle while walking); and `calibrate`. All of them turn at most
`--max-turn-deg` per frame and never rotate in place. `--speed` sets the movement duty cycle.

**Expected VRAM and time per stage on a 3090.** These are estimates from the upstream figures.
Nothing on this list was measured on a GPU here.

| Stage | VRAM | Time (597-frame run) | Basis |
|---|---|---|---|
| generate (MG2 universal) | ~20–24 GB peak; `generate.py` moves the VAE encoder + CLIP to the CPU after encoding (`--keep-encoder-on-gpu` disables this) | model load + `torch.compile` 3–8 min the first time; generation ≈ 1–3 min (upstream: 25 fps on H100; a 3090 has ~1/3–1/2 of the throughput) | upstream README; UNVERIFIED |
| frames | CPU | ~10–20 s | measured scaling from the CPU test |
| poses: MapAnything | ~6–14 GB for 80–150 keyframes (memory-efficient mode) | < 1 min | upstream profiling plots; UNVERIFIED |
| poses: COLMAP (CPU) | – | 1–5 min for 100–150 keyframes at 640x352 | measured: 76 keyframes at 384x212 ≈ 20 s on 4 loaded cores |
| depth: MoGe-3 ViT-L | ~4–6 GB | ~0.1–0.2 s/frame (MoGe states 60 ms/image for ViT-L FP16 on a 3090 for v1/v2) | upstream; UNVERIFIED for v3 |
| fuse | CPU, 1–3 GB RAM | 30 s – 3 min (200k tris, 2048² atlas; xatlas dominates) | measured scaling |
| splat (7k iters) | ~4–8 GB | 5–10 min | gsplat typical; UNVERIFIED |
| collision + export | CPU | seconds | measured |

`generate.py` checks CUDA, bf16 support (sm_80+), total VRAM (≥ 22 GB) and free VRAM before it
loads anything. It gives a clear error, for example "close other GPU processes". It also pins
and verifies the checkpoint sha256 (the hash is cached next to the weights).

## 5. How outputs feed the FUSE asset pipeline

`<work>/export/` contains:

- `world.glb` (POSITION, NORMAL, TEXCOORD_0, COLOR_0, baked albedo atlas; glTF +Y up, metres) and
  `collision.glb`. **Both cook with the real cooker**: `fuse_cook --mesh --input world.glb
  --output world.fusemesh`, which the ctest runs when `fuse_cook` is built.
- `poco/<kind>/<poco_id>.poco.json` + `blobs/sha256/<ab>/<sha256>`, per REMASTER_PLAN §2.1–2.3.
  Kinds: `mesh` (`mesh/worldextract/<name>`, plus `_collision`), `material`, `texture_set`,
  `level`. Streams are little-endian `Position F32x3`, `Normal F32x3`, `Uv0 F32x2`,
  `Color0 Unorm8x4` and `indices32` in canonical +Z up metres. The header carries
  `provenance.ai` (model, revision, weights sha256, seed, action-log sha256, video sha256,
  reconstruction backends), `distribution`, tags `blockout`, and `review.state: pending`.
- `licences.fragment.json`: records in the ASSET_PLAN §2.4 shape (`origin: generated`,
  generator path + git revision, seed, `LicenseRef-FUSE-Generated`, `ai` block,
  `review.required: true`). Merge them into `Content/licences.lock.json` after review.
- `world.fuselevel`: a `SceneSerialiser` v1 file (magic `FUSE`, camera block, entity names +
  transform table; +Y up in the runtime `Camera` yaw/pitch convention). It holds entities
  `WorldMesh`, `WorldCollision`, `PlayerSpawn`, a `__fuse.wire|material|WorldMesh|<mat id>`
  stub and a `__fuse.wire|datablock|PlayerSpawn|PlayerSpawn` stub (the same wiring convention
  `world_converter.cpp` emits). **Follow-up:** v1 has no field for a mesh asset reference, so
  the geometry binds through the POCO `level` record until `mesh_cook.cpp` gains the POCO
  reader (REMASTER_PLAN §2.4).
- `worldextract.manifest.json`: every file with its sha256 and size, provenance, units and scale
  note, stage metrics. `extract.py verify --work <dir>` re-hashes everything.

Units: the POCO/blob data is +Z up metres; the glb and `.fuselevel` are +Y up. The absolute
scale comes from the eye-height assumption (treat it as ±20% until it is checked in the editor).

## 6. Limitations and mitigations

| Limitation | Effect | Mitigation in WorldExtract / next step |
|---|---|---|
| World-model video is not multi-view consistent over long spans (MG2 attends to a short latent window) | The same place looks different on revisits, loops fail to close, and duplicate "ghost" surfaces appear | Short, slow, parallax-rich presets; loop-closure pairs from the action prior; drift correction from the action-predicted closure gap; TSDF averaging and floater removal. Next: Matrix-Game 3.0 (camera-aware memory). |
| Geometry hallucination and drift | Plausible but wrong walls and depth; slow scale/yaw drift | Per-frame depth is aligned to multi-view SfM points, not trusted alone; a temporal median over alignment scales; the calibrated gains expose drift (`poses.json::calibration`, `loop_closure`). |
| Dynamic objects (people, cars, foliage, water) | Floaters and smeared surfaces | Connected-component floater removal. Next: per-frame motion masks. The HUD mask path already accepts masks. |
| No true scale | Size is ±20% | Eye-height normalisation (`--eye-height`), or `--scale-mode action-speed` after calibration. Check against a known prop in the editor. |
| Unknown intrinsics | Focal errors bend geometry | COLMAP refines focal (test: 0.03% error from a 5°-wrong hint); MapAnything and MoGe estimate it. |
| Upward camera glitches (upstream tip) | Black frames | Presets keep pitch at 0; blur/keyframe filters drop black frames. |
| Thin or far structures | Missing or fragmented at grazing angles | Depth truncation at 1.25 × the 95th percentile of sparse depth; lower `--voxel` for close-range captures. |
| Determinism | COLMAP multithreaded BA is not bit-exact across runs | Seeds are fixed everywhere. `--threads 1` makes COLMAP deterministic (slower). |

## 7. CPU test (no world model needed)

```bash
python -m venv /tmp/wx && /tmp/wx/bin/pip install -r requirements.txt
/tmp/wx/bin/python tests/test_synthetic.py --work /tmp/wx-test --keep
# or via CMake: -DFUSE_WORLDEXTRACT_PYTHON=/tmp/wx/bin/python, then  ctest -R fuse_world_extract_synthetic
```

The test renders a textured procedural scene (ground, 10 boxes, 4 pillars, a fake static HUD)
along the `walkthrough` action log produced by `generate.py --dry-run`. It then runs
`extract.py all --cpu` with ground-truth depth aligned to the SfM points, reruns it to check
that every stage is cached, runs the SfM-only depth branch, self-tests loop-closure drift
correction, and cooks the glbs with `fuse_cook`. Results of `ctest -R fuse_world_extract_synthetic`
on 2026-09-23 (4 cores under heavy load from other jobs, load average ~11). Repeat runs differ by a few millimetres and tenths of a degree, because COLMAP's multithreaded BA is not bit-exact:

| Metric | Measured | Threshold |
|---|---|---|
| Keyframes registered | 76 / 76 | – |
| Pose ATE after Sim(3) | 0.022 m (0.11% of the 19.8 m path) | ≤ 0.10 m and ≤ 1% |
| Mean rotation error | 0.29° | ≤ 1° |
| Scale error (eye-height normalisation) | 1.1% | ≤ 5% |
| Focal error (from a 5°-wrong FOV hint) | 0.02% | ≤ 3% |
| Gain calibration error (deg/unit, m/frame) | 0.3%, 1.1% | ≤ 5%, ≤ 8% |
| Chamfer mean, ground-truth depth branch (accuracy / completeness) | 0.050 m (0.037 / 0.064) | ≤ 0.10 m |
| F-score @ 10 cm, ground-truth depth branch | 0.95 (P 0.98, R 0.93) | ≥ 0.85 |
| Ground-plane normal error | 0.14° | ≤ 2° |
| SfM-only depth branch: Chamfer, F-score | 0.175 m, 0.70 | ≤ 0.25 m, ≥ 0.55 |
| Loop-closure self-test (0.8 m + 5° injected drift) | mean position error 0.46 → 0.07 m | residual ≤ 30% |
| HUD detection | box [12,164,92,206] (drawn HUD: [11,165,87,204]) | detected |
| Resume run | 6/6 stages cached, 1.8 s | all cached |
| `fuse_cook --mesh` on world.glb and collision.glb | ok | exit 0 |
| Wall time | 111 s | ctest timeout 600 s |

## 8. Follow-ups

- Run everything on the 3090: `generate.py` (VRAM headroom, fps, action gains via `calibrate`),
  the MapAnything / MoGe-3 / gsplat paths. They are written against the upstream APIs but never
  executed here.
- A Matrix-Game 3.0 backend (Apache-2.0, camera-aware memory) by piping per-iteration keys to its
  `--interactive` mode.
- A POCO mesh reader in `mesh_cook.cpp`, and a `.fuselevel` asset-reference field, so the level
  stub binds geometry without the glTF detour.
- Dynamic-object masks, and a second FOSS feed-forward pose backend (π³ BSD-2 or Depth Anything 3
  Small/Base Apache-2.0).
