# RTX Remix format notes for FUSE Remaster

Remaster plan task W2.1 ([`FUSE_REMASTER_PLAN.md`](../plans/FUSE_REMASTER_PLAN.md) §2.5, §7 Wave 2). Written 2026-11-05.

This page pins the parts of the RTX Remix formats that FUSE Remaster reads and writes:

- the hash rules and how hashes are spelled in prim names;
- the capture layout;
- the mod (replacement) layer layout;
- the material inputs;
- the DDS conventions.

It is shared with [`FUSE_REMIX_PORT_PLAN.md`](../plans/FUSE_REMIX_PORT_PLAN.md) §4, the FUSE Relight port. Neither plan duplicates the other's code.

**Sources.** No network access was used. Every fact here comes from code that is already in the tree. That code reproduces dxvk-remix at commit `0867d3c` and was read for interface facts only.

| Topic | In-tree source | Upstream file it follows |
|---|---|---|
| Hashes | `Source/FUSE/Relight/hash/` (known-answer tests, Python reference `Tools/FUSE/Relight/remix_hash_ref.py`) | `rtx_geometry_utils`, `rtx_utils.h`, `d3d9_common_texture.cpp`, `rtx_lights.cpp` |
| Capture writer | `Source/FUSE/Relight/capture/export/` (`usda_writer`, `dds`, `poco_store`) | `lssusd/game_exporter.cpp`, `game_exporter_paths.h` |
| Mod reader | `Source/FUSE/Relight/mods/usd/` (`remix_profile`, the Remix-profile composition on `vendor/tinyusdz`) | `rtx_mod_usd.cpp` |
| Material tables | `Source/FUSE/Relight/mods/import/material_table.*` | `rtx_material_data.h` |
| DDS reader | `Source/FUSE/Relight/mods/assets/dds.*` | (FUSE's own) |

Anything that would need the Toolkit or upstream source we do not have is listed as an open question (§8). It is not guessed.

**Fixtures.** All of them are ours: handcrafted, synthetic, and with no game content.

| Fixture | What it is |
|---|---|
| `Tests/fixtures/remaster/remix/capture_basic/` | The capture layout of §2: 2 meshes, 2 materials, 1 light. DDS files are generated at test time from `gen_textures.txt`. `expected/ingest.json` holds the oaid set. |
| `Tests/relight/fixtures/mods/*` | 26 USD composition fixtures |
| `Tests/relight/fixtures/mods_import/remix_basic` | A Toolkit-style replacement layer |
| `Tests/relight/fixtures/mods_import/capture_layout` | A Relight capture |

Gates:
- `remaster_remix` (ingest, export, re-ingest, lint);
- `rl_mods_import_*`, `rl_mod_*`, `rl_capture_export_*` (`ctest -L relight`).

## 1. Hash rules

### 1.1 Geometry

Remix computes nine geometry components in a fixed order (enum order):
`positions`, `legacypositions0`, `legacypositions1`, `texcoords`, `indices`, `legacyindices`, `geometrydescriptor`, `vertexlayout`, `vertexshader`.

The per-component algorithms are normative in [`FUSE_REMIX_PORT_PLAN.md`](../plans/FUSE_REMIX_PORT_PLAN.md) §4.1.1. In short:
- XXH3 over the sorted unique rebased vertices or over the indices;
- the descriptor hashes the counts and the Vulkan topology and index type;
- legacy positions are floored with the SSE4.1 reciprocal path.

A **rule** is a comma list of component names. The rule hash takes the first selected component as-is, then folds each later one with `h = XXH64(&component, 8, h)`.

| Rule | Value | Used for |
|---|---|---|
| `rtx.geometryAssetHashRuleString` | `positions,indices,geometrydescriptor` (default) | The **mesh replacement key**, `mesh_<H>` |
| `rtx.geometryGenerationHashRuleString` | `positions,indices,texcoords,geometrydescriptor,vertexlayout,vertexshader` | Which components are computed at all |
| Legacy | `legacypositions0,legacyindices` | Older mods; work when the game's `rtx.conf` selects them |

- The capture records the asset rule in `customLayerData.lightspeed_geometry_hash_rules`.
- Each mesh layer's `customLayerData` holds the nine raw component values as `uint64`.
- FUSE stores a geometry key with `rule_id = XXH3(rule string)`. The DB key namespaces are `remix.geom.asset`, `remix.geom.legacy0` and `remix.geom.legacy1`.

### 1.2 Texture, material and light hashes

- **Texture.** XXH3-64 of mip 0 in Remix's packed layout:
  - `rows = blocksHigh × depth`;
  - `rowBytes = align(elementSize × blocksWide, 4)`;
  - formats from the fork's D3DFORMAT → VkFormat table.

  It is taken at the first upload of subresource 0. `rtx.useObsoleteHashOnTextureUpload` switches it to XXH64 (DB namespace `remix.tex.obsolete`).
- **Material.** The legacy material hash is the stage-0 colour texture's hash, so `mat_<H>` uses the texture hash (DB `remix.tex`). Untextured draws have material hash 0 and cannot be material-replaced.
- **Light.** XXH64 chained over type, position, radius and the shaping hash (DB `remix.light`). Spot, rect and disk lights follow the same pattern.
- **Canonical link.** FUSE adds a `fuse.capture.sha256` key on the same oaid whenever the payload is readable:
  - meshes: the W0.2 canonical mesh stream (`u32` vertex count, `u32` index count, positions as `f32×3`, indices as `u32`, then the other attributes in semantic order);
  - textures: the sha256 of the decoded RGBA8 mip 0.

  This is the remix64 ↔ sha256 link that W2.3 checks.
- **Check on the fixture.** The fixture's `mesh_5EED000000000001` key `7e5edef0…b6f7` was recomputed independently in Python from the quad's positions and indices (positions-only stream). It matches what the gate pins.

### 1.3 How a hash is spelled

- `hashToString`: 16 **upper-case** hex digits, zero-padded, no prefix.
- Prim names are `<prefix><hashToString(h)>`:

  | Prefix | Section |
  |---|---|
  | `mesh_` | meshes |
  | `mat_` | materials |
  | `light_` | lights (legacy name `sphereLight_`) |
  | `skel_` | skeletons |

- The runtime parses a name with `strtoull(name + len(prefix), 16)`. Lower-case digits are therefore accepted on read, and a name that parses to 0 is not a replacement. FUSE always writes upper case.
- `rtx.conf` hash lists use `0x` + 16 upper-case digits.

### 1.4 oaid

FUSE's original-asset id comes from the asset's first key:

`oaid = UUIDv5(UUIDv5(NameSpace_URL, "https://fuse.invalid/remaster/oaid"), "<game>/<algo>:<value>")`

`<game>` is the sanitised game id (`[a-z0-9_-]`). The rule is pinned against Python's `uuid5` by `remaster_db`. The `remaster_remix` expected set was rechecked with Python `uuid5` when it was generated.

## 2. Capture layout

The capture is written by `GameCapturer` and `lssusd::GameExporter`. FUSE's port is `capture/export/usda_writer.cpp`.

| File | Contents |
|---|---|
| `<stage>.usda` (for example `capture.usda`; the Relight fixture names it `mod.usda`) | `defaultPrim = "RootNode"`, plus `metersPerUnit` and `upAxis` from the game. `customLayerData` holds `lightspeed_layer_type = "capture"`, `lightspeed_game_name`, `lightspeed_exe_name`, `lightspeed_game_icon`, `lightspeed_geometry_hash_rules`, and `cameraSettings.boundCamera`. |
| `/RootNode/lights/light_<H>` | `SphereLight` (with `ShapingAPI`) or `DistantLight`, referencing `./lights/light_<H>.usda` |
| `/RootNode/meshes/mesh_<H>` | `Xform` (a `SkelRoot` when skinned), `visibility = "invisible"`. It references `./meshes/mesh_<H>.usda</mesh_<H>>`, has `rel material:binding = </RootNode/Looks/mat_<T>>` and the `MaterialBindingAPI`. |
| `/RootNode/Looks/mat_<T>` | `Material` referencing `./materials/mat_<T>.usda</Looks/mat_<T>>` |
| `/RootNode/instances/inst_<M>_<n>` (sky instances: `sky_<M>_<n>`) | Internal reference to the mesh, time-sampled `xformOp:transform` and visibility, and `mesh/_remix_metadata:*` primvars |
| `/RootNode/cameras/Camera` | The capture camera |
| `meshes/mesh_<H>.usda` | `/mesh_<H>/mesh`, a `UsdGeomMesh`: `points`, `faceVertexCounts`, `faceVertexIndices`, `normals`, `primvars:st` (vertex interpolation), `displayColor` / `displayOpacity`, skinning primvars, and the `remix_category:*` bools. The layer's `customLayerData` holds the nine hash components. |
| `materials/mat_<T>.usda` | `/Looks/mat_<T>/Shader`. `info:mdl:sourceAsset = @./AperturePBR_Opacity.mdl@`, `inputs:diffuse_texture = @../textures/<T>.dds@` with `colorSpace = "auto"`, plus `filter_mode`, `wrap_mode_u` / `wrap_mode_v` and `enable_opacity`, written unprefixed as upstream does. |
| `lights/light_<H>.usda`, `skeletons/skel_<H>.usda` | One layer per light and per skeleton |
| `textures/<T>.dds` | Captured albedo textures. DX9 header, BC blocks passed through (§5). |

Units come from the stage: `metersPerUnit` is game units to metres (for example 0.0254 for inch-based games), and `upAxis` is usually `"Y"`.
- `ingest-remix` records the capture's `metersPerUnit` in `remaster.project.json` under `remix.meters_per_unit`.
- `export-remix` scales our metre-based meshes back by `record.metersPerUnit / game.metersPerUnit`, using an `xformOp:scale` on the `ref_` prim.

Captures are originals. They stay in `build/remaster-cache/<game>/capture/` (§0.1.2). Ingest records them as `origin original`, `LicenseRef-Original-<game>` and `distribution never`.

## 3. Mod (replacement) layer layout

Mods are discovered at `<game>/rtx-remix/mods/<Mod>/mod.{usda,usdc,usd}`, ordered by layer priority and then by name. A per-mod `rtx.conf` is the mod's options layer.

A replacement layer has `customLayerData.lightspeed_layer_type = "replacement"`. It speaks only in `over`s and `def`s under `/RootNode`, and the runtime classifies prims by name alone:

| Prim | Meaning |
|---|---|
| `/RootNode/meshes/mesh_<H>` | Mesh replacement. Its active descendant `UsdGeomMesh` prims (usually a `def Xform "ref_<name>"` that references our mesh layer) replace the draw. `int preserveOriginalDrawCall` keeps the original draw. A `rel material:binding` may bind a replacement material. Lights below it are attached lights. |
| `/RootNode/Looks/mat_<T>` | Material replacement: a `Shader` with `info:mdl:sourceAsset` and `inputs:*`. A material without the prefix is keyed by its shader's strongest opinion site; FUSE never writes those. |
| `/RootNode/lights/light_<H>` | Light replacement or deletion (legacy name `sphereLight_<H>`) |
| `remix_category:<name>` | Bool attributes. Authoring one sets the category's "exists" bit. |
| `ParticleSystemAPI` | Applied schema; `primvars:particle:*` attributes |

Composition supported in the Remix profile:
- sublayers;
- references and payloads (list ops, namespace mapping);
- `over`s;
- variant sets;
- instanceable prims, flattened.

Inherits, specializes, relocates, value clips and layer offsets are warned about and ignored (`Source/FUSE/Relight/mods/usd/include/fuse/relight/mods/usd/usd_stage.hpp`).

**What `export-remix` writes.** Only our assets. The capture is never sublayered or referenced, and no original payload is copied (the lint in `checkExport` enforces this).

| File | Contents |
|---|---|
| `mod.usda` | The `replacement` layer: `over "mesh_<H>"` with `preserveOriginalDrawCall = 0` and a `def Xform "ref_<poco>"` that references `./assets/meshes/<poco>.usda` with translate and scale ops. It sublayers `./materials/materials.usda` when there are materials. |
| `materials/materials.usda` | `def Material "mat_<T>"` under `/RootNode/Looks`, an `AperturePBR_Opacity` shader (§4) |
| `assets/meshes/<poco>.usda` | One `UsdGeomMesh` per replacement mesh: points in the record's units, triangles, normals and `primvars:st` |
| `assets/textures/<poco>_<slot>.dds` | §5 |
| `remaster-export.json` | Schema `fuse.remaster.remix-export/1`. Per replacement: `oaid`, `kind`, `remix_hash`, `prim`, `poco`, `distribution` (`ship` or `local-only`), `mesh_layer` and `textures`. The W2.5 Toolkit client consumes it. |

## 4. Material inputs

There are three MDL surfaces, selected by `info:mdl:sourceAsset`:
- `AperturePBR_Opacity.mdl` (opaque);
- `AperturePBR_Translucent.mdl`;
- `AperturePBR_Portal.mdl`.

An unknown or missing shader gives opaque defaults and a warning. The full parameter tables, with ranges and defaults copied verbatim from `rtx_material_data.h`, are in `material_table.cpp`.

Opaque, texture inputs:

| Input | Colour space (`colorSpace`) |
|---|---|
| `diffuse_texture` | sRGB (`"auto"`) |
| `normalmap_texture` | raw |
| `tangent_texture` | raw |
| `height_texture` | raw |
| `reflectionroughness_texture` | raw |
| `metallic_texture` | raw |
| `emissive_mask_texture` | sRGB |
| `subsurface_transmittance_texture`, `subsurface_thickness_texture`, `subsurface_single_scattering_texture`, `subsurface_radius_texture`, `secondary_texture` | raw |

Opaque, constants (defaults):
- `diffuse_color_constant` 0.2;
- `opacity_constant` 1;
- `reflection_roughness_constant` 0.5;
- `metallic_constant` 0;
- `emissive_intensity` 40, range [0, 65504];
- `emissive_color_constant` (1, 0.1, 0.1);
- `enable_emission` false;
- `anisotropy` 0;
- thin film: `enable_thin_film`, `thin_film_thickness_constant` 200;
- sprite sheet: rows, cols and fps, all 0;
- blend and alpha test: `use_legacy_alpha_state` true, `blend_enabled` false, `blend_type` alpha, `inverted_blend`, `alpha_test_type` always, `alpha_test_reference_value` 0;
- displacement: `displace_in` 0.05, `displace_out` 0;
- subsurface: transmittance colour, measurement distance, single-scattering albedo, volumetric anisotropy, diffusion profile, radius and radius scale, max sample radius;
- sampler: `filter_mode` linear, `wrap_mode_u` / `wrap_mode_v` repeat;
- the DLSS control-mask fields.

Translucent adds `ior_constant` 1.3, `transmittance_color` 0.97, `transmittance_measurement_distance` 1, `thin_walled`, `thin_wall_thickness` and `use_diffuse_layer`.

Portal has `emissive_mask_texture`, `portal_index` and `rotation_speed`.

`export-remix` maps the POCO records as follows:

| POCO field | AperturePBR input |
|---|---|
| `Material.baseColor` (rgb) | `diffuse_color_constant` |
| `Material.roughness` | `reflection_roughness_constant` |
| `Material.metallic` | `metallic_constant` |
| `TextureSet.albedo` | `diffuse_texture` (`"auto"`) |
| `TextureSet.normal` | `normalmap_texture` (`"raw"`) |
| `TextureSet.emissive` | `emissive_mask_texture` |
| `TextureSet.height` | `height_texture` |

ORM is not exported yet. AperturePBR wants separate roughness and metallic maps, so ORM needs a channel split, which is Wave 3 texture-pipeline work. Relight's importer handles the inverse mapping and the `material_ext` record for Remix-only parameters (port plan §4.4).

## 5. DDS conventions

- **Captures** (`capture/export/dds.hpp`):
  - the classic DX9 header with no DX10 extension;
  - DXT1-5 and ATI1/2 as the game's own BC blocks (BC passthrough), other formats in their D3DFORMAT texel layout;
  - format by FourCC or bit masks, as D3DX writes them;
  - rows use the DDS pitch (`elementSize × blocksWide`).

  Re-packing gives the canonical bytes whose XXH3 is the texture hash, so a capture round-trips its own keys.
- **Mods** (FUSE's reader accepts a superset):
  - DX9 or DX10 headers;
  - BC1–BC7 including BC6H, RGBA8 / BGRA8 (and sRGB), R16F and R32F;
  - mips, arrays, cubes and volumes.

  BCn blocks are uploaded without re-encoding. The sRGB decision follows the `colorSpace` hint or the parameter's colour space.
- **What `export-remix` writes:**
  - a DX10 header;
  - albedo and emissive as **BC7** (`BC7_UNORM_SRGB` when the albedo is sRGB);
  - normal as **BC5** (R, G);
  - a full box-filtered mip chain;
  - encoders from `Tools/FUSE/Cook`.

  Normal maps are written in the OpenGL (+Y) convention: a `TextureSet.normalConvention = DX` record has its green channel flipped. See §8 Q3.

## 6. Mapping to POCO and the replacement DB

- **Ingest** (`fuse_remaster ingest-remix`) uses Relight's mod importer in capture mode. Per captured prim:

  | Captured prim | POCO records | DB keys |
  |---|---|---|
  | `mesh_<H>` | `mesh` | `remix.geom.asset:<H>` (rule-qualified) and `fuse.capture.sha256` of the canonical stream, one oaid |
  | `mat_<T>` | `material`, `material_ext`, `texture_set` (albedo = the DDS blob) | `remix.tex:<T>` and `fuse.capture.sha256` of RGBA8 mip 0 |
  | `light_<H>` | `light` | `remix.light:<H>` |
  | the capture as a whole | `relight_mod` | — |

  Records go into the project's blob store and `poco/`. Keys go into `remaster.db` and the canonical `remaster.lock.json`. Re-ingest is idempotent.
- **Export** (`fuse_remaster export-remix`) reads the replacement rows with `tier = remix` and `variant = default`:
  - an oaid with a `remix.geom.asset` key becomes a mesh replacement;
  - an oaid with a `remix.tex` key becomes a material replacement.

  Records of originals are refused. Recipe-only derivatives are refused unless `--local` is given, in which case the manifest marks them `local-only`.
- **Check** (run by `export-remix`, `checkExport`):
  1. The manifest schema.
  2. Every DDS reads back as BC7 or BC5.
  3. The composed `mod.usda` classifies every replacement with its hash and has no error diagnostics.
  4. A re-ingest of the mod (Relight importer, mod mode) has a replacement row for every exported oaid.
  5. The lint: no file is a blob of an original, never or recipe-only record; no BC7 texture decodes to a captured original's pixels; every USD asset path stays inside the mod folder (the AperturePBR MDL modules are referenced by name only); no stray files.

## 7. The §2.5 [VERIFY] items, resolved

| Item | Resolution |
|---|---|
| Exact prim paths | **Resolved** (§2, §3): `/RootNode/{meshes,Looks,lights}/{mesh_,mat_,light_}<H>`. Only children of the three sections count. Legacy `sphereLight_`. Capture layers sit under `meshes/`, `materials/`, `lights/` and `skeletons/`. Source: `remix_profile.hpp` and `usda_writer.hpp`, which follow `rtx_mod_usd.cpp` and `game_exporter_paths.h`. |
| Hash rule | **Resolved** (§1): the component list, the rule strings and their defaults, the fold, and the texture, material and light hashes, with known-answer tests in `Source/FUSE/Relight/hash` |
| Hash spelling | **Resolved** (§1.3): 16 upper-case hex digits, parsed with `strtoull(…, 16)` |
| Capture layout | **Resolved** (§2) |
| Mod layer layout | **Resolved** for what the runtime reads (§3). How the Toolkit names files inside a mod is an open question (Q1). |
| Material inputs | **Resolved** (§4): `material_table.cpp` carries the upstream tables |
| Texture suffixes (`*.a.rtex.dds`, also the W0.6 binary-gate pattern) | **Open question Q1.** The runtime reads any DDS path a layer names, so FUSE does not depend on the suffix. |
| DDS conventions | **Resolved** for the runtime reader and the capture writer (§5). The Toolkit's ingest encodings are open (Q2, Q3). |

## 8. Open questions

Each needs the toolkit-remix source or a running Toolkit. Neither is available offline, and the Toolkit runs on Omniverse Kit, which is not FOSS. None of them blocks W2.3 or W2.4.

- **Q1. Toolkit ingest naming.** The Toolkit's ingest output naming, including the `.a.rtex.dds` / `.n.rtex.dds` style suffixes and its `ingested/` folder layout, is not pinned. FUSE keeps `*.rtex.dds` in the binary gate as a conservative pattern and writes its own `<poco>_<slot>.dds` names.
- **Q2. Octahedral normal maps.** Whether the Toolkit converts normal maps to an octahedral encoding at ingest, and how a layer marks the encoding (an `inputs:encoding`-style parameter is not in the ported 0867d3c tables).
- **Q3. Normal-map convention.** The tangent-space convention the runtime expects (OpenGL +Y is assumed, as in port plan §4.5). This needs a parity render on the 3090 (W2.6, [WIN/RTX manual]).
- **Q4. Toolkit REST routes.** The REST routes and payloads behind the W2.5 client. Every route in `fuse_ai/remix_toolkit.py` carries `[VERIFY against toolkit-remix]`, and the mock server defines our contract until then.
- **Q5. `.usdc` output.** Whether stock Remix accepts a mod whose mesh layers are `.usda` while `mod.usda` sublayers `.usda` materials. Relight does. Stock-Remix parity is W2.6.
