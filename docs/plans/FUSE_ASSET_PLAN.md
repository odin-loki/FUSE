# FUSE Asset Plan: realistic but lightweight content for the whole planet and for fantasy

Status: plan, not yet started. Author date 2026-09-23. Companion to
[`FUSE_RENDERER_PLAN.md`](FUSE_RENDERER_PLAN.md) and [`FUSE_MASTER_PLAN.md`](FUSE_MASTER_PLAN.md).

**Goal.** Build a content library that looks as close to photographic as FUSE's renderer allows and
still stays small on disk, in VRAM and in cook time. It should cover every major Earth biome, built
environments from ancient to modern on every inhabited continent, and the main fantasy and
speculative genres. Agents in this repository's environment must be able to carry out the plan:
a Linux container with no GPU (CPU and Lavapipe only), Python and C++ available, internet through a
proxy, no paid DCC software and no photogrammetry rig. Everything is therefore **procedural first**,
**sourced second** and only from CC0 or permissively licensed libraries, with a licence manifest the
build can check.

**The compromise in one sentence.** Spend bytes on *shared, tileable, layered* surface detail and on
*silhouettes near the camera*, and use procedural functions, instancing, masks, decals and impostors
everywhere else. Unique texture per object, unique scan per rock and full-resolution everything are
out.

---

## 0. What exists in the repository today (survey, 2026-09-23)

The plan builds on these pieces and names the gaps it must close first (Wave 0).

| Area | What exists | Gap the asset plan needs closed |
|---|---|---|
| Mesh cook | `Tools/FUSE/Cook/src/mesh_cook.cpp`: assimp import → `.fusemesh` (`FMSH` v1): positions, normals, uv0, 32-bit indices, submeshes, bounds, FNV-1a trailer. Strict validation, no repair. | No tangents, no uv1 (lightmap/detail), no vertex colour, no skin joints/weights, no LOD or meshlet/cluster data, no quantisation. Needs `FMSH` v2 (§5.1). |
| Texture cook | `texture_cook.cpp` + `bc7_encoder.cpp` (BC7 mode 6 only), optional `ispc_texcomp_hook.cpp` (BC7/BC5), `.fusetex` text header + blocks; max 16384². `TextureImportDesc` already lists `None/BC1/BC3/BC4/BC5/BC7`, sRGB/linear, `is_normal_map`, `is_hdr`. `asset_cooker.cpp` picks BC5 for normal maps, BC7 otherwise. | `cook_stub_writer.cpp` comments that BC5/BC1 are not implemented (everything goes to BC7 mode 6). No BC6H, no BC4, no texture arrays, no KTX2 container, no supercompression. |
| Cook orchestration | `Source/FUSE/Project`: `cook_manifest`, `cook_cache`, `cook_content_hash`, `cook_dependency_graph`, `cook_job_graph`, `asset_graph`, `import_pipeline`; CLI `fuse_cook --manifest/--project/--mesh/--texture/--audio/--fuselevel`, `--strict` default, `--lenient` placeholders. | No asset kinds for material, vegetation, impostor, SDF volume, animation clip, look or biome pack. No licence metadata in the manifest. |
| Materials | `renderer/material/material.hpp`: shading models `Opaque, Translucent, Emissive, SubsurfaceSSS, ClearCoat, Cloth`; flags for normal, AO and metallic maps; bindless (`vk/bindless.hpp`). `procedural_materials.hpp`: analytic `Wood, Metal, Concrete` on world-space position with hashed-lattice Perlin and fBm (no tiling, no UVs). | No layered/blend material, no detail-texture slot, no height-blend, no triplanar flag, no thin-translucency foliage model, no anisotropy (hair), no iridescence (thin-film). Procedural IDs 0..32767 are available (15 bits). |
| Terrain | `Source/FUSE/Terrain`: seeded fractal value-noise heightfield, clipmap LOD chunk grid, crack-free seams, deformation, SVO caves (`fuse_scene` `svo.hpp`), gates up to 4096². | No DEM ingest, no erosion, no biome/splat masks, no material layers on terrain, no scatter. |
| SDF | `ecs/components/sdf_object.hpp`, `ecs/sdf_csg.hpp`, `renderer/shadow/sdf_*`. | No baked SDF volume asset format for meshes (rocks, props). |
| Look | `renderer/look/look_params.hpp`, `effect_graph.hpp`, `.fuselook` schema (AO, DoF, bloom, exposure, tone map, colour grade + 3D LUT, grain…). `Samples/Looks/luts/` (empty). | No per-biome or per-weather look presets, no LUT content. |
| GI / lighting | `gi/ddgi.hpp`, clustered lighting, `atmosphere/`, `volumetric/volumetric_fog.hpp`. | Material albedo must stay within physically plausible ranges or DDGI bounce looks wrong (validation gate §5.3). |
| Animation | `Source/FUSE/Animation`: skeleton, clip, animator, blend tree, IK solver, retarget, skinning (CPU/GPU kernel). | No clip importer (BVH/FBX/glTF animation) into the cooked format, no skinned mesh stream in `FMSH`. |
| VFX | `Source/FUSE/VFX`: particle emitter/system, GPU sim kernel, SoA ops. | No flipbook texture asset, no motion-vector flipbooks. |
| Audio | `Source/FUSE/Audio`: clip, bus, spatial mixer (HRTF-lite binaural pan), CPU convolution reverb (+ CUDA path), reverb zones, occlusion. `AudioImportDesc` targets 48 kHz, Ogg Vorbis. | No ambience-bed/one-shot bank format, no IR library, no procedural synth. |
| Lint / pins | `Tools/FUSE/Lint/fuse_lint.cpp` with `vendored-pins` mode; each `Engine/lib/<dep>/VERSION` pins upstream, tag, commit and file sha256s (ctest `fuse_lint_vendored_pins_*`). | Needs a matching `asset-licences` mode (§2.4). |
| Samples | `Samples/unification/demo_*` (2D, 3D-empty, adventure stub, AI BT, FX, HUD, timeline), `Samples/Looks`. | No reference scenes for golden renders of content (§5.4). |

Renderer features the plan relies on (from `FUSE_RENDERER_PLAN.md`): visibility buffer, GPU-driven
culling, meshlets and virtual geometry (Phase 1/5, meshoptimizer), virtual shadow maps (Phase 3),
DDGI (Phase 6), volumetrics and atmosphere (Phase 8), neural techniques as optional T3 (Phase 9).
Content is authored so that it works on T0 (no mesh shaders, discrete LODs plus impostors) and scales
up on T1+ (cluster DAG).

---

## 1. Principles of the realism / weight compromise

### 1.1 Where realism comes from (in order of payoff per byte)

1. **Lighting and atmosphere** (engine, zero asset bytes): physically based sky, aerial perspective,
   DDGI, volumetric fog, correct exposure. A plain scene with good light looks more real than a
   detailed scene with bad light. Assets must therefore be *physically calibrated* (albedo range,
   roughness distribution, real-world scale) so the engine's lighting works on them.
2. **Correct scale and silhouettes** near the camera: real-world metres, plausible proportions,
   bevelled edges (no knife edges), no floating objects.
3. **Material response**: roughness variation, micro-normal detail, height-blended layering (dust in
   crevices, moss on top faces, wetness), correct metals versus dielectrics.
4. **Variation and breakup** at every scale: macro (biome masks, colour variation maps), meso (decals,
   vertex-colour masks, instance random), micro (detail textures). Repetition is the main thing that
   gives away a lightweight scene.
5. **Unique high-resolution texture data**: comes last, and is used only for hero assets.

### 1.2 Budget tables

Numbers are for the **Standard** quality tier (1440p target, T1/T2 GPU). A **Lite** tier drops one
texture mip level (¼ memory) and uses the next coarser LOD bias. A **Showcase** tier allows one mip
more for hero assets only. Triangle counts are for LOD0 *source*; with the T1+ cluster DAG, source
counts can go ×2–4 higher for rocks and architecture, because the DAG streams and culls clusters
(the storage budget in §6.9 still applies).

| Asset class | LOD0 triangles (T0 / DAG source) | LODs | Textures (per unique set) | Texel density at LOD0 | Notes |
|---|---|---|---|---|---|
| Hero character (player, main NPC) | 40–70k / 120k | 4 + DAG | 2048² body + 1024² head detail, 1024² hair strands card atlas | 20.48 px/cm (2048 per m) on face, 10.24 body | Skin SSS, hair anisotropic cards; morph targets for face |
| Crowd human / NPC | 8–15k | 3 + impostor | Shared 1024² atlas from a parametric generator; colour by mask | 5.12 px/cm | 1 atlas serves many variants (tint masks) |
| Large creature (dragon, elephant) | 60–100k / 250k | 4 + DAG | 2048² body + tiled scale/skin detail texture | 5.12 px/cm + detail | Detail textures carry scale/skin pores |
| Small fauna (birds, fish, insects) | 0.3–3k | 2 + impostor/particle | 256–512² atlas shared per family | n/a | Flocks via instancing/VFX |
| Tree (hero/near) | 20–60k trunk + cards | 3 + octahedral impostor | Bark: tileable 1024² (shared per genus); leaves: 2048² atlas per 3–5 species | 5.12 px/cm bark | Wind by vertex colour + pivot data |
| Shrub / bush | 2–8k | 2 + impostor | Shares leaf atlas | n/a | |
| Grass / ground cover | 4–40 per blade clump | 1–2 + fade | 512–1024² atlas per biome | n/a | GPU-instanced, density maps |
| Rock / boulder | 2–10k / 50k (DAG) | 3 + SDF | **No unique texture**: triplanar tileable rock material + baked tangent normal 512–1024² + vertex AO | 5.12 px/cm via tiling | 5–10 base shapes per biome × scale/rotation/material variation |
| Cliff module | 10–30k / 200k (DAG) | 4 | Tileable cliff material + 1024² macro normal | Tiled | Snaps to terrain, blends via height |
| Modular architecture piece | 0.5–5k / 20k | 3 | **Trim sheet** 2048² per style + tileable wall materials | 5.12 px/cm (trim), 2.56 for tiles | Decals for dirt, cracks, signage |
| Hero building (landmark) | 30–100k / 500k | 4 + DAG | Trim sheet + 1–2 unique 2048² sets | 5.12 px/cm | |
| Small prop (crate, pot, chair) | 0.2–3k | 2–3 | Share a 2048² props atlas per kit (8–16 props) | 5.12 px/cm | |
| Hand-held item / weapon | 3–10k | 2 | 1024² | 10.24 px/cm | Visible in first person |
| Vehicle | 20–60k | 4 | 2048² + trim | 5.12 px/cm | |
| Terrain | Clipmap (engine) | Clipmap | 6–12 tileable layers per biome at 1024–2048², + macro colour/normal from DEM | 2.56–5.12 px/cm via tiling | Splat via biome masks |
| Decal | 2–8 (quad) | – | Decal atlas 2048² per kit (BC7 + BC5) | 5.12 px/cm | Deferred/VB decals |
| VFX flipbook | – | – | 8×8 frames of 128–256² in 1024–2048² atlas | – | Motion vectors for frame blending |
| HDRI sky (reference/backdrop) | – | – | 4k equirect BC6H for reflection capture only; engine atmosphere renders the sky | – | 2k for Lite |
| Audio ambience bed | – | – | 48 kHz stereo Vorbis q4, 60–120 s seamless loop | – | ~1 MB/min |
| Audio one-shot | – | – | 48 kHz mono Vorbis q5, 0.2–4 s, 4–8 round-robin variants | – | |

**Texel density rule.** Default world texel density is **512 px/m (5.12 px/cm)** at LOD0 for
everything the player walks next to, 1024 px/m for anything held or seen within 1 m, and
256 px/m for building facades above 4 m. Validation measures it per UV island (§5.3).

### 1.3 Texture compression formats

| Map | Format | Bits/px | Colour space | Rationale |
|---|---|---|---|---|
| Base colour (+ opacity) | BC7 | 8 | sRGB | Best quality LDR; alpha for cutout leaves |
| Base colour, low priority (terrain far, crowd tints) | BC1 | 4 | sRGB | Half the size; no alpha |
| Tangent normal | BC5 (RG, Z reconstructed) | 8 | Linear | Standard; never BC7/BC1 for normals |
| ORM (occlusion, roughness, metal) | BC7 or BC1 | 8 / 4 | Linear | BC1 when the metal channel is constant |
| Height / single masks | BC4 | 4 | Linear | Height-blend, parallax, biome weight |
| Emissive HDR, HDRIs, sky captures, light-probe bakes | BC6H (UF16) | 8 | Linear HDR | |
| Distribution / download | KTX2 with Basis Universal UASTC (LDR 4×4, HDR 4×4) + Zstandard supercompression, or KTX2 BCn + zstd | varies | as above | Transcoded to BCn at install/cook time |

- **KTX2 / Basis Universal**: UASTC HDR 4×4 is a subset of ASTC HDR that transcodes to BC6H with
  "very little loss (typically a fraction of a dB PSNR)"; UASTC HDR 6×6 and RDO modes were added in
  2025 [S13, S14]. KTX-Software (Apache-2.0 for its own files) provides `ktx create` with an explicit
  Vulkan `--format` argument [S15]. FUSE usage: store *source* textures in the asset cache as
  PNG/EXR; store *distributed* packs as KTX2 (UASTC + zstd, or BC7/BC5 + zstd); the cook writes
  `.fusetex` (BCn) for the runtime. The existing `.fusetex` format stays the runtime format; KTX2 is
  only a transport format.
- **RDO (rate-distortion optimisation)** for BC7/UASTC before zstd gives 30–50 % smaller
  downloads for small quality loss; use it on everything except hero normals.
- **Virtual texturing** (sparse residency / software page tables) is *not* needed for tiled
  materials. Use it only for (a) terrain macro colour/normal from DEMs over large worlds and (b) the
  optional runtime virtual texture that caches terrain splat blends. It belongs to Renderer Phase 8+
  and the asset side must only provide tile-friendly source data (≤ 16k² per tile, power of two, with
  borders).
- **Neural texture compression (optional, T3)**: NVIDIA's RTX Neural Texture Compression SDK is
  public on GitHub [S16], but it targets RTX hardware and cannot be validated here (no GPU). Treat
  it as an *optional runtime transcoding target* with BCn fallback. Never make it the only stored
  form. Check its licence before vendoring anything.

### 1.4 Geometry: LOD chains, clusters, impostors, SDFs

- **Discrete LODs (T0)**: meshoptimizer `meshopt_simplify` (with attribute weights and
  `LockBorder` for modular pieces) at target ratios 1, 0.5, 0.25, 0.1, keeping the screen-space error
  under 1 px at switch distances computed from bounds.
- **Cluster DAG (T1+)**: meshoptimizer v1.0 ships `clusterlod.h`, a single-header Nanite-style
  hierarchy builder (group → simplify → re-cluster), plus `meshopt_partitionClusters` and DAG BVH
  construction [S17, S18]. The cook stores meshlets of ≤ 64 vertices / 124 triangles (renderer plan
  Phase 1) and the DAG in `FMSH` v2.
- **Impostors**: octahedral (or hemi-octahedral for ground-bound objects) impostors, a technique
  popularised by Ryan Brucks in 2018 [S19]. Bake an 8×8 (trees) or 6×6 (rocks/buildings) view grid of
  albedo+alpha, normal+depth into a 2048² atlas (256² per view) or 1024² for small objects, using the
  FUSE renderer on Lavapipe or a CPU path tracer. Impostors replace everything past about 150–300 m
  and every tree past about 80 m.
- **SDF representations**: bake a mesh distance field (32³–64³ sparse brick, BC4/R16F) for every
  rock, cliff and building. It serves soft shadows (`sdf_soft_shadow.hpp`), DDGI ray queries, AO,
  collision proxies and far-LOD ray-marched rocks. Hero rocks can be *authored* as SDF CSG trees
  (`ecs/sdf_csg.hpp`) and meshed only for the cook.

### 1.5 Surfaces: layering instead of unique maps

- **Material layering**: every surface is `base material (tileable) × macro variation (world-space
  noise / vertex colour) + up to 3 height-blended layers (moss, dust/sand, snow, wetness) + detail
  normal`. The layers are shared across biomes. This one change removes most unique textures.
- **Detail textures**: a small set of 256–512² tileable detail normal/roughness maps (stone grain,
  wood fibre, fabric weave, skin pores, scales, rust pitting, plaster) sampled at 4–16× frequency and
  faded with distance. About 24 detail maps cover the whole library.
- **Trim sheets**: one 2048² trim sheet per architectural style (mouldings, planks, beams, window
  frames, tiles, metal strips) + 2–4 tileable wall/floor materials. Modular meshes map their UVs onto
  the sheet, so hundreds of pieces share one texture set.
- **Decals**: cracks, leaks, stains, graffiti, moss patches, footpaths, blood, scorch; one decal atlas
  per theme.
- **Vertex-colour masks**: R = layer blend (moss/dirt), G = AO/cavity, B = wetness/snow, A =
  per-vertex wind weight or variation seed. `FMSH` v2 must carry this stream (§5.1).
- **Procedural and tileable first**: the analytic path in `procedural_materials.hpp` (no UVs, no
  tiling, infinite resolution) extends to rock, sand, snow, soil, plaster, brick, ice and lava. Every
  procedural material also has a *baked* tileable version (Wave 1) for T0 and to keep the cost
  bounded.
- **Instancing**: vegetation, rocks, debris and crowds are always instanced, with per-instance
  random (hue shift ±5 %, scale ±20 %, rotation, wetness) to break up repetition.

### 1.6 Physically based calibration rules (enforced by validation)

| Quantity | Rule |
|---|---|
| Albedo (sRGB, dielectrics) | 30–240 (≈ 0.02–0.9 linear); fresh snow ≤ 0.9, charcoal ≥ 0.03 |
| Metal albedo | 180–255 sRGB (specular colour); metal mask binary except at transitions |
| Roughness | full 0–1 range but *no* constant 0.5 maps; standard deviation ≥ 0.03 per material |
| Normal maps | unit length after decode, OpenGL (+Y) convention, BC5, linear, no baked lighting |
| Scale | 1 unit = 1 m; the validator checks bounds against category ranges (door 2.0–2.4 m high, etc.) |
| Base colour | no baked AO or lighting in albedo (checks low-frequency luminance correlation with the AO map) |

---

## 2. Sources and licences

### 2.1 Policy

1. **Allowed without review**: CC0 1.0, public domain (US federal works), Unlicense, and FUSE's own
   generated output.
2. **Allowed with attribution tracking**: CC-BY 4.0 (and 3.0), ODC-BY, the Copernicus DEM licence,
   OGA-BY. Attribution lines go into the generated `CREDITS.md` and the in-game credits.
3. **Only after human review**: CC-BY-SA, ODbL (OpenStreetMap), GPL-licensed *art*, custom "CC0 with
   restrictions". These have share-alike, database or redistribution clauses (§2.3).
4. **Forbidden**: NC (non-commercial), ND (no derivatives), "editorial use", "royalty-free with
   seat licence" marketplace assets, anything with unclear provenance, and AI-generated assets whose
   training data or terms of service forbid commercial use.
5. **Tools versus outputs**: GPL *tools* (Blender, Sverchok, Sapling, MPFB's code) are fine to run;
   their *output* belongs to the user unless the tool embeds licensed assets in it. MPFB's bundled
   assets are CC0 (below).

### 2.2 Source catalogue

| Source | Content | Licence (verified 2026-09-23) | Use in FUSE | Caveats |
|---|---|---|---|---|
| Poly Haven | HDRIs, scanned PBR textures, models | CC0: commercial use, no attribution, redistribution allowed [S1] | Calibration references, HDRIs for reflection captures, a *small* set of hero scans (rock, bark, soil, plaster) used as exemplars for texture synthesis | **The public API** is free for non-commercial use, requires a unique User-Agent, and asks for a "Powered by Poly Haven" credit if you build on it [S20]. Fetch a pinned list once into the asset cache and record sha256s; do not call the API at runtime. |
| ambientCG | PBR materials, HDRIs, some models | CC0 1.0, including raw files in games [S2] | Main tileable material seed library (≈ 100 materials to start) | Downsample 4k/8k sources to 2k before cook. |
| ShareTextures | Textures, models | "Custom CC0": free for commercial use, **but no redistribution** in collections without written permission [S3] | Reference only, or cooked-only use | Because FUSE's asset cache could be seen as redistribution, class as *review-required* and keep source files out of any published pack. |
| Kenney | Low-poly kits, UI, audio | CC0 [S4, S5] | Blockout meshes, UI, placeholder audio, WFC tile prototypes | Stylised. Use for gameplay blockouts, not final realism. |
| Quaternius | Low-poly characters, animals, nature, animations | CC0 [S5, S6] | Rigged animal/creature *base topology* and animation prototypes; stylised fantasy set | Stylised. Upgrade through detail textures or re-sculpt. |
| Smithsonian Open Access | ≈ 2,000+ 3D scans (OBJ/glTF) + images | CC0 where marked "CC0" [S7, S8] | Cultural artefacts, fossils, historic objects: museum props, reference for architecture ornament | Some items are *not* CC0; filter per object. Scans are heavy: retopologise and bake. |
| Sketchfab | Community models | Per model: CC0, CC-BY, CC-BY-SA, -ND, -NC variants; the API filters by licence and downloadable status [S9] | Accept **CC0 and CC-BY only**; CC-BY-SA needs review | Uploaders sometimes mislabel licences (scans of copyrighted objects, game rips). Require provenance review for any non-CC0 import. The Download API has usage guidelines [S9]. |
| OpenGameArt | Mixed community art | Only CC0, CC-BY, CC-BY-SA, OGA-BY, GPL 2/3 are accepted [S10] | CC0 items only by default | GPL art and CC-BY-SA pull share-alike obligations into derivatives. Mixing GPL and CC-BY-SA is not strictly legal [S10]. Per-file licences vary within one "collection". |
| Freesound | Audio | Per sound: CC0, Attribution, Attribution-NonCommercial; the API filter `license:"Creative Commons 0"` narrows results [S11] | CC0 audio (ambience, foley) plus procedural synthesis | Some CC0 sounds have descriptions asking for credit; honour them as courtesy and record them. |
| CMU Graphics Lab Motion Capture DB | ≈ 2,500 mocap sequences, 144 subjects | "may be copied, modified, or redistributed without permission"; allowed in commercial products; the data itself must not be resold [S12] | Human locomotion, interaction, sports and idle base clips; retarget via `animation/retarget.hpp` | Old skeleton, marker noise. Clean with filters and foot-lock IK. The official site was returning 503 on 2026-09-23; use a pinned mirror and record the sha256s. |
| Bandai Namco Research Motion Dataset 1/2 | ≈ 3,000 stylised motions | **CC BY-NC-ND 4.0**, for research, no commercial use [S21] | **Forbidden** for shipped content. OK only for internal research comparisons that never enter the cook. | Listed so agents do not use it by mistake. |
| MakeHuman / MPFB 2 | Parametric human generator (Blender extension) | Code GPLv3; bundled assets CC0; the generated output can be used commercially, including in closed-source games [S22, S23] | Human bodies, faces, proxies, skeleton; crowd variety | Bundled clothes/hair quality varies. Add FUSE-generated clothing. |
| Copernicus DEM GLO-30 / GLO-90 | Global 30 m / 90 m DEM | Free worldwide licence (Armenia/Azerbaijan tiles excluded from GLO-30); the source must be credited: "© DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA" [S24] | Real-world terrain seeds for each biome | Attribution line required in credits. |
| NASA SRTM / USGS 3DEP / other USGS | DEMs, imagery | US public domain; acknowledge the source [S25] | DEMs (56°S–60°N for SRTM), US high-resolution lidar DEMs | |
| ESA WorldCover 10 m | Global land cover 2020/2021 | CC BY 4.0 [S26] | Biome-mask ground truth (tree cover, shrub, grass, crop, built, bare, snow, water, wetland, mangrove, moss/lichen) | Attribution + citation. |
| RESOLVE Ecoregions 2017 | 846 ecoregions, 14 biomes, 8 realms | CC BY 4.0 [S27] | Biome taxonomy and species lists per ecoregion (for choosing plant genera) | Attribution. |
| Beck et al. Köppen-Geiger 1 km | Present and future climate class maps | CC BY 4.0, cite Beck et al. 2018 [S28] | Climate class → vegetation/weather/look mapping | Attribution + citation. |
| OpenStreetMap | Roads, buildings, land use | ODbL: a game or map built from OSM data is a "Produced Work" and may use any licence, with attribution, but a *database* derived from OSM must stay ODbL [S29, S30] | Optional: street layouts, building footprints, city block statistics | The cooked `.fuselevel` or a building-footprint table can count as a Derivative Database. Keep OSM-derived data in a separate, clearly ODbL-licensed layer. **Review required.** Prefer procedural layouts calibrated on OSM *statistics* (block size, street width distributions) instead of shipping OSM geometry. |

### 2.3 Licence traps to guard against

- **Share-alike leakage**: a CC-BY-SA texture baked into a trim sheet makes the whole trim sheet
  CC-BY-SA. The manifest tracks *derivation edges* and propagates the strictest licence to outputs
  (§2.4).
- **Database rights**: OSM (ODbL) and some national datasets carry database rights. Deriving a
  footprint table is a Derivative Database; rendering it into a mesh inside a game is a Produced
  Work [S29, S30].
- **API ToS versus asset licence**: Poly Haven assets are CC0, but the API has its own
  non-commercial terms [S1, S20]. Record the retrieval method in the manifest.
- **Mislabelled uploads** on community sites (Sketchfab, OpenGameArt, Freesound): scans of branded
  products, ripped game models, trademarks. The review checklist requires a provenance check and a
  trademark/likeness check (no real people's faces, no logos).
- **Research-only datasets** (Bandai Namco, many ML datasets, AMASS subsets, Mixamo-derived sets):
  forbidden in shipped content.

### 2.4 Licence manifest and the lint gate

Every asset that enters the asset cache gets a sidecar record, collected into
`Content/licences.lock.json`. The design mirrors the `Engine/lib/<dep>/VERSION` pins that
`fuse_lint vendored-pins` checks.

```json
{
  "schema": 1,
  "assets": [
    {
      "id": "mat/rock/granite_01",
      "origin": "sourced",                    // sourced | generated | derived
      "source": "ambientCG",
      "source_id": "Rock030",
      "url": "https://ambientcg.com/view?id=Rock030",
      "retrieved": "2026-09-23",
      "retrieval": "direct-download",          // direct-download | api | mirror | generator
      "licence": "CC0-1.0",                    // SPDX id where one exists
      "attribution": null,
      "files": [{"path": "Rock030_2K_Color.png", "sha256": "…"}],
      "derived_from": [],
      "review": {"required": false, "by": null, "date": null}
    },
    {
      "id": "tree/temperate/quercus_robur_a",
      "origin": "generated",
      "generator": "Tools/FUSE/AssetGen/vegetation/tree.py@<git-sha>",
      "seed": 1234,
      "licence": "LicenseRef-FUSE-Generated",
      "derived_from": ["mat/bark/oak_01", "atlas/leaf/quercus_01"]
    }
  ]
}
```

New `fuse_lint asset-licences` mode (ctest `fuse_lint_asset_licences`, labels `lint;asset`):

1. Every file under `Content/` and every cooked output in the cook manifest resolves to a record.
2. File sha256s match (catches silent replacement).
3. The licence is in the allow-list, or `review.required` is true and `review.by` is filled.
4. Licence propagation: the effective licence of a derived asset is the most restrictive of its
   inputs; SA/ODbL/GPL art cannot flow into assets marked for the permissive pack.
5. Forbidden identifiers (`CC-BY-NC*`, `CC-BY-ND*`, known research-only dataset names) fail the gate.
6. `CREDITS.md` is regenerated from the lock file and must be up to date (diff-clean).
7. Generator records must name a generator path and git revision plus a seed, so every generated
   asset can be rebuilt bit for bit.

---

## 3. Generation pipelines (open tools only)

### 3.1 Toolchain

| Tool | Licence | Role | Headless in this container |
|---|---|---|---|
| Blender 5.x LTS (5.2 LTS manual current) [S31] | GPL (tool) | Mesh modelling via `bpy`, Geometry Nodes, UV, bakes (Cycles CPU), glTF export | `blender --background --python script.py -- args`; `--cycles-device CPU` [S31] |
| Sverchok | GPLv3 [S32] | Parametric/architectural node graphs where GN lacks features | Scripted through `bpy` |
| Sapling Tree Gen (Blender extension) | GPLv3+, limited support [S33] | Weber-Penn style trees | Yes |
| Space-colonisation tree add-on / `spacetree` | GPLv3+ [S34] | Space-colonisation branching | Yes |
| Material Maker 1.4 | MIT, Godot-based [S35] | Node-based procedural materials (Substance Designer-like) | Needs a GL context. Use Mesa llvmpipe/xvfb, or port its node shaders into FUSE's Python/C++ material baker (preferred) |
| meshoptimizer v1.x (+ `clusterlod.h`) | MIT | Simplification, meshlets, cluster DAG, vertex/index codecs [S17, S18] | Library (vendor with a `VERSION` pin, per renderer plan) |
| KTX-Software (`ktx create`) / Basis Universal | Apache-2.0 [S15, S14] | KTX2 packaging, UASTC, supercompression | CLI |
| ispc_texcomp (already in `third_party/`) | MIT | Fast BC7/BC6H/BC5 encode | Library (already hooked) |
| NVIDIA FLIP | BSD-3-Clause [S36] | Golden-render comparison | C++/Python |
| WaveFunctionCollapse (reference) | MIT [S37] | Tile layout synthesis (port to Python/C++) | Yes |
| MPFB 2 | GPLv3 code, CC0 assets [S22, S23] | Humans | Blender extension, scriptable |
| GDAL / rasterio / numpy / scipy | MIT/BSD | DEM and land-cover ingest, erosion, masks | Yes |
| Pure Data or Python (numpy/scipy) synthesis, SoX | BSD/GPL tools | Procedural audio, resampling, loudness normalisation | Yes |

All generators live in `Tools/FUSE/AssetGen/` (Python 3 + small C++ helpers). Each generator:
takes a JSON recipe + seed, is deterministic, writes glTF/PNG/EXR/WAV to the asset cache
(`build/asset-cache/`, git-ignored), and writes its licence record. Git holds **recipes and
generator code**. The bytes are rebuilt or fetched (pinned sha256) on demand. A small set of golden
cooked outputs is committed for regression tests.

### 3.2 Materials

**Target library**: ≈ 220 base materials + 24 detail maps + 6 layer materials (moss, dust, sand,
snow, wet, ash) cover every kit in §4.

Pipeline:

1. **Seed from CC0 scans** (ambientCG, Poly Haven): download 2k–4k, crop to square, remove baked
   lighting (luminance high-pass against a heavily blurred copy, preserving albedo mean), recalibrate
   albedo into §1.6 ranges, re-tile (offset + seam heal with multi-band blending / graph-cut seam
   repair), recompute the normal from height when the source normal is bad.
2. **Exemplar-based synthesis** for variants: patch-based / histogram-preserving synthesis (Heitz &
   Neyret "by-example noise" style, and "texture bombing" with random stochastic tiling at runtime)
   so one exemplar gives 3–5 non-repeating variants. The runtime counterpart is a
   `StochasticTiling` flag in the material (hex-tiling blend), which costs about 3 samples instead of 1.
3. **Procedural graph materials** (FUSE-native): extend `ProceduralMaterials` with `Rock`, `Sand`,
   `Soil`, `Snow`, `Ice`, `Plaster`, `Brick`, `Bark`, `Lava`, `Crystal`. The same code runs CPU-side
   (single-source kernels) to **bake** tileable 2k textures and GPU-side for analytic SDF surfaces.
   A C++ `fuse_matbake` tool evaluates on a torus domain (periodic noise) so bakes tile exactly.
4. **Frequency separation**: split each material into macro (≤ 1/8 of the texture frequency, stored
   in the 64² colour-variation LUT or in world-space noise), meso (the 1–2k tileable map), and micro
   (shared detail map). This saves bytes and cuts visible tiling.
5. **Material Maker graphs** where an artist-style graph is faster to write (weave, tiles, brick
   bonds): run headless under llvmpipe, or port the graph's GLSL to the FUSE baker.
6. **Output per material**: `albedo.png` (sRGB), `normal.png` (linear, +Y), `orm.png`,
   `height.png` (16-bit), `material.json` (shading model, layer compatibility, physical category,
   IOR, SSS radius, thin-film, texel size in metres).

### 3.3 Terrain (DEM ingest, erosion, biome masks)

1. **Pick exemplar regions** per biome (table §4.1) and pull Copernicus GLO-30 (or SRTM / 3DEP)
   tiles plus ESA WorldCover and Köppen maps for the same bounds [S24–S28].
2. **Reproject** to a local metric grid (GDAL), crop to 8×8 km or 16×16 km, resample to 4096² or
   8192² (≈ 2–4 m/px), and **upsample detail** with fBm plus erosion (DEMs are too smooth below
   30 m).
3. **Erosion** (CPU, numpy/C++ single-source kernel): particle hydraulic erosion (100k–1M droplets),
   thermal erosion (talus angle per material: sand 33°, scree 38°, rock 60°+), and an optional
   stream-power/uplift pass for mountain ranges. Output: height, flow accumulation, sediment,
   wetness, talus/deposit masks.
4. **Biome masks**: combine land cover + altitude + slope + curvature + flow + aspect (sun exposure)
   + climate class into per-layer weights (up to 8 per chunk, packed as 2× RGBA8 → BC7 splat or
   index+weight maps). Vegetation density maps come out of the same pass.
5. **Macro textures**: low-frequency colour (derived from the palette, not satellite photos, to
   avoid licence and baked-lighting issues) and a macro normal at 1–2 m/px.
6. **Caves**: the existing SVO caves (`terrain_caves.hpp`) get material IDs (limestone, basalt lava
   tube, ice) and speleothem scatter (stalactite/stalagmite SDF primitives).
7. **Fully procedural fallback**: the current fractal generator + erosion for fantasy/alien worlds
   and wherever no DEM fits.

### 3.4 Vegetation

**Generator**: a FUSE-owned Python generator (`AssetGen/vegetation/`) with two growth models:
- **Weber-Penn parametric** (as used by Sapling) for conifers, palms and stylised forms.
- **Space colonisation** (attraction points in a crown envelope) for broadleaf trees and shrubs,
  with tropism (phototropism, gravitropism), apical dominance and pruning.

Pipeline per species (genus-level archetype + parameter set):
1. Grow the skeleton → sweep branches as generalised cylinders with bark UVs (tiling along length,
   texel density §1.2) → leaf cards/clusters placed by phyllotaxis (137.5°).
2. **Leaf/needle atlas**: procedurally drawn leaf outlines (parametric shape per family: ovate,
   lanceolate, palmate, pinnate, needle, scale, frond, blade) with vein normal maps and
   translucency/thickness maps. Render to a 2048² atlas via CPU rasteriser, or seed from CC0 leaf
   scans (Poly Haven/ambientCG) where they exist.
3. **Bark**: shared per genus (fissured, plated, smooth, peeling, fibrous, palm-ring) from §3.2.
4. **Wind data**: per-vertex hierarchy (trunk/branch/leaf pivot and stiffness) in vertex colour +
   uv1, for a 3-level wind shader.
5. **LODs**: skeleton-aware reduction (drop twigs → merge leaf cards into clusters → billboard
   clusters), then an octahedral impostor.
6. **Variants**: 3–5 seeds per species × growth stage (sapling, mature, old/dead) × season states
   (leaf colour ramp in the material, not in new textures).
7. **Ground cover**: grass, ferns, flowers, mosses, lichens, succulents, corals and kelp as small
   instanced clumps with a biome-wide 1024² atlas.

**Coverage**: ≈ 120 plant archetypes (§4.3) give recognisable flora for every biome.

### 3.5 Rocks and cliffs

1. **SDF sculpt**: combine primitives with smooth unions/subtractions + domain-warped fBm +
   Voronoi fracture planes (strata, joints, columnar basalt, conchoidal fractures) in
   `sdf_csg.hpp`-compatible Python or C++.
2. **Erosion on the SDF**: a few passes of curvature-dependent erosion (rounding convex edges for
   weathered granite, sharp for fresh basalt), wind/sand abrasion for desert yardangs.
3. **Mesh** via dual contouring / surface nets at 1–2 cm resolution (hero) → simplify to budget →
   bake tangent normal + AO + curvature from the high SDF into 512–1024² maps. Albedo comes from the
   triplanar tileable material, so there are **no unique albedo maps**.
4. **Keep the SDF** at 32³–64³ for shadows/GI/collision.
5. **Per geology family** (§4.5): 6–10 boulders, 3–5 cliff modules, 1 scree/debris set, 1 pebble
   set.

### 3.6 Buildings and settlements

1. **Modular kit per style** (§4.4): grid-based pieces (walls 1/2/4 m, corners, windows, doors,
   floors, roofs, stairs, columns, arches, balconies, chimneys) that map UVs onto one style trim sheet.
   Pieces are generated from Blender scripts / Sverchok parametric profiles (moulding profiles swept
   along edges, bevels, bricks via instancing on facades for LOD0 only).
2. **Shape grammar (CGA-like)**: a FUSE Python grammar interpreter (`Lot → Mass → Floors → Facade →
   Tiles → Pieces`) with style rule files per architectural tradition. Parameters come from style
   data (storey height, roof pitch, window ratio, material palette).
3. **WFC** for interiors, dungeons and irregular villages (tile adjacency from the modular kit)
   [S37].
4. **Settlement layout**: street networks by tensor fields / L-systems / agent growth, calibrated on
   *statistics* of real cities (block size, street width, building footprint distributions), plus
   optionally OSM geometry in an ODbL layer (§2.2).
5. **Instancing and merging**: modular pieces instance at runtime. Far LODs merge per building into
   a proxy mesh + impostor, cooked per city block.
6. **Weathering**: vertex-colour dirt/moss masks from ambient occlusion + "rain flow" simulation on
   the merged facade, plus decals (cracks, leaks, posters, graffiti).

### 3.7 Characters and creatures

- **Humans**: MPFB 2 driven from Python (population sampler: age, sex, body mass, height and
  ancestry-neutral morph sliders spread across real anthropometric ranges) → export with the
  game-engine skeleton → FUSE clothing generator (garment templates per culture/era cut from 2D
  patterns, simulated drape in Blender cloth on CPU, then baked) → hair cards (generated strand
  clumps → card atlas) → skin material (SSS, detail pore map, tint masks).
  **Uncanny-valley guard** (§7): use a slightly stylised realism (no photoreal pore-level faces at
  crowd distance), a strong animation-quality bar, and eyes/teeth with correct wet specular.
- **Parametric creatures**: a *procedural creature assembler*. Body plans (biped, quadruped,
  hexapod insect, serpentine, avian, fish, cephalopod, dragon = quadruped + wing pair) as graphs of
  parts (torso, neck, head, limbs, tail, wings, fins) with SDF bodies (metaballs + smooth union) →
  mesh → auto-rig (the skeleton follows the part graph) → skinning weights from the SDF (heat
  diffusion). Surface: tileable skin/scale/fur/feather/chitin detail materials + mask-based
  patterns (stripes, spots, countershading via reaction-diffusion or noise).
- **Real fauna** (§4.6): use Quaternius CC0 animals as topology/rig starting points [S6], reshaped
  via lattice morphs to real proportions, or build from the assembler. Fur as shell/fins for short
  fur, cards for manes.

### 3.8 Animation

- **Mocap**: CMU database (permissive, see §2.2) [S12] → BVH import → cleanup (Butterworth filter,
  foot-contact detection + foot-lock IK, root-motion extraction) → retarget via
  `animation/retarget.hpp` to the FUSE humanoid skeleton → compress (keyframe reduction, quantised
  quaternions). Target: ≈ 300 clips (locomotion sets for 8 styles, idles, interactions, climbing,
  swimming, combat basics).
- **Procedural locomotion** for creatures: phase-based gait generator (walk/trot/gallop/crawl/
  slither/fly/swim) + foot IK on terrain (`ik_solver.hpp`) + spine/tail spring chains + look-at. It
  covers every assembler creature without mocap.
- **Secondary motion**: spring/verlet chains for tails, ears, cloth panels, hair cards, vegetation
  (wind is shader-side).
- **Facial**: FACS-style blend-shape set from MPFB expressions + procedural blink, saccades, visemes
  from phoneme timing.
- **Forbidden**: Bandai Namco dataset (NC-ND) [S21], Mixamo (Adobe terms, not an open licence),
  research-only datasets.

### 3.9 Audio

- **CC0 Freesound**: API search with `license:"Creative Commons 0"` [S11], then human listening
  review (tier-2 agents can pre-filter by duration, sample rate ≥ 44.1 kHz, spectral flatness,
  clipping) → loudness-normalise (EBU R128, −23 LUFS for beds, −16 LUFS peak-safe for one-shots) →
  resample 48 kHz → seamless loops (crossfade at zero crossings, spectral match) → Vorbis.
- **Procedural synthesis** (numpy/Pure Data), especially for:
  - Wind (filtered noise, gusts driven by the weather system), rain (granular droplets, surface
    type), fire (crackle impulses + roar), water (streams: modal bubbles; surf: filtered noise
    envelopes), thunder (N-wave + reverb), insects (FM chirp models), footsteps (surface-dependent
    modal + noise, driven by physics material), impacts (modal synthesis from material presets), magic
    (FM/granular pads).
- **Impulse responses**: generate IRs procedurally (image-source + stochastic late tail
  parameterised by room volume and absorption) for the convolution reverb (`conv_reverb_cpu.hpp`):
  forest, canyon, cave, cathedral, small room, street canyon, underwater.
- **Structure**: each biome has 1 day bed + 1 night bed + weather layers (wind light/strong, rain
  light/heavy, snow hush) + 20–40 spatial one-shots (birds, insects, animals, water spots), all
  scheduled by the audio engine with randomisation.

### 3.10 VFX

- **Flipbooks** baked from simulations on CPU: Blender Mantaflow (smoke, fire, explosions,
  splashes) or a FUSE-native 2D/3D fluid solver → render with Cycles CPU at 128–256² per frame
  → 8×8 atlas + **motion vectors** for frame blending (halves the frame count) → BC7 (colour) /
  BC5 (motion) / BC4 (density). Emissive HDR flipbooks (fire, magic) go to BC6H.
- **Mesh particles** for debris, leaves, embers, and procedural shapes for magic (ribbons,
  trails, SDF shapes).
- **Library**: ≈ 60 flipbooks (smoke ×6, fire ×5, explosion ×4, dust ×5, sand, snow, rain splash,
  water splash ×4, steam, sparks, magic ×10, blood, ash, fog wisps, waterfall mist, lava bubbles,
  bioluminescence).

### 3.11 Skies and HDRIs

- The engine's atmosphere renders the sky. HDRIs serve (a) calibration references for golden scenes
  and (b) optional backdrop/reflection for indoor or stylised scenes.
- Poly Haven CC0 HDRIs [S1]: one clear, one overcast, one sunset and one night per climate family
  (≈ 20 HDRIs at 4k, BC6H ≈ 21 MB each with mips; 2k for Lite).
- Clouds: volumetric cloud noise textures (Perlin-Worley 128³ + 32³ detail) generated
  procedurally; weather maps per biome.

---

## 4. Coverage matrix

Legend: **G** = generated, **S** = sourced (CC0 seeds, then processed), **G/S** = generated from
sourced exemplars. "MVK" = minimum viable kit: the smallest set that makes the biome recognisable.
Look profile = a `.fuselook` preset (§5.6), plus weather variants (clear/overcast/rain/fog/storm/snow
where applicable).

### 4.1 Biomes of the planet

Köppen class uses Beck et al. 2018 [S28]; biome names follow RESOLVE 2017's 14 biomes [S27].

| # | Biome (Köppen; RESOLVE biome) | Exemplar DEM regions | MVK: terrain layers | MVK: vegetation (archetypes) | MVK: rocks/props | Ambience | Look profile |
|---|---|---|---|---|---|---|---|
| B1 | Temperate deciduous forest (Cfb/Dfb; Temperate Broadleaf & Mixed) | Black Forest DE, Appalachians US | leaf litter, forest soil, moss, mud, grass, granite/sandstone | oak, beech, maple, birch, hazel, fern, bramble, bluebell/wildflowers, grass | mossy boulders, logs, stumps, fungi, stone walls, fences | birds (dawn chorus), wind in leaves, stream | `temperate_forest` (soft green bounce, seasonal LUTs ×4) |
| B2 | Temperate coniferous / Pacific rainforest (Cfb/Csb; Temperate Conifer) | Olympic Peninsula US, Scottish Highlands | needle litter, moss, fern floor, rock, gravel | Douglas fir, spruce, pine, hemlock, cedar, sword fern, huckleberry | moss logs, nurse stumps, basalt boulders | wind in needles, ravens, rain drip | `conifer_mist` |
| B3 | Boreal taiga (Dfc/Dfd; Boreal Forests) | Finland, Siberia, Canadian Shield | lichen, moss, peat, podzol, granite, bog water | spruce, larch, birch, pine, dwarf shrubs, cotton grass | glacial erratics, peat hummocks | wind, loons, mosquitoes | `boreal` (low sun, long shadows) |
| B4 | Tundra (ET; Tundra) | Svalbard, Alaska North Slope | lichen, frost-heave soil, tussock, gravel, permafrost polygons | dwarf willow, sedges, mosses, lichens, arctic flowers | frost-shattered rock, pingos, stone circles | wind, geese, silence | `tundra` |
| B5 | Polar ice / glacier (EF; Rock & Ice) | Greenland, Antarctica, Vatnajökull | snow, firn, blue ice, crevasse walls, moraine | none | séracs, crevasses, ice caves, moraine boulders | wind (strong), ice creak | `polar` (high albedo exposure, blue shadows) |
| B6 | Alpine / high mountain (ET/Dfc highland; Montane Grasslands) | Alps, Himalaya, Andes | scree, alpine meadow, snow, granite/limestone cliff | dwarf pine, alpine grasses, edelweiss, rhododendron | cliff modules, cairns, scree | wind, marmots, bells | `alpine` (thin air, strong UV) |
| B7 | Tropical rainforest (Af; Tropical Moist Broadleaf) | Amazon (Peru), Borneo, Congo | red laterite, leaf litter, mud, buttress-root soil | emergent (kapok), strangler fig, palms, lianas, epiphytes, bromeliads, ferns, giant leaves | buttress roots, fallen trunks, termite mounds | cicadas, frogs, macaws, rain | `rainforest` (humid haze, dappled) |
| B8 | Tropical savanna (Aw; Tropical Grasslands & Savannas) | Serengeti, Kruger | dry grass, red soil, termite soil, dust | acacia, baobab, marula, tall grass, shrubs | kopjes (granite inselbergs), termite mounds | insects, lions distant, wind in grass | `savanna` (golden hour heavy) |
| B9 | Hot desert, sand (BWh; Deserts & Xeric) | Rub' al Khali, Namib, Sahara erg | dune sand (ripples), hardpan, desert pavement | sparse: date palm (oasis), tamarisk, grass tussock | dunes (procedural), bones, wind-sculpted rock | wind, sand hiss | `desert_sand` (bright, heat shimmer) |
| B10 | Rock desert / canyon (BWh/BWk) | Utah/Arizona, Wadi Rum | sandstone (strata), slickrock, desert varnish, gravel wash | saguaro/prickly pear (Americas), creosote, juniper, joshua tree | mesas, hoodoos, arches, canyon walls | wind, ravens, echo | `canyon` |
| B11 | Salt flat / playa (BWk) | Salar de Uyuni, Bonneville | salt polygon crust, mud crack, brine pools | none | salt crust plates | silence, wind | `salt_flat` (mirror when wet) |
| B12 | Cold steppe / prairie (BSk/Dfa; Temperate Grasslands) | Mongolia, Kazakhstan, Great Plains | short grass, loess, dry soil, gravel | feather grass, sagebrush, wildflowers, lone poplar | yurt-site props, fences, burial mounds | wind, larks, horses | `steppe` |
| B13 | Mediterranean scrub (Csa; Mediterranean Forests) | Provence, Crete, California chaparral | terra rossa, limestone, dry grass, gravel | olive, cork oak, cypress, stone pine, maquis/lavender/rosemary, cistus | limestone outcrops, dry-stone terraces | cicadas, goats, wind | `mediterranean` (hard light, warm) |
| B14 | Wetland / marsh / bog (various; Flooded Grasslands) | Everglades, Pantanal, Irish bogs | mud, peat, standing water, reeds soil | reeds, cattails, papyrus, water lilies, cypress/tupelo, sphagnum | boardwalks, rotting logs | frogs, herons, insects | `wetland` (fog, reflections) |
| B15 | Mangrove (Af/Am coast; Mangroves) | Sundarbans, Florida | tidal mud, oyster beds, brackish water | red/black mangrove (prop roots, pneumatophores), nipa palm | driftwood | water lap, crabs, birds | `mangrove` |
| B16 | Coast: beach, cliff, dune (any) | Algarve PT, Big Sur, Great Ocean Road AU | beach sand (wet/dry), shingle, tidal rock, kelp wrack | marram grass, coastal pine, ice plant, coconut palm (tropical) | sea stacks, tide pools, driftwood, shells | surf, gulls, wind | `coast` (sea spray haze) |
| B17 | Coral reef / underwater (Af marine) | Great Barrier Reef, Red Sea | sand, reef rock, rubble, seagrass | hard/soft corals (procedural L-system/DLA), kelp forest (temperate), seagrass, sponges, anemones | shipwreck modules, reef boulders | underwater muffle, bubbles, whale song | `underwater` (caustics, absorption by depth) |
| B18 | Volcanic (varies) | Iceland, Hawaii, Etna | fresh basalt (aa/pahoehoe), ash, pumice, sulfur, lava (emissive) | pioneer moss, ferns, ohia (Hawaii) | lava tubes, columnar basalt, fumaroles, cinder cones | rumble, hiss, bubbling | `volcanic` (ash haze, emissive glow) |
| B19 | Karst / caves (Cfa/Cfb) | Guilin CN, Ha Long, Slovenian karst | limestone (karren), cave floor, flowstone, mud, water | karst forest, hanging vegetation | tower karst, sinkholes, stalactites/stalagmites, cenotes | drips, bats, cave reverb | `cave` (darkness, torch, volumetric shafts) |
| B20 | Subtropical humid / monsoon (Cwa/Cfa) | Southern China, Northern India | rice-paddy mud, red soil, bamboo litter | bamboo, banyan, rice, tea shrubs, magnolia | terraces, paddies | frogs, monsoon rain | `monsoon` |
| B21 | Agricultural / cultivated (any) | Tuscany, Midwest, Mekong | ploughed soil, crop stubble, gravel road | wheat, maize, rice, vineyards, orchards, hedgerows | fences, haystacks, irrigation | tractors distant, birds | shared with host climate |

**Weather and seasons** (engine systems, content hooks): each biome defines valid weather states
and seasonal ramps: leaf colour and density curve (deciduous), snow-layer coverage, wetness,
puddle masks, grass dryness hue. These are *material parameters and masks*, not new textures.
Precipitation VFX: rain, snow, sleet, hail, sandstorm, ash fall. Look profile variants per weather
(overcast desaturation, fog density, wet-road reflections).

### 4.2 Geology families (shared across biomes)

| Family | Materials (tileable + detail) | Rock set | Used by |
|---|---|---|---|
| Granite / gneiss | granite grey/pink, weathered granite, lichen-covered | rounded boulders, tors, exfoliation sheets | B1, B3, B6, B8, B16 |
| Sandstone | red/cream/banded, desert varnish, slickrock | mesas, hoodoos, arches, strata cliffs | B10, B16 |
| Limestone / marble | grey limestone, karren, marble (polished/raw) | tower karst, pavements, sea stacks | B13, B19, urban |
| Basalt / volcanic | fresh/weathered basalt, columnar, pumice, obsidian, tuff | columns, aa blocks, lava tubes | B18, B16, fantasy |
| Slate / shale / schist | layered, splitting, wet slate | sharp strata, scree | B2, B6, urban roofs |
| Sedimentary loose | sand (8 colours), gravel, pebbles, clay, loess, mud | dunes, pebble sets | all |
| Ice / snow | fresh snow, wind-packed, firn, blue ice, dirty ice | séracs, ice boulders | B5, B6, B4 |
| Salt / evaporite | salt crust, gypsum | polygons, crystals | B11, fantasy |
| Soils | forest, laterite, podzol, chernozem, terra rossa, peat, permafrost | – | all |

### 4.3 Flora families (≈ 120 archetypes, generated)

| Group | Archetypes (genus-level) | Count |
|---|---|---|
| Broadleaf temperate | Quercus, Fagus, Acer, Betula, Fraxinus, Tilia, Salix, Populus, Castanea, Corylus, Sorbus, Juglans | 12 |
| Conifers | Pinus (3 habits), Picea, Abies, Larix, Pseudotsuga, Tsuga, Thuja/Cedrus, Juniperus, Cupressus, Sequoia | 12 |
| Tropical trees | Ceiba (emergent), Ficus (banyan/strangler), Dipterocarp, Mahogany-type, Mangrove (Rhizophora, Avicennia), Tabebuia, Cecropia | 8 |
| Palms / cycads | Cocos, Phoenix (date), Roystonea, fan palm (Washingtonia), rattan, cycad, nipa | 7 |
| Dryland trees/shrubs | Acacia (umbrella), Adansonia (baobab), Olea (olive), Quercus suber, Tamarix, Prosopis, Larrea, Artemisia, Joshua tree | 9 |
| Succulents / cacti | Carnegiea (saguaro), Opuntia, Agave, Aloe, Euphorbia (candelabra), barrel cactus | 6 |
| Shrubs / understory | hazel, holly, rhododendron, heather, lavender/rosemary, bramble, bamboo (2), tea, coffee, dwarf willow | 11 |
| Herbaceous / grasses | temperate grass (3), tall savanna grass, feather grass, marram, reeds/cattail, papyrus, sedge, rice, wheat, maize, tussock | 14 |
| Ferns / mosses / lichens | bracken, sword fern, tree fern, sphagnum, cushion moss, crustose/foliose lichen (decals), epiphytes, bromeliad, orchid | 9 |
| Flowers | 12 generic flower heads (parametric petals: count, shape, colour) | 12 |
| Aquatic | water lily, lotus, kelp, seagrass, algae mats | 5 |
| Corals / reef | branching, brain, table, fan, soft coral, sponge, anemone | 7 |
| Fungi | bracket fungi, mushrooms (3 forms), mycelium decals | 4 |
| Dead / special | snags, fallen logs, stumps, driftwood | 4 |

### 4.4 Built environments (by region and era)

Each style = 1 trim sheet (2048²) + 3–5 tileable materials + 40–80 modular pieces + 15–30 props +
1 decal atlas + grammar rules. Styles are grouped so they can share trim families (timber, stone,
mud-brick, concrete).

| Region | Ancient / classical | Medieval / early modern | Vernacular (rural) | Industrial / colonial | Modern / contemporary |
|---|---|---|---|---|---|
| Europe | Greco-Roman (temples, insulae, aqueducts) | Romanesque/Gothic stone town, castles, half-timber (N. Europe), Mediterranean village | Alpine chalet, Nordic turf/log, Irish stone cottage | Victorian brick terraces, factories, rail | Modernist concrete/glass, Soviet blocks, European suburb |
| Middle East / N. Africa | Mesopotamian mud-brick, Egyptian temples | Islamic medina (courtyards, domes, minarets, mashrabiya), caravanserai | Adobe/mud-brick villages, Bedouin tents | Ottoman timber-frame | Gulf high-rise, dense concrete city |
| Sub-Saharan Africa | Great Zimbabwe dry-stone, Aksum stelae | Sahelian mud mosque (Djenné style), Swahili coral-stone | Round huts (thatch), Ndebele painted houses, Maasai boma | Colonial verandah | African megacity (informal + modern towers) |
| South Asia | Indus Valley brick | Hindu temple (Nagara/Dravidian), Mughal (red sandstone, marble) | Rajasthani havelis, Kerala timber | Colonial Indo-Saracenic | Dense Indian city, markets |
| East Asia | Chinese Han/Tang timber, Japanese Nara | Chinese courtyard (siheyuan), Japanese castles/shrines, Korean hanok | Rice-terrace villages, tulou | Treaty-port shikumen | Tokyo/Shanghai dense modern, neon streets |
| SE Asia / Oceania | Khmer (Angkor) sandstone | Thai/Burmese temples (stupas) | Stilt houses, longhouses, Polynesian fale, Māori wharenui (only with respectful, non-sacred generic motifs) | Colonial shophouses | SE Asian megacity, Australian suburb |
| Americas | Maya/Aztec stepped pyramids, Inca ashlar, Ancestral Puebloan | Spanish colonial missions, plazas | Log cabin, adobe pueblo, Andean stone/thatch, Caribbean clapboard | Western frontier town, brick industrial | US downtown, suburbia, favela |
| Infrastructure (global) | roads (Roman to asphalt), bridges (stone arch, truss, suspension), walls, harbours, rail, power lines, wind turbines, signage (generic, no real brands) | | | | |

**Cultural care**: sacred sites and indigenous designs are represented through generic, researched
forms rather than copied specific sacred objects. Reviewers check this for kits in the Oceania,
Americas and Africa rows.

### 4.5 Fauna classes (generated via the creature assembler unless noted)

| Class | Archetypes (MVK) | Method | LOD strategy |
|---|---|---|---|
| Large mammals | deer, horse, cattle, bear, wolf/dog, big cat, elephant, camel, giraffe, antelope | Assembler + Quaternius CC0 topology seeds (S) | 3 LODs + impostor herds |
| Small mammals | rabbit, squirrel, fox, rodent, monkey, bat | Assembler | 2 LODs |
| Birds | songbird, raptor, gull, crow, parrot, duck, heron, penguin | Assembler (avian body plan), flocking VFX | Far flocks as particles |
| Reptiles / amphibians | lizard, snake, crocodile, turtle, frog | Assembler (serpentine/quadruped low) | |
| Fish / marine | reef fish (5 body shapes × pattern masks), shark, ray, whale, dolphin, jellyfish, octopus | Assembler (fish/cephalopod) + boids | Schools as instanced VFX |
| Invertebrates | butterfly, bee, beetle, spider, crab, ant | Assembler (hexapod/octopod) | Mostly particles |
| Humans | population sampler (MPFB) × clothing per region/era (§4.4 columns) | MPFB CC0 [S22] + generated garments | Crowd atlas + impostor |

### 4.6 Fantasy and speculative sets

| Set | Core idea | MVK assets | Materials | Vegetation / terrain | Audio | Look |
|---|---|---|---|---|---|---|
| F1 High fantasy | Bright, heroic, ornate | elven/dwarven/human kits (reuse Europe medieval + ornament trim), wizard tower, bridges, ruins, banners, weapons/armour set | polished marble + gold inlay, enchanted wood, mithril (anisotropic metal), crystal | giant trees (scaled Quercus/Sequoia), glowing flowers | choirs, harps (procedural pads), birds | `high_fantasy` (saturated, bloom) |
| F2 Dark fantasy | Decay, gothic horror | gothic ruins (reuse Gothic kit + damage), graveyards, cages, bones, chains, cursed altars | rotten wood, rusted iron, bone, wax, blood/ichor, tarnished silver | dead trees, thorns, fungal growth, bog | wind howl, crows, drones | `dark_fantasy` (desaturated, fog) |
| F3 Modular dungeons | WFC-generated layouts | 4 styles: stone crypt, dwarven hall, natural cave, sewer; doors, traps, torches, chests, pillars (≈ 60 pieces per style) | wet stone, moss, iron, torch-lit plaster | cave SVO + stalactites | drips, cave reverb IRs | `dungeon` |
| F4 Sci-fi | Near-future to far-future | hard-surface kit (panels, pipes, doors, consoles), spaceship corridor, habitat modules, vehicles, drones | painted metal, carbon fibre, glass, plastics, emissive panels, holograms | terraformed domes, hydroponics | hums, servos, beeps (synth) | `scifi_clean`, `scifi_grimy` |
| F5 Post-apocalyptic | Ruined modern world | damage variants of Modern kits (procedural fracture via SDF boolean + debris scatter), rust vehicles, barricades, overgrowth | rust, peeling paint, ash, broken glass, cracked asphalt | invasive vegetation over ruins (ivy, weeds via growth on surfaces), dust storm | wind, metal creaks, geiger | `wasteland` (dusty, warm/green grade) |
| F6 Alien biomes | Plausible-but-foreign ecology | alien flora (L-system with alien parameters: spirals, tubes, fans, bulbs), alien rocks (mineral colours, crystal clusters), strange skies | iridescent (thin-film), bioluminescent (emissive + SSS), chitin, gel (translucent) | recoloured erosion, hexagonal basalt, floating rocks (fantasy), methane lakes | procedural drones, alien calls (FM) | `alien_teal`, `alien_violet` (non-Earth sky tints via atmosphere params) |
| F7 Magic materials | Shared across F1–F6 | crystals (SDF clusters), runes (decals + emissive), portals, force fields, enchanted weapons | crystal (refractive, dispersion approximated), emissive runes, iridescent, gold leaf, obsidian, ethereal (additive/translucent), animated emissive | – | shimmer, resonance | – |
| F8 Creature archetypes | Assembler presets | dragon (quadruped + wings + long tail), wyvern, griffin, troll/ogre (scaled human morphs), goblin, undead (human + decay masks), elemental (VFX body + rock/fire/ice SDF), giant insect/arachnid, sea serpent, kaiju | scales (detail), leather wing membrane (SSS), bone, fur, ice/fire emissive | – | roars (pitch-shifted/granular layered animal CC0 + synth) | – |
| F9 Mythic cultures | Fantasy versions of §4.4 styles | reuse region trims with fantasy ornament overlays (Norse, Egyptian, East Asian, Mesoamerican-inspired) | same | – | – | – |

### 4.7 Generated versus sourced summary

| Category | Generated | Sourced (processed) |
|---|---|---|
| Terrain heightfields | erosion, detail, masks, fantasy terrain | DEM seeds (Copernicus/SRTM/3DEP), land cover, climate maps |
| Base materials | ≈ 60 % (procedural bakes, synthesis variants) | ≈ 40 % (ambientCG/Poly Haven CC0 scans, calibrated) |
| Vegetation | 100 % meshes; leaf atlases mostly generated | some CC0 leaf/bark scans as exemplars |
| Rocks/cliffs | 100 % (SDF) | material textures from §3.2 |
| Architecture | 100 % modular/grammar | Smithsonian CC0 ornament references (and a few scans for museum props) |
| Props | ≈ 80 % (parametric generators: barrels, crates, pottery lathe, furniture) | ≈ 20 % (Kenney/Quaternius/Smithsonian CC0 base shapes, retextured) |
| Humans | MPFB + generated clothes/hair | MPFB CC0 assets |
| Creatures | assembler | Quaternius CC0 topology seeds |
| Animation | procedural locomotion/secondary | CMU mocap |
| Audio | synthesis (wind, rain, fire, water, footsteps, magic, IRs) | Freesound CC0 (animals, specific foley) |
| VFX | 100 % (sim + render) | – |
| HDRIs | procedural atmosphere captures | Poly Haven CC0 HDRIs |

---

## 5. The asset pipeline in FUSE

### 5.1 Cook formats (extensions to what exists)

| Format | Status | Change |
|---|---|---|
| `.fusemesh` (`FMSH`) | v1: pos/normal/uv0 f32 | **v2**: optional streams with a stream table: tangent (oct-encoded + sign), uv1, colour0 (RGBA8), joints (u8×4/u16×4) + weights (unorm8/16×4), quantised positions (unorm16 in bounds), meshopt vertex/index codec, discrete LOD table (per LOD index range + error), meshlet table (≤ 64 v / 124 t, cone + sphere bounds), cluster DAG (groups, parent error, bounds), per-submesh material slot names. Stays deterministic with the FNV-1a trailer and the no-repair rule. |
| `.fusetex` | BC7 mode 6 only | Real BC1/BC4/BC5/BC6H/BC7 through `ispc_texcomp` (hook exists), texture arrays, cube maps, explicit colour space, per-mip RDO, optional zstd; header records `srgb`, `normal_convention=gl`, `texel_m` (metres per texel at mip 0). |
| `.fusemat` (new) | – | JSON source → binary: shading model, texture slots (bindless indices resolved at load), layer stack (≤ 3 height-blended layers), detail map + scale, stochastic tiling flag, triplanar flag, procedural function id + params, physical category (for footsteps/impacts/audio), wind profile. |
| `.fuseveg` (new) | – | Species: LOD meshes, impostor atlas refs, wind hierarchy, seasonal ramps, placement rules (slope, altitude, moisture, density, clustering). |
| `.fuseimp` (new) | – | Octahedral impostor: grid size, view count, atlas refs (albedo/alpha BC7, normal+depth BC7), bounds. |
| `.fusesdf` (new) | – | Sparse brick SDF (R16F or BC4-encoded bricks), voxel size, bounds; also used by rock LOD ray marching. |
| `.fuseanim` (new) | – | Clip: skeleton hash, sample rate, quantised tracks, root motion, events (footsteps). |
| `.fusebank` (new) | – | Audio bank: Vorbis clips + ambience scheduler data + IR refs. |
| `.fuselook` | exists | Add per-biome presets and weather variants; LUTs (33³ or 65³ `.cube` → RGBA16F 3D texture) in `Samples/Looks/luts`. |
| `.fusebiome` (new) | – | Biome pack: terrain layer set, splat rules, vegetation/rock/prop scatter tables, ambience bank, look preset, weather table, residency group id. |

The cook manifest (`cook_manifest.hpp` `CookAssetKind`) gains `Material, Vegetation, Impostor, Sdf,
Anim, AudioBank, Look, Biome`, and every entry gets a `licence_id` that points into
`licences.lock.json`.

### 5.2 Directory layout

```
Content/
  licences.lock.json        # §2.4
  CREDITS.md                # generated
  recipes/                  # JSON recipes (committed): materials, species, rocks, styles, creatures, biomes
  sources.lock.json         # sourced files: url + sha256 (committed); bytes live in build/asset-cache
  golden/                   # small committed cooked references + golden renders (PNG, ≤ 20 MB total)
Tools/FUSE/AssetGen/        # generators (python + C++ helpers), each deterministic by seed
build/asset-cache/          # git-ignored: downloaded sources + generator outputs
build/cooked/<tier>/        # git-ignored: cooked packs per quality tier
```

Rule: **nothing large in git**. Recipes + locks are small and enough to rebuild everything.

### 5.3 Validation gates (ctest labels `asset;gate`)

| Gate | Check | Fails when |
|---|---|---|
| `asset_licences` | §2.4 lint | missing record, bad sha, forbidden licence, missing review, stale CREDITS |
| `asset_naming` | `<class>/<biome|style>/<name>_<variant>` ids; file suffixes `_albedo/_normal/_orm/_height/_emissive` | mismatch |
| `asset_budget_mesh` | triangles per LOD vs §1.2 class table; LOD ratio monotonic; screen-space error recorded | over budget, LOD missing, LOD not reducing ≥ 40 % |
| `asset_budget_texture` | resolution vs class, power of two, mip chain complete, format per map type (§1.3) | 4k where 2k is allowed, BC7 normal, sRGB normal |
| `asset_texel_density` | per UV island px/m vs class target (±50 %) | outliers |
| `asset_normal_map` | decoded vectors unit length ±0.05, mean Z > 0.7, no G-channel inversion (compare against height-derived normal sign) | DirectX-convention or broken maps |
| `asset_color_space` | albedo in sRGB, data maps linear; albedo range §1.6; no baked lighting (AO/albedo correlation < 0.5) | out of range |
| `asset_pbr_sanity` | metal mask binary-ish; roughness non-constant; emissive only on emissive materials | violations |
| `asset_geometry` | real-world scale vs category; no degenerate/zero-area triangles; manifold where required (SDF bake); pivots at base; up axis +Z/+Y consistent with FUSE convention | violations |
| `asset_lod_presence` | every mesh used at distance > 50 m has LODs **or** an impostor **or** a DAG | missing |
| `asset_skin` | ≤ 4 weights per vertex, weights sum to 1, skeleton hash matches rig | violations |
| `asset_audio` | 48 kHz, loudness targets ±1 LU, loop seam click check, no clipping | violations |
| `asset_determinism` | regenerating from recipe + seed gives identical bytes (sampled 5 % per run) | drift |
| `asset_golden` | §5.4 | FLIP/SSIM over threshold |

Gates run on CPU. The cook refuses (strict mode, which is already the default) assets that fail
any gate; `--lenient` only for local iteration.

### 5.4 Golden renders on reference scenes

- **Reference scenes** (`Samples/content_golden/`): (1) material ball grid (every material under 3
  lighting setups: sun, overcast, interior), (2) biome vignette per kit (fixed camera, fixed
  time of day and weather), (3) character turntable, (4) vegetation LOD strip (same tree at each
  LOD + impostor at matched distance), (5) architecture street, (6) VFX flipbook contact sheet.
- **Renderer**: the FUSE renderer on **Lavapipe** at 960×540, fixed seed, TAA off (or fixed jitter
  sequence), deterministic DDGI probe update count.
- **Metrics**: NVIDIA FLIP (LDR/HDR, BSD-3-Clause) [S36] as the primary metric (mean FLIP ≤ 0.05
  for a pass; per-pixel heat-map artefact on failure), SSIM ≥ 0.97 secondary. LOD-transition gate:
  FLIP between LODn and LODn+1 at the switch distance ≤ 0.08.
- **Update policy**: a golden changes only in the same change as its recipe, with the diff image
  attached in the PR description.

### 5.5 Streaming, residency and packaging per biome

- **Pack structure**: `core.pak` (shared materials, detail maps, layer materials, UI, shared
  audio, humans base) + `biome_<id>.pak` + `style_<id>.pak` + `fantasy_<id>.pak` + `creatures.pak`.
- **Residency groups**: world partition cells reference residency groups. The streamer
  (existing `lod_residency_queue.hpp` pattern, `WorldPartition`) prefetches the neighbouring biome's
  pack when the player is within a transition band (≈ 500 m).
- **Mip streaming**: packs store mips tail-first; mips 0–1 of tileable materials load on demand by
  screen-space texel footprint feedback (from the visibility buffer's material/UV derivatives).
- **Budgets per resident biome** (Standard tier): VRAM for textures ≤ 1.0 GB (core 450 MB + biome
  400 MB + transition 150 MB), geometry ≤ 400 MB.

### 5.6 Look profiles

- One `.fuselook` per biome/set (names in §4.1/§4.6) + weather variants as blend targets (the Look
  system already blends flat parameter blocks without allocation).
- LUTs authored procedurally (ACES-style tone map + colour-grade parameters baked to 33³) and
  validated on the golden scenes. LUTs live in `Samples/Looks/luts/`.
- Rule: looks *grade*; they must not fix bad materials. The golden material-ball scene renders with
  the neutral look.

---

## 6. Phased execution plan

Each wave is a set of agent-sized tasks (1 task ≈ one PR, one session). Exit criteria are checked by
ctest gates. CPU times are estimates for an 8–16 core container.

### Wave 0: pipeline foundations (≈ 10 tasks) **[P0]**

| Task | Output | Exit criteria |
|---|---|---|
| W0.1 `FMSH` v2 streams (tangent, uv1, colour, skin, quantisation) | `mesh_cook.cpp` + tests | round-trip tests; v1 still loads |
| W0.2 Vendor meshoptimizer with a `VERSION` pin; LODs + meshlets + cluster DAG in the cook | `Engine/lib/meshoptimizer` | `fuse_lint_vendored_pins_meshoptimizer`; LOD error monotonic test |
| W0.3 Real BC1/BC4/BC5/BC6H/BC7 through ispc_texcomp; texture arrays | `texture_cook.cpp` | per-format PSNR thresholds; normal maps BC5 |
| W0.4 KTX2 import/export (transport only) | `fuse_cook --texture` accepts `.ktx2` | round-trip test |
| W0.5 Licence lock + `fuse_lint asset-licences` | `Content/licences.lock.json`, lint mode | ctest `fuse_lint_asset_licences` green |
| W0.6 Validation gates (§5.3) as `fuse_assetcheck` | tool + ctest | each gate has a failing fixture |
| W0.7 `.fusemat` + layered material + detail + triplanar + stochastic tiling in the renderer | renderer material system | material-ball golden scene renders on Lavapipe |
| W0.8 Golden render harness (FLIP C++ vendored) | `Samples/content_golden` | deterministic re-render FLIP = 0 |
| W0.9 `Tools/FUSE/AssetGen` skeleton: recipe schema, seed plumbing, cache, licence records | python package | `asset_determinism` gate |
| W0.10 Blender headless in container (install pinned Blender LTS into cache, smoke script) | script + doc | exports a glTF cube via `--background` |

### Wave 1: shared material library (≈ 8 tasks) **[P0]**

- 100 sourced CC0 materials (ambientCG/Poly Haven), calibrated; 60 procedural bakes
  (`fuse_matbake`); 24 detail maps; 6 layer materials; the extended analytic procedural set (rock,
  sand, soil, snow, ice, plaster, brick, bark, lava, crystal).
- **Exit**: 190 materials pass all gates; material-ball goldens committed; storage ≤ 700 MB cooked
  (Standard), ≤ 200 MB Lite.
- CPU: ≈ 6–10 h total baking (2k maps, synthesis variants).

### Wave 2: terrain + biome system (≈ 8 tasks) **[P0]**

- DEM/land-cover/Köppen ingest (GDAL), erosion kernels (single-source CPU/GPU), biome mask
  generator, terrain layer splat material, `.fusebiome` format, scatter system (GPU-instanced,
  deterministic by cell), cave material IDs.
- **Exit**: B1 temperate forest terrain from a real DEM renders with layers + scatter placeholders;
  terrain gates still green at 4096²; erosion of 4096² ≤ 10 min CPU.

### Wave 3: vegetation generator + rocks (≈ 10 tasks) **[P0]**

- Space-colonisation + Weber-Penn generator, leaf atlas generator, wind data, LODs, octahedral
  impostor baker (Lavapipe), `.fuseveg` / `.fuseimp`; SDF rock generator + `.fusesdf`.
- **Exit**: 12 temperate species × 3 variants, 3 geology rock families; LOD-strip goldens pass;
  a tree cooks in ≤ 3 min CPU including impostor.

### Wave 4: biomes in priority order (≈ 3–4 tasks per biome)

Priority by reuse and by coverage of common game settings:

| Order | Biomes | Reason |
|---|---|---|
| 4a | B1 temperate deciduous, B2 conifer, B6 alpine, B16 coast | most common settings; exercise every system |
| 4b | B9/B10 deserts, B13 Mediterranean, B8 savanna, B12 steppe | dryland set shares sand/rock/grass |
| 4c | B7 rainforest, B14 wetland, B15 mangrove, B20 monsoon, B21 agriculture | dense vegetation, water |
| 4d | B3 taiga, B4 tundra, B5 polar, B11 salt flat | cold set, snow/ice |
| 4e | B18 volcanic, B19 karst/caves, B17 reef/underwater | special rendering (emissive, caves, underwater) |

**Exit per biome**: MVK complete (§4.1 row), ambience bank, look profile, golden vignette, pack
within budget (§6.9), 10-minute fly-through capture on Lavapipe with no gate failures.

### Wave 5: urban styles (≈ 3 tasks per style)

Grammar engine + WFC first (≈ 4 tasks), then styles in order: Europe medieval → Europe modern →
Middle East medina → East Asia traditional → East Asia modern → South Asia → Americas colonial →
US modern/suburb → Sub-Saharan vernacular + megacity → SE Asia/Oceania → ancient sets (Greco-Roman,
Egyptian, Maya/Aztec, Khmer, Mesopotamian) → industrial/Victorian.
**Exit per style**: trim sheet + kit + props + decals + grammar rules; a generated 300×300 m
district renders; merged-proxy LODs; cultural-care review noted in the manifest.

### Wave 6: fantasy sets (≈ 2–3 tasks per set)

F3 dungeons (reuse WFC) → F7 magic materials → F1 high fantasy → F2 dark fantasy → F5
post-apocalyptic (damage system for modern kits) → F4 sci-fi → F6 alien → F9 mythic overlays.
**Exit**: each set's MVK + look + golden vignette.

### Wave 7: characters, creatures, animation (≈ 14 tasks)

MPFB population + clothing generator (6 region/era wardrobes first) → hair cards → skin/eye
materials → CMU mocap cleanup/retarget → locomotion sets → creature assembler (5 body plans) →
procedural gaits → fauna MVK (§4.5) → fantasy creatures (§4.6 F8) → facial set.
**Exit**: 40 human variants + crowd atlas, 30 fauna, 10 fantasy creatures, 300 clips, turntable
goldens, foot-slide metric < 2 cm on locomotion.

### Wave 8: audio + VFX completion (≈ 6 tasks, can run in parallel from Wave 2)

Procedural synth library, IR generator, Freesound CC0 curation, per-biome banks; 60 flipbooks.

### 6.9 Storage footprint estimates

Assumptions: BC7/BC5 = 1 B/px, BC1/BC4 = 0.5 B/px, mips +33 %. A 2048² PBR set (albedo BC7 +
normal BC5 + ORM BC1) ≈ 5.3 + 5.3 + 2.7 = **13.3 MB**; 1024² set ≈ **3.3 MB**. Meshes after
meshopt codecs + zstd ≈ 8–12 bytes/triangle including LODs. Disk sizes are cooked, zstd-compressed
(≈ 0.6–0.7× for BCn with RDO), Standard tier.

| Pack | Contents | Disk (Standard) | Disk (Lite) |
|---|---|---|---|
| core | 190 materials (mix of 2k/1k), 24 detail, 6 layers, humans base, shared props, UI, shared audio | ≈ 1.2 GB | ≈ 350 MB |
| per biome (×21) | 6–10 unique materials, 10–15 species (meshes + atlases + impostors), rock sets (SDF + normals), 20 props, terrain macro + masks (8 km²), ambience bank (≈ 15 MB), look | ≈ 150–250 MB (avg 200) → ≈ 4.2 GB | ≈ 60 MB each → 1.3 GB |
| per urban style (×≈ 30) | trim sheet, 3–5 materials, 60 pieces, 20 props, decals | ≈ 60–100 MB (avg 80) → ≈ 2.4 GB | ≈ 25 MB each → 0.75 GB |
| fantasy sets (×9) | kits + materials + VFX | ≈ 80–150 MB → ≈ 1.0 GB | ≈ 0.3 GB |
| characters + creatures + animation | 40 humans (crowd atlas), 40 creatures, 300 clips (≈ 60 MB) | ≈ 0.9 GB | ≈ 0.3 GB |
| VFX + HDRIs | 60 flipbooks (≈ 3–6 MB each), 20 HDRIs | ≈ 0.6 GB | ≈ 0.2 GB |
| **Total** | | **≈ 10 GB** | **≈ 3.2 GB** |

Asset-cache sources (not shipped, not in git): ≈ 25–40 GB (4k source scans, DEM tiles, Blender
intermediates). The container disk must be checked before Waves 1–4 (`df`), and the cache must be
prunable per wave (`AssetGen cache prune --wave N`).

A single game ships only the packs it uses: e.g. a temperate fantasy RPG ≈ core + 4 biomes + 3
styles + 3 fantasy + characters ≈ 3.5 GB Standard.

---

## 7. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| **Licence contamination** (NC/ND/SA/ODbL sneaking in, mislabelled uploads, API ToS confused with asset licence) | Legal exposure; forced removal of derived packs | Allow-list + `fuse_lint asset-licences` gate with licence propagation along `derived_from`; sha256 pins; forbidden-dataset names list; human review queue for anything not CC0/PD/CC-BY; OSM kept in a separate ODbL layer; research datasets never in the cook. |
| **Uncanny valley** (humans, faces, animation) | Realism backfires | Slightly stylised realism for humans; animation-quality bar (foot-slide, pops) gated; eye/teeth/skin shaders validated on turntables; faces kept small at crowd distance; procedural blink/saccades; prefer masks/helmets/hoods where style allows. |
| **Repetition / tiling** | Scenes look synthetic | Stochastic tiling, macro variation, 3-level layering, per-instance randomness, decals, 3–5 seeds per species/rock, scatter clustering rules, golden-scene "tiling detector" (autocorrelation peak metric on vignettes). |
| **Disk budget** | Packs too large to ship or to cache in the container | Per-class budgets enforced by gates; Lite tier; KTX2/RDO + zstd; shared core; recipes-in-git instead of bytes; cache prune per wave. |
| **CPU-only baking time** (Cycles CPU, impostors, erosion, cloth, fluid sims) | Waves stall | Prefer FUSE-native single-source kernels (C++/SIMD) over Blender for bakes; small bake resolutions and upscaling only where invisible; incremental cook cache (`cook_cache`, content hashes); parallel job graph (`cook_job_graph`); flipbooks at 128–256² per frame with motion vectors; sims at low resolution + noise upres. |
| **Lavapipe fidelity/speed** for goldens and impostor bakes | Slow or slightly different from GPU output | Low golden resolution (960×540), deterministic settings, tolerance thresholds; GPU goldens as a separate optional tier later. |
| **Tool availability** (Blender download through the proxy, GL-dependent tools such as Material Maker) | Blocked pipeline steps | Pinned Blender LTS tarball in the cache with sha256; llvmpipe/xvfb fallback; port critical graphs into FUSE's own baker. |
| **Physically wrong content** (albedo too bright, baked lighting) | DDGI/exposure look wrong everywhere | Calibration gates §1.6; the neutral-look material-ball golden is mandatory. |
| **Cultural misrepresentation** in regional architecture/clothing | Reputational harm | Research notes per style in recipes, generic rather than sacred-specific motifs, review field in the manifest. |
| **Generator drift** (non-determinism across Python/Blender versions) | Goldens churn | Pin tool versions in `sources.lock.json`; determinism gate; seeds in recipes. |
| **Scope creep** ("the whole planet") | Never finished | MVK per biome/style first; priority order in §6; each wave ships something usable. |
| **Neural compression dependence** | Vendor lock-in, untestable here | Optional only; BCn is always the stored form. |

---

## 8. Sources (all accessed 2026-09-23)

- [S1] Poly Haven licence (CC0): https://polyhaven.com/license
- [S2] ambientCG licence (CC0 1.0): https://docs.ambientcg.com/license/
- [S3] ShareTextures licence (custom CC0, redistribution restriction): https://www.sharetextures.com/p/license
- [S4] Kenney support/licence (CC0): https://kenney.nl/support
- [S5] Kenney vs Quaternius (both CC0): https://3dxdev.com/kenney-vs-quaternius-the-best-free-cc0-game-assets/
- [S6] Quaternius CC0 statement: https://x.com/quaternius/status/1559299393177747456 ; https://gamefromscratch.com/quaternius-free-3d-assets/
- [S7] Smithsonian Open Access FAQ: https://www.si.edu/openaccess/faq
- [S8] Smithsonian CC0 release (Creative Commons): https://creativecommons.org/2020/02/27/smithsonian-releases-2-8-million-images-data-into-the-public-domain-using-cc0/ ; 2,000+ 3D models: https://www.cgchannel.com/2020/03/get-2000-free-3d-models-from-the-smithsonian-collection/
- [S9] Sketchfab licence filters: https://sketchfab.com/blogs/community/refine-downloadable-model-searches-with-new-license-filters/ ; Download API guidelines: https://sketchfab.com/developers/download-api/guidelines
- [S10] OpenGameArt licences and mixing caveats: https://opengameart.org/forumtopic/remixing-cc-by-and-cc-by-sa ; https://opengameart.org/forumtopic/source-required-for-art-licensed-under-the-gpl ; https://app.cinevva.com/guides/game-asset-licenses
- [S11] Freesound licences and API licence filter: https://freesound.org/help/faq/ ; https://opensource.creativecommons.org/blog/entries/freesound-intro/
- [S12] CMU Motion Capture Database terms (via mirrors; official http://mocap.cs.cmu.edu returned 503 on access): https://huggingface.co/datasets/gbionics/cmu-fbx ; https://www.re3data.org/repository/r3d100012183
- [S13] Basis Universal UASTC HDR (BC6H transcode): https://github.com/BinomialLLC/basis_universal/wiki/UASTC-HDR-Examples ; https://github.com/BinomialLLC/basis_universal/wiki/UASTC-HDR-6x6-Support-Notes
- [S14] Basis Universal repository: https://github.com/BinomialLLC/basis_universal
- [S15] KTX-Software licence and tools: https://github.com/KhronosGroup/KTX-Software/blob/main/LICENSE.md ; https://github.khronos.org/KTX-Software/ktxtools/index.html
- [S16] NVIDIA RTX Neural Texture Compression SDK: https://github.com/NVIDIA-RTX/RTXNTC
- [S17] meshoptimizer v1.0 (clusterlod.h, partitionClusters): https://meshoptimizer.org/v1.html ; https://github.com/zeux/meshoptimizer/releases
- [S18] Nanite-style DAG in meshoptimizer: https://github.com/zeux/meshoptimizer/discussions/750 ; https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/
- [S19] Octahedral impostors (Ryan Brucks): https://www.yummers.dev/hemi-octahedral-impostors.html ; https://80.lv/articles/new-optimization-solution-amplify-impostors
- [S20] Poly Haven public API (non-commercial API terms, User-Agent, credit): https://github.com/Poly-Haven/Public-API ; https://polyhaven.com/our-api
- [S21] Bandai Namco Research Motion Dataset (CC BY-NC-ND 4.0): https://github.com/BandaiNamcoResearchInc/Bandai-Namco-Research-Motiondataset ; https://www.cgchannel.com/2022/05/download-3000-free-mocap-moves-from-bandai-namco-research/
- [S22] MPFB 2 licence (GPLv3 code, CC0 assets): https://github.com/makehumancommunity/mpfb2/blob/master/LICENSE.md ; https://static.makehumancommunity.org/mpfb/faq/is_it_really_free.html
- [S23] MPFB 2 overview: https://www.cgchannel.com/2025/03/check-out-open-source-blender-character-generation-plugin-mpfb-2/
- [S24] Copernicus DEM GLO-30 licence: https://documentation.dataspace.copernicus.eu/APIs/SentinelHub/Data/DEM/resources/license/License-COPDEM-30.pdf ; https://registry.opendata.aws/copernicus-dem/
- [S25] USGS/NASA public domain and SRTM: https://www.usgs.gov/emergency-operations-portal/data-policy ; https://www.earthdata.nasa.gov/engage/open-data-services-software-policies/data-use-guidance ; https://wiki.openstreetmap.org/wiki/SRTM
- [S26] ESA WorldCover (CC BY 4.0): https://esa-worldcover.org/en/data-access ; https://registry.opendata.aws/esa-worldcover-vito/
- [S27] RESOLVE Ecoregions 2017 (CC BY 4.0): https://ecoregions.appspot.com/ ; https://developers.google.com/earth-engine/datasets/catalog/RESOLVE_ECOREGIONS_2017
- [S28] Beck et al. 2018 Köppen-Geiger 1 km (CC BY 4.0): https://figshare.com/articles/dataset/Present_and_future_K_ppen-Geiger_climate_classification_maps_at_1-km_resolution/6396959
- [S29] OSM Produced Work guideline: https://osmfoundation.org/wiki/Licence/Community_Guidelines/Produced_Work_-_Guideline
- [S30] ODbL use cases (including games): https://wiki.openstreetmap.org/wiki/Open_Data_License/Use_Cases ; https://osmfoundation.org/wiki/Licence/Attribution_Guidelines
- [S31] Blender command-line arguments (5.2 LTS manual): https://docs.blender.org/manual/en/latest/advanced/command_line/arguments.html
- [S32] Sverchok (GPLv3): https://github.com/nortikin/sverchok
- [S33] Sapling Tree Gen extension (GPLv3+): https://extensions.blender.org/add-ons/sapling-tree-gen/
- [S34] Space colonisation tree generator (GPLv3+): https://extensions.blender.org/add-ons/space-colonization-tree-generator/ ; https://github.com/varkenvarken/spacetree
- [S35] Material Maker (MIT): https://github.com/RodZill4/material-maker ; https://www.cgchannel.com/2025/10/material-maker-1-4/
- [S36] NVIDIA FLIP (BSD-3-Clause): https://github.com/NVlabs/flip
- [S37] WaveFunctionCollapse (MIT): https://github.com/mxgmn/WaveFunctionCollapse

Licence facts can change. W0.5 requires the licence gate to record the retrieval date, and each
wave must re-check the terms of any source it adds.
