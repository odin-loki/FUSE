# FUSE Remaster Plan: importing legacy games and upgrading them with RTX Remix, Gemini, world models and POCO assets

Status: plan, not yet started. Author date 2026-09-23. Companion to
[`FUSE_ASSET_PLAN.md`](FUSE_ASSET_PLAN.md) (formats, licence lock, budgets, gates),
[`FUSE_RENDERER_PLAN.md`](FUSE_RENDERER_PLAN.md) (tiers T0–T3, golden images),
[`FUSE_MASTER_PLAN.md`](FUSE_MASTER_PLAN.md) and the licensing research in
[`../research/upscaling-framegen-and-post-injectors.md`](../research/upscaling-framegen-and-post-injectors.md).

**Goal.** Build a *remaster stack* for FUSE. It takes an old game that the user owns and either
(A) **wraps it at runtime**, running the original executable through NVIDIA RTX Remix (or, later, a
FUSE-owned D3D8/9 capture layer) with upgraded replacements, or (B) **imports and ports it**,
converting its assets, levels and logic into native FUSE content. Both tracks use one library of
**POCO assets** (plain data records with no behaviour) and one **replacement database** keyed by
content hash. Upgrades come from the FUSE asset library, new procedural or hand-authored content,
local AI models, and optionally cloud AI (Gemini, Nano Banana image models, Veo). Everything goes
through reproducible recipes and human review.

**The plan in one sentence.** Legacy data comes in through importers or capture and is hashed.
Upgrades are produced as plain-data POCO assets with provenance and licence records. The same POCO
asset is emitted either as a Remix USD mod layer (track A) or as FUSE cooked content (track B).
We only ever distribute our own content and recipes, never the original game's files.

Markers used in this document: **[VERIFY]** means a fact that must be re-checked against upstream
source or docs in the first task that depends on it. **[SPECULATIVE]** means a design bet or estimate
that has not been proven. All web sources were accessed on 2026-09-23 (§9).

---

## 0. Goals, non-goals and the legal/IP policy

### 0.1 Legal and IP policy (binding for every task in this plan)

> **Not legal advice.** This policy is deliberately stricter than common modding practice. Any
> exception needs a written review by the project owner, recorded in the licence lock (§2.6).

1. **Only games the user owns or has rights to.** The remaster tooling runs against a game install
   (or disc image) that the user supplies. FUSE never downloads, bundles or points to copies of
   commercial games. Test fixtures in the repository are **synthetic**: legacy-format files that
   FUSE generates itself (for example a DTS or DIF written by our own test writer), or content that
   is CC0 or MIT (for example Torque3D's own MIT sample art already in the tree).
2. **No redistribution of original copyrighted assets.** Nothing the game's authors made enters git,
   a release pack, a CI artefact, a bug report or a cloud API, unless point 5 allows it. That covers
   textures, meshes, audio, text, scripts, levels and captures. Captures and extracted files live in
   a git-ignored per-game cache (`build/remaster-cache/<game-id>/`).
3. **Ship-as-patch model.** A distributable remaster contains only:
   - (a) **our own content**: FUSE library assets, newly authored assets, and AI outputs generated
     from *our own* prompts and references with no original asset as input;
   - (b) **recipes**: deterministic instructions that turn the user's original files into upgraded
     files *on the user's machine*;
   - (c) **deltas and mappings**: hash → replacement tables, entity mappings, rewritten logic in
     FUSE Lua or Remix Logic graphs. These must not embed original data. Hashes and names are fine;
     embedded original geometry, pixels or script text is not.
4. **Derivatives of originals are recipe-only.** An AI-upscaled texture, a PBR map inferred from an
   original albedo, a retopologised original mesh and a cleaned-up original sound are *derivative
   works* of copyrighted material. We **do not distribute these files**. We distribute the recipe,
   and the user's machine rebuilds them from the user's own files. This is why local deterministic
   models (§3.4) matter: a recipe that needs a cloud call only works for users who opt in and bring
   their own API key. Exception: written permission from the rights holder, recorded as
   `licence: "LicenseRef-RightsHolderGrant-<id>"` with the grant document's sha256.
5. **Cloud AI and user-owned proprietary assets: off by default.** No original asset, capture,
   screenshot or script is sent to any cloud API (Gemini, Veo, Genie, World Labs and so on) unless
   the per-game project file sets `"cloud_upload": "opt-in"` *and* the user confirms it in the
   editor or CLI once per game. Even then:
   - only **paid-tier** Gemini API keys are allowed for original content. On the unpaid tier, Google
     uses prompts and responses to improve its products and human reviewers may read them. On the
     paid tier it does not [M3].
   - the upload log (hash, provider, model, time, purpose) goes to the per-game audit file.
   - the Genie/Project Genie consumer product is **never** given original assets (it is a consumer
     research prototype, not a data-processing service; §3.2).
6. **AI output licensing per provider** (recorded per output in the licence lock, §2.6):

   | Provider | Output ownership / terms | Data use | Constraints | Class in `licences.lock.json` |
   |---|---|---|---|---|
   | Gemini API (text, code), paid tier | Google claims no ownership of generated content, but "may generate the same or similar content for others" [M3] | Paid tier: not used to improve products; logs kept for abuse detection and legal compliance [M3] | 18+; for professional/business use; must not be used to build competing models [M3]; users in the EEA, Switzerland and the UK may only use paid services [M3]; Australia is a supported region [M4] | `LicenseRef-AI-Gemini` + model id + prompt hash |
   | Gemini API, **unpaid tier** | as above | **Used to improve Google products; human review possible** [M3] | Our own content only, never originals | same, flagged `tier: free` |
   | Nano Banana image models (`gemini-3.1-flash-image`, `gemini-3-pro-image`) | as Gemini API | as tier | Every image carries an invisible **SynthID** watermark [M5]; prohibited-use policy applies | `LicenseRef-AI-Gemini-Image` |
   | Veo 3.1 (`veo-3.1-generate-preview`) | as Gemini API; preview model | as tier | SynthID watermark; no realistic depictions of identifiable real people without consent [M7] | `LicenseRef-AI-Veo` (reference/cutscene only) |
   | Project Genie (Genie 3) | Terms of the consumer product (Google AI Ultra); no API [G1] | Consumer product | US only, 18+, 60-second sessions; video download only [G1] | `LicenseRef-AI-Genie-Reference`, **never shipped** (reference only) |
   | Local models (§3.4) | Depends on model licence *and* training-data licence | Local | See the allow-list in §3.4 | model SPDX id + `training_data` field |

   **Copyrightability caveat.** The US Copyright Office's January 2025 report says that purely
   AI-generated material made from prompts alone is not copyrightable, while assistive use that
   keeps human authorship is [L1]. The lock therefore records a `human_authorship` field
   (`none | selection | substantial`). Assets marked `none` are treated as uncopyrightable,
   unprotected content: fine to ship, but not something we can enforce rights in.
7. **Same binary rules as the NVIDIA plugin.** The RTX Remix runtime is MIT-licensed source [R1][R2],
   so vendoring is *legally* possible. We still treat it as an **external, optional, runtime-only
   component**, the same way we treat DLSS in `docs/nvidia-plugin.md`. The reasons:
   - it is Windows-only and RTX-only [R5];
   - it is large;
   - it pulls NVIDIA SDK components (DLSS, NRD, RTXDI, NRC) that stay under their own NVIDIA
     licences [R11];
   - and the Toolkit is built on Omniverse Kit **[VERIFY Kit licence]**.

   No Remix binary (`d3d9.dll` from dxvk-remix, `d3d8to9`, bridge `NvRemixLauncher32.exe`/`.trex/`,
   `remixapi.dll`, Kit apps) is ever committed. An extended `nvidia_binary_gate.cmake` pattern set
   enforces this (§7, W0.6).
8. **Voice and likeness.** No cloning of original voice actors' voices. The 2025 SAG-AFTRA
   Interactive Media Agreement requires separate written consent and payment per generated line for
   vocal digital replicas [L2]; and Gemini TTS exposes voice replication [M1]. AI voice is allowed
   only for (a) our own placeholder VO with synthetic, non-cloned voices, clearly labelled, or (b)
   actors who give written consent. No real-person faces (the asset plan's rule, §2.3 there).
9. **EULAs and anti-circumvention.** Many game EULAs forbid reverse engineering. Interoperability
   exceptions exist in some jurisdictions, but their scope varies. Track A (runtime wrap) is plain
   API interposition, which is Remix's own model. Track B (format import) reads data files. **We do
   not break DRM or encryption, and we do not ship decryptors or keys.** Games whose data is
   encrypted are out of scope unless the rights holder provides a tool.

### 0.2 Goals

1. **G1:** a user can point FUSE at an owned Torque-era game (TGE/TGEA/T3D content: `.dts`, `.dsq`,
   `.dif`, `.mis`, `.cs`) and get a playable FUSE project with upgraded content (track B). This is
   the first target because FUSE descends from Torque3D and already has converters (§6).
2. **G2:** for DX8/9 fixed-function games, a user can run the original through RTX Remix and use
   FUSE to author, AI-upgrade, review and export a Remix mod layer (track A). CI never needs Windows
   or an RTX GPU.
3. **G3:** one POCO asset model and one replacement database serve both tracks, the FUSE asset
   library and newly generated content.
4. **G4:** every AI-derived asset can be rebuilt from a recipe, or replayed from a cache in CI,
   with its cost, model version and reviewer recorded.
5. **G5:** QA uses golden captures and the renderer's existing FLIP/SSIM metrics
   (`Source/FUSE/Renderer/include/fuse/renderer/quality/image_metrics.hpp`).

### 0.3 Non-goals

- Replacing a game engine at runtime with a world model. Genie 3 runs at about 720p/24 fps, keeps
  worlds consistent for minutes (Project Genie caps sessions at 60 s), and has no API [G1][G2][G3].
  It is a previs and reference tool only (§4.7).
- Shader-model games (most DX9.0c and later, DX10+, OpenGL). Remix cannot reconstruct scenes from
  arbitrary shaders [R6]. For these, track B (data import) is the only path.
- Decrypting or unpacking DRM-protected archives (§0.1.9).
- Console ROM/ISO pipelines. They may come later, only for user-dumped media, and they are not
  planned here.
- Hosting or distributing mods. We produce patch packages and the user chooses where to publish.
- Automatic, unreviewed shipping of any AI output.

---

## 1. Architecture: two tracks, one asset database

```
                    user-owned game install (never leaves the machine unless opted in)
                                   |
             +---------------------+----------------------+
             |                                            |
   TRACK A: runtime wrap                        TRACK B: import / port
   original .exe                                importers (DTS/DSQ, DIF, MIS/CS,
     -> RTX Remix runtime (external, Windows)   Assimp formats, BSP, NIF...)
        or FUSE D3D8/9 capture proxy (later)       -> POCO assets (+ source hashes)
     -> capture (USD + DDS) or FUSE capture        -> scenes (.fuselevel)
        -> remix_import -> POCO assets            -> logic -> FUSE Lua (Gemini-assisted)
             |                                            |
             +--------------> replacement DB <------------+
                     (original-asset id <-> hash keys <-> POCO replacement)
                                   |
                     upgrade pipelines (§4): library pick, procedural,
                     local AI, cloud AI (opt-in), human authoring, review
                                   |
             +---------------------+----------------------+
             |                                            |
   emit Remix mod layer (mod.usda + DDS)        cook FUSE content (FMSH v2, .fusetex,
   + Remix Logic graphs                         .fusemat, .fuseanim, .fusebank, .fuselevel)
   -> ship as patch (our content + recipes)     -> run in FUSE renderer T0–T3
```

### 1.1 Track A: runtime wrap

- **A1, RTX Remix (primary).** The user installs the Remix runtime (dxvk-remix `d3d9.dll`,
  including the bridge for 32-bit games; the separate `bridge-remix` repo was archived on
  2025-05-05 and merged into dxvk-remix [R3]). They capture scenes with Remix, and Remix writes
  OpenUSD captures [R6]. FUSE tools read the capture (USD via TinyUSDZ/LightUSD, §2.5), register
  every captured texture and mesh hash in the replacement DB, produce upgrades as POCO assets, and
  **write a Remix mod layer** (USD) that the Remix runtime loads. The Remix Toolkit's REST API [R12][R13]
  is an optional live link on Windows. FUSE can push replacements into an open Toolkit project the
  way the ComfyUI connector does [R15].
- **A2, FUSE D3D8/9 capture proxy (later, optional, [SPECULATIVE]).** A small MIT `d3d9.dll` /
  `d3d8.dll` proxy owned by FUSE that only **captures**. It records fixed-function draw state,
  vertex and index buffers, textures, transforms and lights into a FUSE capture file (POCO +
  hashes). It does not path trace. Its value:
  - hash parity checks against Remix;
  - games where Remix is unstable;
  - a route into track B (captured geometry becomes the starting point for an import when no format
    importer exists);
  - platforms without RTX.

  Runtime *replacement* inside the proxy (swapping textures and meshes as the game draws them) is a
  later stretch goal. It re-implements a small part of Remix, and we should only do it if Remix does
  not cover a case we need.

  The proxy's recording logic is split from the COM plumbing, so a synthetic D3D9 command-stream
  mock can test it on Linux and CPU only (the NVIDIA mock pattern).
- **Logic in track A** stays in the original executable. Upgrades react to game events through Remix
  Logic: more than 30 event types and about 900 triggers, without source code, since Remix 1.3 in
  January 2026 [R8][R9]. FUSE can author these graphs from its own event mapping [VERIFY file format
  of Logic graphs].

### 1.2 Track B: full import / port

- **Importers** turn legacy files into POCO assets plus a *source map* (legacy path, legacy object
  id, content hash). The importer matrix is in §6. The first importers reuse code already in the
  tree: Torque's `TSShape` reader (`Engine/source/ts/tsShape.cpp`, which reads old DTS versions
  through `tsShapeOldRead.cpp`), `fuse_convert` (`.mis`/`.cs` → `.fuselevel`) and `fuse_import`
  (dry run).
- **Levels become FUSE scenes**: `.fuselevel`, reusing `world_converter` and extending its entity
  mapping.
- **Logic is re-implemented**, not emulated. TorqueScript or other legacy scripts are parsed to an
  AST, summarised, and translated into FUSE Lua (`Source/FUSE/Script`, sandboxed `ScriptVM`) with
  Gemini help and human review (§4.5). The existing `t3d:`/`t2d:` legacy chunk routes in
  `legacy_script_route.hpp` stay available as a compatibility bridge for scripts not yet ported.
- **Output** is ordinary FUSE content. It is cooked with `fuse_cook`, checked by the asset plan's
  gates (§5.3 there), and rendered on T0–T3.

### 1.3 The shared replacement database

The two tracks meet in one table set. The file is `remaster.db`, SQLite, which is already vendored
in `Engine/lib/sqlite`. A canonical JSON export, `remaster.lock.json`, is committed for *our*
projects and diffed in review.

| Table | Key | Content |
|---|---|---|
| `original_asset` | `oaid` (UUIDv5 of game-id + first strong hash) | kind (texture, mesh, material, sound, anim, entity, script), game id, **no payload** |
| `hash_key` | (`algo`, `value`) → `oaid` | Several keys per original: `sha256` of canonical decoded data (RGBA8 mip0 for textures, canonical vertex/index stream for meshes, PCM for sound); `remix64` (Remix's 64-bit texture/geometry hash, algorithm pinned in W2.1 [VERIFY]); `fuse-capture64` (FUSE proxy); `legacy-path` (normalised VFS path + archive); `dts-node` (shape sha256 + detail/mesh index) |
| `replacement` | (`oaid`, `variant`, `tier`) → `poco_id` | variant = `default`, `lowspec`, `stylised`…; tier = T0–T3 or `remix` |
| `recipe` | `recipe_id` | the recipe that produced a replacement (§3.3), status `draft/approved/rejected` |
| `review` | `recipe_id` | reviewer, date, decision, FLIP/SSIM numbers, notes |

Rules:

1. Every importer and capture path writes `hash_key` rows. A texture seen by Remix at runtime and the
   same texture found in the game's archive by a track B importer resolve to the same `oaid`, as
   long as their canonical sha256 matches. Remix hashes are linked to the canonical hash when the
   capture includes the pixel data (Remix captures include textures [R6]).
2. A replacement is written once as a POCO asset and emitted to either backend (§2.5).
3. The DB never stores original payloads. The per-game cache holds those, git-ignored.

---

## 2. The POCO asset model

"POCO" here means plain old data: records with public fields, no virtual methods, no engine
handles and no behaviour. Any tool can read and write them: C++, Python, the editor, Remix
exporters. Behaviour (cooking, rendering, playing) lives in the systems that *consume* them. The
same record describes an original-asset replacement, a FUSE library asset and a newly generated
asset.

### 2.1 On-disk layout

```
<store>/poco/<kind>/<poco_id>.poco.json   # header: small, diffable, schema-versioned
<store>/blobs/sha256/<ab>/<sha256>        # content-addressed payloads (vertex streams, PNG/EXR/KTX2, WAV/FLAC)
```

- `poco_id` is a readable, stable id (`tex/castle/wall_stone_01`, `mesh/prop/barrel_a`). For
  replacements that are not yet named, it is `r/<game>/<oaid-short>`.
- Blobs are immutable and addressed by sha256. They are the transport format, not the cooked format.
  Meshes use a documented little-endian stream layout (below), textures use lossless PNG/EXR/KTX2 and
  sound uses FLAC/WAV.
- Two stores: `Content/` (committed recipes and our small assets, per the asset plan's "nothing
  large in git" rule) and `build/remaster-cache/<game>/` (git-ignored, holds captures, originals
  and derived files).

### 2.2 Common header (all kinds)

```json
{
  "schema": "fuse.poco/1",
  "kind": "texture_set",
  "id": "tex/castle/wall_stone_01",
  "name": "Castle wall stone",
  "units": {"length": "m", "up": "+Z", "handedness": "right"},
  "payload": { "...kind specific..." : "..." },
  "provenance": {
    "origin": "generated",            // original | library | generated | derived | authored
    "derived_from": ["oaid:6f1c...", "poco:mat/rock/granite_01"],
    "recipe": "recipe:tex-upscale/6f1c...@3",
    "tool": "Tools/FUSE/Remaster/remaster@<git-sha>",
    "ai": {"provider": "local", "model": "pbrify-remix-upscale", "model_sha256": "…", "seed": 7},
    "human_authorship": "selection"
  },
  "licence_id": "tex/castle/wall_stone_01",   // → Content/licences.lock.json record
  "distribution": "recipe-only",               // ship | recipe-only | never
  "replaces": ["oaid:6f1c..."],                // empty for library/new assets
  "tags": ["stone", "masonry", "medieval"],
  "review": {"state": "approved", "by": "odin", "date": "2026-10-02"}
}
```

`distribution` is computed from `provenance` and the licence (§0.1.3–4) and cannot be set by hand.
Any `origin: original` or `derived` asset with an original in its derivation chain is `recipe-only`
or `never`. The lint (§2.6) fails if a `recipe-only` blob is referenced by a pack or by git.

### 2.3 Kind payloads (C++ view)

The C++ structs live in a new header-only library `fuse_poco` (`Source/FUSE/Remaster/include/fuse/remaster/poco.hpp`).
They are aggregates only: `std::vector`, `std::string`, fixed arrays and enums, and no pointers into
engine state. There are JSON (de)serialisers in the same library. Python gets mirror dataclasses
generated from one JSON Schema (`Content/schemas/poco/*.schema.json`), so C++ and Python cannot
drift apart (a W0 gate).

```cpp
namespace fuse::remaster::poco {

struct BlobRef { std::string sha256; u64 size = 0; std::string media; };  // "fuse/mesh-stream", "image/png"...

struct MeshStream {               // one vertex attribute
    enum class Semantic : u8 { Position, Normal, Tangent, Uv0, Uv1, Color0, Joints0, Weights0 };
    enum class Format : u8 { F32x2, F32x3, F32x4, Unorm8x4, U16x4, Unorm16x4 };
    Semantic semantic; Format format; BlobRef data;
};
struct Submesh { u32 indexOffset = 0, indexCount = 0; std::string materialSlot; };
struct Mesh {
    std::vector<MeshStream> streams; BlobRef indices32; std::vector<Submesh> submeshes;
    std::array<f32, 6> bounds{};           // min xyz, max xyz in metres
    std::string skeletonId;                // empty = static
    std::vector<std::string> lodIds;       // optional pre-made LODs (else the cook generates them)
};
struct TextureSet {                         // one material's maps, all optional
    BlobRef albedo, normal, orm, height, emissive, opacity;
    enum class NormalConvention : u8 { GL, DX } normalConvention = NormalConvention::GL;
    f32 texelsPerMetre = 0.f;               // 0 = unknown (legacy)
    bool albedoSrgb = true;
};
struct Material {
    enum class Model : u8 { Opaque, Masked, Translucent, Emissive, Subsurface, ClearCoat, Cloth };
    Model model = Model::Opaque; std::string textureSetId;
    std::array<f32, 4> baseColor{1,1,1,1}; f32 roughness = 0.5f, metallic = 0.f, emissiveNits = 0.f;
    f32 alphaCutoff = 0.5f; bool twoSided = false;
    std::string physicalCategory;           // footsteps/impacts (asset plan .fusemat)
    std::string layerRecipe;                // optional: FUSE layered/procedural material recipe id
};
struct Light {
    enum class Type : u8 { Point, Spot, Directional, Rect, Disk, Dome };
    Type type; std::array<f32,3> colorLinear{}; f32 intensity = 0.f;   // lumens (point/spot), lux (dir), nits (area)
    f32 range = 0.f, innerCone = 0.f, outerCone = 0.f; std::array<f32,2> size{};
    bool castsShadows = true;
};
struct Skeleton { std::vector<std::string> joints; std::vector<i32> parents; BlobRef inverseBind; };
struct AnimClip { std::string skeletonId; f32 sampleRate = 30.f; f32 duration = 0.f;
                  BlobRef tracks; bool rootMotion = false;
                  std::vector<std::pair<f32, std::string>> events; };
struct Sound { BlobRef pcm; u32 sampleRate = 48000; u8 channels = 1; bool loop = false;
               f32 loudnessLufs = -23.f; std::string category; };
struct EntityComponent { std::string type; std::string json; };        // opaque to the POCO layer
struct Entity { std::string archetype; std::array<f32,16> transform{};
                std::vector<std::string> assetRefs; std::vector<EntityComponent> components;
                std::string legacyClass, legacyName; };                  // e.g. "StaticShape", "door01"
struct Level { std::vector<Entity> entities; std::vector<std::string> lightIds;
               std::string lookId; std::string skyId; };
}
```

### 2.4 Mapping to FUSE cooked formats (track B and the library)

| POCO kind | FUSE cooked output | Cook path | Notes |
|---|---|---|---|
| `Mesh` | `.fusemesh` FMSH v2 (asset plan §5.1: tangents, uv1, colour, skin, LOD, meshlets) | `mesh_cook.cpp` gains a POCO reader next to its Assimp reader | Legacy meshes often have no tangents; the cook generates MikkTSpace tangents. The no-repair rule stays: broken input fails and the importer must fix it explicitly. |
| `TextureSet` | `.fusetex` (BC1/BC4/BC5/BC7/BC6H per asset plan W0.3) | `texture_cook.cpp` | `normalConvention: DX` is flipped at cook time. The asset plan's `asset_normal_map` gate applies. |
| `Material` | `.fusemat` (asset plan §5.1) | new material cook | `Model` maps to the shading models in `renderer/material/material.hpp`. |
| `Light` | `.fuselevel` light component | `world_converter` | Legacy lights have no physical units; §4.4 describes calibration. |
| `Skeleton`, `AnimClip` | `.fuseanim` | new clip cook (asset plan W7) | Torque DSQ sequences come in here (§6). |
| `Sound` | Ogg Vorbis 48 kHz / `.fusebank` | `AudioImportDesc` path | Loudness normalisation per the `asset_audio` gate. |
| `Entity`, `Level` | `.fuselevel` | `world_converter.cpp` | `legacyClass` → FUSE archetype table (§4.5). |

### 2.5 Mapping to and from RTX Remix USD (track A)

Remix mods are OpenUSD layers. Replacements target captured prims named after the asset hash
(`mesh_<HASH>`, material prims for texture hashes) and use NVIDIA's `AperturePBR_Opacity` /
`AperturePBR_Translucent` MDL materials, with textures ingested as DDS [R18][R19][R20] **[VERIFY
exact prim paths, texture suffixes and the hash rule against dxvk-remix and toolkit-remix source in
W2.1]**.

| Direction | Mapping |
|---|---|
| Remix capture → POCO | Captured mesh prim → `Mesh` (points, normals, `primvars:st`, indices; convert Remix/game units to metres with the per-game `unit_scale`) + `hash_key(remix64)`; captured texture (DDS) → decoded RGBA8 → canonical sha256 + `hash_key(remix64)`; captured lights → `Light` (reference only); camera → golden-capture viewpoint (§4.8). |
| POCO → Remix mod layer | For each `replacement(oaid, tier=remix)`: write an `over` on the captured prim (`mesh_<HASH>` → reference to our USD mesh; material → `AperturePBR_Opacity` inputs from `Material` + `TextureSet`); write DDS files (BC7 albedo/ORM, BC5 normal) from the POCO blobs; write the `mod.usda` layer stack and a manifest. |
| USD library | TinyUSDZ (Apache-2.0 with some MIT helper code; now continued as LightUSD) [T6] is small and has no dependencies. It can be vendored with a `VERSION` pin. Assimp 6.0.5 in `Engine/lib/assimp` already contains a TinyUSDZ-based USD importer, currently switched off (`ASSIMP_BUILD_USD_IMPORTER off`). OpenUSD itself is under the TOST licence (Apache 2.0 with a changed trademark section) [T7]. It is heavier and optional (open decision D4). |

The mod layer contains only our assets and `over`s keyed by hashes, never captured original
payloads. The Remix capture folder stays in `build/remaster-cache/<game>/capture/`.

### 2.6 Licences: linking to the asset plan's `licences.lock.json`

The asset plan's lock (§2.4 there) is extended rather than duplicated:

- New `origin` values: `original` (never shipped; recorded so that derivation propagates),
  `derived-original`, and `ai` (with a `provider`/`model`/`tier`/`recipe` block).
- New fields: `distribution` (ship | recipe-only | never), `human_authorship`, `game_id`,
  `cloud_uploads` (list of upload-log ids when an original went to a cloud API).
- New licence ids: `LicenseRef-Original-<game-id>` (restrictive, not shippable),
  `LicenseRef-AI-Gemini`, `LicenseRef-AI-Gemini-Image`, `LicenseRef-AI-Veo`,
  `LicenseRef-AI-Genie-Reference`, `LicenseRef-RightsHolderGrant-<id>`.
- `fuse_lint asset-licences` gains checks:
  - (8) no `LicenseRef-Original-*` or `recipe-only` blob is reachable from a pack or tracked by git;
  - (9) every `ai` record has a recipe with model id, version or date, seed and output hash;
  - (10) every record with an original in its derivation chain and a cloud upload has an opt-in
    record;
  - (11) forbidden model licences (NC or ND weights such as PBRFusion; research-only training data
    without review) fail, per §3.4.

### 2.7 Dropping in library and generated assets

A replacement is just a `replacement` row that points at *any* POCO id. So:

- **FUSE library asset** (asset plan content, for example `mat/rock/granite_01`): the editor's
  "Replace with library asset" or `fuse_remaster replace --oaid … --with mat/rock/granite_01` adds a
  row. The asset is already in the lock as CC0 or generated, so it is shippable.
- **New generated asset** (procedural recipe, Blender script or AI from our own prompt): it is
  created as a POCO with `origin: generated`, reviewed, and linked the same way. It is shippable.
- **Upgraded original** (upscale or PBR inference of the original): it is created with
  `origin: derived`. Its distribution is recipe-only, and the patch ships the recipe.
- **Scale and orientation adapters** handle the mismatch between library assets in metres/+Z and
  legacy content (for example Torque units, or Quake units ≈ 1 inch **[VERIFY per game]**). A
  `fit` block on the replacement row (translate, rotate, uniform scale, pivot policy) is applied at
  emit time. The POCO stays in canonical units.

---

## 3. The AI services layer

### 3.1 Shape

A provider-agnostic Python package, `Tools/FUSE/Remaster/fuse_ai/`. Offline tooling runs in Python,
and the engine runtime never calls AI. It has a thin C++ client for the editor that talks to a local
`fuse_ai serve` process over stdio or JSON-RPC.

```python
class Provider(Protocol):
    id: str                                  # "gemini", "local-onnx", "comfyui", "remix-toolkit", "mock"
    def capabilities(self) -> set[Task]: ... # Task: TAG_IMAGE, INFER_MATERIAL, UPSCALE, PBR_MAPS,
                                             #       EDIT_IMAGE, GEN_IMAGE, SUMMARISE_CODE, PORT_SCRIPT,
                                             #       GEN_VIDEO, SEGMENT, TRANSCRIBE, TTS_SYNTH
    def estimate(self, req: Request) -> Cost: ...
    def run(self, req: Request) -> Result: ...   # pure function of (req, model version, seed) as far as the provider allows
```

Providers, in priority order:

| Provider | Tasks | Where | Notes |
|---|---|---|---|
| `mock` | all | CI | Deterministic CPU stand-ins, like `fuse_nvplugin_mock`: bicubic ×4 for UPSCALE, Sobel-from-luma for PBR normal, a keyword table for TAG_IMAGE, canned Lua for PORT_SCRIPT. They test plumbing, not quality. |
| `local-onnx` | UPSCALE, PBR_MAPS, SEGMENT | CPU (CI-capable at small sizes) or GPU | ONNX Runtime with pinned model files, from the allow-list in §3.4. |
| `comfyui` | UPSCALE, PBR_MAPS, GEN_IMAGE (local diffusion) | local GPU, user-run | The same engine the Remix Toolkit AI tools use [R14][R15]. The workflow JSON is pinned in the recipe. |
| `remix-toolkit` | ingest/validate textures, push replacements | Windows + Toolkit | Through the REST API [R13]. Optional. |
| `gemini` | TAG_IMAGE, INFER_MATERIAL, SUMMARISE_CODE, PORT_SCRIPT, EDIT_IMAGE, GEN_IMAGE, GEN_VIDEO (Veo), TRANSCRIBE, TTS_SYNTH | cloud, paid key, opt-in | Model ids are pinned per recipe (§3.2). |
| `worldmodel-reference` | none automated | human | Genie/Marble/HY-World outputs are logged as *reference* assets by hand (§4.7). |

### 3.2 Provider facts that shape the design (September 2026)

- **Gemini API models** [M1]:
  - stable: `gemini-3.8-flash` (text, image, video and audio input; aimed at long-horizon coding and
    agents), `gemini-3.7-flash`, `gemini-3.6-flash`, `gemini-3.5-flash-lite`;
  - preview: `gemini-3.1-pro-preview` (advanced reasoning);
  - images: `gemini-3.1-flash-image` ("Nano Banana 2"), `gemini-3-pro-image` ("Nano Banana Pro",
    4K);
  - video: `veo-3.1-generate-preview`, `veo-3.1-lite-generate-preview`;
  - TTS: `gemini-3.8-flash-tts` (voice replication, restricted by §0.1.8);
  - embeddings: `gemini-embedding-2-preview` (multimodal).
  - **Imagen 4 has been shut down**, and the Gemini 2.5 models are restricted to past users [M1].

  **Consequence:** model churn is real. Every recipe pins a model id, and the cache (§3.3) is the
  source of truth for reproducible builds, not the live API.
- **Gemini pricing** (paid tier, per 1M tokens) [M2]:
  - `gemini-3.8-flash`: $0.75 in / $3.75 out until 2026-12-31, then $1.50 / $7.50;
  - `gemini-3.1-pro-preview`: $2 / $12 (≤ 200k context), $4 / $18 above;
  - `gemini-3.5-flash-lite`: $0.30 / $2.50;
  - images: Nano Banana 2 costs $0.045 (0.5K), $0.067 (1K), $0.101 (2K) or $0.151 (4K) per image,
    and Nano Banana Pro $0.134 (1K/2K) or $0.24 (4K);
  - Veo 3.1 standard: $0.40/s (720p/1080p), $0.60/s (4K); Fast $0.10–0.30/s; Lite $0.05–0.08/s;
  - Batch mode is 50 % off, and context-cache reads cost about 20 % of input.
- **Genie 3 / Project Genie**:
  - Genie 3 was announced in August 2025: real-time 720p at 24 fps with minutes of consistency
    [G2][G3].
  - Project Genie launched on 2026-01-29 for **Google AI Ultra subscribers in the US only, 18+**.
    Sessions are capped at 60 s. You can download videos of worlds; there is **no API** [G1][G3].
  - Content filters block some copyrighted characters [G3].
  - A Waymo variant exists for driving simulation [G3].
  - Consequence: Genie cannot be a pipeline provider. It is a human-operated reference tool that
    produces videos.
- **Other world models** (for reference and prototyping, all [SPECULATIVE] in usefulness):
  - **World Labs Marble** exports Gaussian splats (`.spz`/`.ply`), collider meshes and a textured
    GLB mesh. Commercial rights and GLB export come with the paid Pro plan [W1].
  - **Tencent HunyuanWorld 1.0 / HY-World 2.0** have open weights under Tencent community licences
    and export meshes and 3DGS [W2]. **[VERIFY territory and user-count clauses before any use]**.
  - **Microsoft Muse/WHAM** has open weights for gameplay ideation research [W3].
- **Remix AI tools** [R14]:
  - Since Remix 1.5 they run through ComfyUI, locally or on a remote machine, and templates download
    models from Hugging Face on first run. Tasks: PBR generation, upscaling, style transfer and mesh
    processing.
  - **PBRFusion** (the model NVIDIA promotes) is **CC-BY-NC-SA-4.0** [R16], so it is **forbidden**
    for anything we ship under the asset plan's policy. It may be used only for private user-side
    experiments, never in our recipes.
  - **PBRify_Remix** is **CC0**, trained only on CC0 ambientCG data [R17], and is allowed.

### 3.3 Recipes, caching and reproducibility

Every AI call is described by a **recipe**, a committed JSON file under `Content/remaster/<game>/recipes/`
(for patches) or `Content/recipes/ai/` (for library assets):

```json
{
  "schema": "fuse.ai-recipe/1",
  "id": "tex-upscale/6f1c9a…",
  "task": "UPSCALE",
  "provider": "local-onnx",
  "model": {"id": "pbrify-remix-upscale-4x", "sha256": "…", "licence": "CC0-1.0",
            "training_data": "ambientCG CC0"},
  "params": {"scale": 4, "tile": 256, "overlap": 16, "denoise": 0.2},
  "seed": 7,
  "inputs": [{"oaid": "6f1c…", "canonical_sha256": "…"}],
  "output": {"sha256": "…", "tolerance": {"metric": "flip", "max_mean": 0.01}},
  "cost": {"usd": 0.0, "tokens_in": 0, "tokens_out": 0, "seconds": 3.2},
  "review": {"state": "approved", "by": "…", "date": "…"}
}
```

- **Cache.** Results are stored by `sha256(canonical recipe without output/review)` in
  `build/ai-cache/`, alongside blobs. Three modes:
  - `replay` (CI default): only use cached outputs; a cache miss is a failure.
  - `record`: call the provider and store the result.
  - `mock`: CI plumbing tests.
- **Determinism.** Local CPU ONNX with a fixed thread count and fixed tiling is expected to be
  bit-stable. GPU inference and cloud calls are not, so recipes carry an output **tolerance**. A
  rebuild on the user's machine passes if FLIP/SSIM against the recorded hash's image stays within
  the tolerance. The reference image is kept only in the *author's* cache, never shipped when it is
  a derivative. For patches we ship a small perceptual fingerprint instead: a 16×16 downsample plus a
  histogram, which does not reproduce the original **[SPECULATIVE: adequacy of fingerprints]**.
- **Seeds and nondeterminism.** Gemini text calls use `temperature: 0` plus a seed where the API
  supports one **[VERIFY seed support per model]**. Because outputs can still drift, *the committed
  artefact is the reviewed output* (Lua code, tags, material parameters), and the recipe is the audit
  trail. Code produced by Gemini is committed as source; it is not regenerated at build time.
- **Budgets.** `Content/remaster/<game>/ai-budget.json` sets per-provider monthly USD caps, per-run
  caps, and request-per-minute limits. `fuse_ai` refuses to run when an estimate exceeds the
  remaining budget, and records the actual spend in the recipe.
- **Review gates.** A recipe is `draft` until a human approves it in the editor review queue
  (§5.3). Emitters (Remix layer, cook) include only `approved` recipes, except in local preview mode.
  Every shippable AI output needs a reviewer name. There is no auto-approve for anything that
  affects gameplay logic.

### 3.4 Local model allow-list (initial)

| Model / tool | Licence | Training data | Status |
|---|---|---|---|
| PBRify_Remix (upscale, normal, roughness, height) | CC0 [R17] | ambientCG CC0 [R17] | **Allowed** (default texture path) |
| Real-ESRGAN (code) | BSD-3-Clause [T1] | n/a | Allowed as code |
| Real-ESRGAN official weights (`RealESRGAN_x4plus` and others) | BSD-3 repository | Trained on DIV2K / Flickr2K / OST; DIV2K is "for academic research purpose only" [T2] | **Review required**; not the default |
| OpenModelDB community models | Per model; the site's position is that a model licence does not extend to its outputs [T4] | Per model | CC0/CC-BY models with documented data only; NC models forbidden in recipes |
| PBRFusion 3/4 | CC-BY-NC-SA-4.0 [R16] | Undocumented | **Forbidden** in recipes |
| chaiNNer | GPL-3.0 tool [T3] | n/a | Can be *run* by users; never vendored or linked (MIT tree) |
| ComfyUI + Remix nodes | Tool (the Remix nodes are an NVIDIA open-source repo [R15], licence [VERIFY]) | n/a | Run externally only |
| gsplat / nerfstudio (Gaussian splats) | Apache-2.0 [T5] | n/a | Allowed; the original INRIA 3DGS code is non-commercial [T5] and forbidden |
| Local TTS/voice models | Per model | Per model | Only non-cloned synthetic voices (§0.1.8) |

---

## 4. Pipelines (how-tos)

Every pipeline is a `fuse_remaster <verb>` CLI subcommand (new tool `Tools/FUSE/fuse_remaster.cpp`
plus the Python `fuse_ai`) and an editor panel action (§5.3). They are listed in the order a project
uses them.

### 4.1 Capture and identify

**Track A (Remix):**

1. Create the game project: `fuse_remaster init --game-id <id> --install <path> [--exe <exe>]`.
   This writes `Content/remaster/<id>/project.json` (committed; no paths to the user's machine),
   `build/remaster-cache/<id>/local.json` (install path, git-ignored), `cloud_upload: none` and the
   default budget.
2. The user installs RTX Remix per NVIDIA's docs. Requirements: Windows 10/11 and any RTX GPU at
   minimum; recommended RTX 4070 with 12 GB VRAM and 32 GB RAM [R5]. DX8 games go through d3d8to9
   into DX9 [R7]. FUSE's `remaster doctor` checks for the files and prints which Remix version it
   found. **It never downloads them.**
3. Capture several representative scenes in Remix (captures are written as USD [R6]).
4. `fuse_remaster ingest-remix --capture <dir>` parses the USD with TinyUSDZ, decodes the DDS
   textures, computes canonical sha256 hashes, and writes `original_asset` + `hash_key` rows
   (remix64 + sha256). It also writes camera poses as golden viewpoints and preview thumbnails
   (≤ 128 px, cache only).
5. `fuse_remaster report` gives counts by kind, duplicate hashes (the same texture uploaded twice),
   hash churn between captures (unstable hashes are flagged; Remix exposes hash-rule settings
   **[VERIFY]**), and UI/HUD textures (heuristics: screen-space draws, orthographic projection).

**Track B (import):**

1. `fuse_remaster import --game-id <id> --format <fmt>` runs the importers (§6) on the install. It
   mounts archives read-only through the VFS (`t3d_asset_vfs.hpp` for Torque games).
2. Each imported asset gets canonical hashes and `legacy-path` keys. Where a Remix capture also
   exists, canonical sha256 links the two views.
3. The importers write POCO records with `origin: original` into the **cache store only**.

**Identify (both tracks):**

1. **Local first:** CPU classifiers give coarse tags (normal-map detection, alpha usage, tiling
   test, lightmap detection, UI atlas detection, dominant colour, texel density from mesh UV
   usage).
2. **Optional cloud (opt-in):** Gemini multimodal tagging via `TAG_IMAGE` on a thumbnail, not the
   full texture, unless the user allows more. It returns tags, a material class (`stone`, `wood`,
   `metal/painted`…), a physical category for the asset plan's `.fusemat`, and "is this text or a
   logo". Batch mode at half price. The results are stored as tag rows, and a human spot-checks 5 %.
3. **Embeddings (optional, [SPECULATIVE])** from `gemini-embedding-2-preview` [M1] or a local CLIP
   model give "find similar textures" searches and library matches (§4.2 step 3).

### 4.2 Texture upgrade (upscale, PBR inference, review)

For each texture `oaid` (priority order: large, frequently used surfaces first, from the capture
draw counts):

1. **Classify** (§4.1): albedo, normal, lightmap, UI, decal, sky, font. UI and fonts get special
   handling (vector re-trace or a hand redo). Lightmaps are **dropped**, because FUSE and Remix
   relight the scene. Baked lighting in albedo is detected with the asset plan's AO/albedo
   correlation test.
2. **Choose a strategy** (recorded in the recipe):
   - **Replace with a library material.** This is the best result for generic surfaces (stone,
     wood, grass) and is shippable. A CPU matcher ranks library materials by colour statistics and
     tags, optionally with embeddings. A human picks one.
   - **Upscale the original** (recipe-only): PBRify_Remix 4× (CPU/ONNX in CI at ≤ 256², GPU for
     users), then de-JPEG/de-DXT clean-up in the same model [R17].
   - **Re-author with image editing** (recipe-only when the original is an input; opt-in cloud):
     Nano Banana 2 edit with a style prompt, at 1K or 2K. The output is SynthID-marked [M5]. Useful
     for signs, posters and hand-painted detail. Every result gets a human check.
   - **Generate new** from our own prompt without the original as input (shippable, SynthID-marked).
   - **Author by hand** in external tools, then import as `origin: authored`.
3. **Infer PBR maps.** Normal, roughness and height come from PBRify_Remix (CC0). Metallic comes from
   the material class (tag → 0 or 1 mask) plus a user override. AO comes from height or geometry
   bakes, not from albedo. The asset plan's calibration rules (albedo range, roughness distribution)
   are then applied, and failures go back to review.
4. **Make it seamless and tile** (if the original tiled): a wrap-aware process with the tile seam
   test from the asset plan's gates.
5. **Review:** side by side at 1:1 and at the in-game distance, a golden-capture comparison (§4.8),
   and a check for any text or logo (trademark risk). Approve or reject.
6. **Emit:** to Remix (DDS BC7 albedo/ORM, BC5 normal, `AperturePBR_Opacity` inputs) and/or FUSE
   (`.fusetex` + `.fusemat`).

### 4.3 Mesh upgrade

1. **Triage by screen coverage** (capture draw stats) and silhouette importance. Most legacy props
   need *normals + a material*, not new geometry.
2. **Strategies:**
   - **Library swap** (the asset plan's props and kits, with the `fit` adapter). Shippable.
   - **Rebuild procedurally.** Barrels, crates, columns and stairs come from the asset plan's Blender
     headless generators, matched to the legacy bounds and silhouette. Shippable.
   - **Refine the original** (recipe-only): smooth normals (Remix 1.5 added smooth normals for
     legacy geometry [R8]), subdivision plus a displacement bake from the upscaled height,
     meshoptimizer LODs, and retopology where the topology is broken. Blender headless, with the
     recipe pinning the Blender version and script sha.
   - **Author a new hero mesh** by hand. Shippable.
   - **Gaussian-splat capture** (gsplat, Apache-2.0 [T5]) is reference only. It is not a runtime
     asset in the FUSE renderer today, so any use is [SPECULATIVE] and would need a renderer feature.
3. **Skinned characters:** keep the legacy skeleton, replace the skin mesh, and retarget
   (`animation/retarget.hpp`). Original animations are recipe-only unless re-authored.
4. **Collision:** keep the original collision hulls for gameplay parity in track B, and check them
   against the new render mesh bounds (a gate: render-mesh vs collision-hull Hausdorff distance
   within a tolerance).
5. **Emit:** USD mesh + material reference for Remix; FMSH v2 + LODs/meshlets for FUSE.

### 4.4 Lighting

| Aspect | Track A: Remix path tracing | Track B: FUSE renderer |
|---|---|---|
| Hardware | RTX only, Windows [R5] | T0 (Vulkan 1.3, Lavapipe for CI) up to T3 |
| Light sources | Remix converts legacy fixed-function lights; modders add or replace lights in USD; Remix Logic changes them from game events [R9] | Legacy lights → POCO `Light` → clustered lights; DDGI (T0 SDF-traced, T2 hardware); ReSTIR/path tracing at T2–T3 (renderer plan phases 6–7) |
| Emissive surfaces | Emissive materials are real light sources in the path tracer | Emissive + DDGI bounce; at T0 an emissive surface also spawns a proxy point/rect light (importer heuristic) |
| Denoise/upscale | DLSS 4.5 RR and MFG in Remix since August 2026 [R10] | FUSE upscaler registry: FSR1/NIS/CAS kernels in-tree, DLSS via the optional NVIDIA plugin |

**Calibration step (both tracks):**

1. Legacy light colour and intensity are unitless 0–1 values. Estimate an exposure scale per level
   from a golden capture: fit the FUSE or Remix render's mean luminance per region to the original
   capture after tone mapping, and solve one global scale plus per-light-type multipliers by least
   squares, on CPU.
2. Then author a `.fuselook` per level (asset plan §5.6: looks grade, they do not fix materials).
3. Mark the sun, sky and interior/exterior zones (Remix Logic "player is outdoors" style triggers
   [R9]; FUSE look blend volumes).

Our lights are shippable. The original light list (positions and colours) is data from the game: it
is re-derived from the user's files at build time (recipe-only), and only *our edits* (added,
removed and tuned lights, as deltas keyed by legacy light id) are shipped.

### 4.5 Level and logic port with Gemini (track B)

1. **Scene conversion:**
   - `fuse_convert --mis` (existing) produces the `.fuselevel` skeleton.
   - Extend `world_converter` with an **archetype table**
     (`Content/remaster/<game>/archetypes.json`) that maps `legacyClass` (Torque `StaticShape`,
     `Trigger`, `Item`, `SpawnSphere`, `InteriorInstance`, `TSStatic`, …) to FUSE components.
   - Unmapped classes become `wiringStub` entities, which the converter already counts as
     `wiringStubCount`, and a report lists them.
2. **Script inventory:** parse TorqueScript (`.cs`; compiled `.dso` only when the source is absent,
   and only for the user's own local use) into an AST. hxTorqueScript is prior art for a parser
   [L3], but FUSE writes its own, in C++ or Python, MIT. Build a call graph, the datablock
   inheritance tree (reusing `t3d_datablock_resolve.hpp`) and the event handlers (`onCollision`,
   `onEnterTrigger`, `onAdd`, schedules).
3. **Summarise (cloud, opt-in)** with `gemini-3.1-pro-preview` or `gemini-3.8-flash`. Each module
   goes in as source plus the call-graph context, and comes out as a structured spec: state, events,
   timers, dependencies and an invariant list. Context caching covers the shared engine API
   description. The specs are committed, as our own text.
4. **Port** each spec to FUSE Lua, targeting `ScriptRuntime` behaviours (`on_start`, `on_update(dt)`,
   `on_trigger_enter`…) and the `Entity.*` / `Physics.*` bindings. Rules for generated code:
   - It must pass the sandbox (no io or os).
   - It must pass a **behavioural parity test**: a recorded event trace from the original is
     replayed against the Lua port, and the outputs are compared (score, inventory, door states).
   - Where the original script text is copyrighted, the port is **reimplementation from the spec**.
     Generated Lua is committed as our code. Original script text is never pasted into committed
     files, comments or prompts that go into committed recipes.
5. **Review:** human code review plus the parity test (below). No auto-merge.
6. **Remix Logic (track A equivalent):** for wrapped games, the same event specs can drive Remix
   Logic graphs instead of Lua **[VERIFY graph file format and whether it can be authored
   externally]**.

**Parity harness (CPU, ctest):** a deterministic headless FUSE run of the converted level with a
scripted input trace, compared against an expected event log. The first expected logs come from
the original running under the Torque-lineage code paths already in the tree, where possible. For
non-Torque games the log is authored from observed gameplay [SPECULATIVE].

### 4.6 Audio

1. **Import** WAV/OGG/MP3 (formats vary by game) into POCO `Sound` with the canonical PCM sha256.
2. **Upgrade options:**
   - Replace with CC0 or procedural sounds (asset plan §3.9). Shippable.
   - **Restore** the original (recipe-only): resample to 48 kHz, declick, denoise with open DSP, and
     normalise loudness to the `asset_audio` gate targets.
   - **Bandwidth extension or AI upsampling** of the original (recipe-only, [SPECULATIVE] quality).
   - Music: keep the original (recipe-only reference) or commission or compose new music. The Lyria
     models [M1] are an option for *our own* new music under the Gemini terms, with human selection
     (`human_authorship: selection`).
3. **Voice:** original VO stays as the user's files, recipe-only. New VO comes from consented human
   actors or clearly labelled synthetic voices. No cloning (§0.1.8). Subtitle transcription of
   original VO uses local ASR or, with opt-in, `gemini-3.5-transcribe`. Transcripts are derived text,
   so they are recipe-only.
4. **Spatialisation:** in track B, map legacy 3D sound emitters to FUSE spatial mixer emitters and
   reverb zones. In track A, audio is untouched.

### 4.7 Genie and other world models: previs and prototyping only

What is **feasible**:

- **Mood and previs.** A human operates Project Genie (US, AI Ultra, 18+, 60 s sessions) from text
  prompts or *our own* concept images to explore art direction: lighting moods, weather, alternative
  palettes. Downloaded videos go into the reference board as `LicenseRef-AI-Genie-Reference`,
  `distribution: never` [G1].
- **Playable 60-second "feel" prototypes** of a remastered area, to discuss with stakeholders
  before building anything [G1][G3].
- **Reference footage** for animators and lighting artists (camera moves, foliage motion, water).
- **Veo 3.1** (API, opt-in, our own prompts): short reference clips and animatic cutscene drafts.
  Final in-game cutscenes are rendered by FUSE (the cinematics module) or remain the original's
  (recipe-only), with Veo used for previs only. Veo output shipped as-is needs a separate decision
  (D7), SynthID and review [M6][M7].
- **Geometry-producing world models** (Marble GLB/splats [W1], HY-World meshes/3DGS [W2]) can seed
  **blockouts** for *new* areas. They are imported as POCO `Mesh` with `origin: ai`, then rebuilt or
  heavily edited by hand before shipping [SPECULATIVE].

What is **not feasible** (and not planned):

- Replacing an engine at runtime with Genie. It has no API, a 60 s session cap, 720p/24 fps output,
  and it drifts [G1][G2][G3].
- Getting exact level geometry, collision or gameplay rules out of Genie video.
- Feeding original game footage or assets into Genie, which is a consumer product (§0.1.5).
- Any automated CI step that needs Genie.

### 4.8 QA: golden captures and image metrics

1. **Golden viewpoints:** camera poses from Remix captures or from the FUSE converted scene (track
   B). About 5–20 per level. They are stored as pose JSON (ours, shippable).
2. **Reference images:**
   - **originals** (the original game rendered at those poses) stay in the local cache only;
   - **our goldens** (FUSE renders of the remastered scene) are committed for *our own content
     scenes*, or kept per user when they include recipe-only derivatives.
3. **Metrics** reuse `quality/image_metrics.hpp`: FLIP (primary), SSIM and PSNR, and the harness in
   `Source/FUSE/Renderer/tests/harness/golden.*`. The gates:
   - **Regression** (remaster vs previous remaster golden): mean FLIP ≤ 0.05, SSIM ≥ 0.97 (asset
     plan §5.4 thresholds), on Lavapipe at 960×540, fixed seed.
   - **Fidelity sanity** (remaster vs original at low resolution, luminance and structure only):
     structure SSIM on edge maps ≥ 0.6. This catches "the upgrade changed the silhouette, lost a
     door or moved a sign" [SPECULATIVE threshold, calibrated in W3].
   - **Texture recipe tolerance** (§3.3): FLIP between a rebuilt and a recorded output ≤ the
     recipe's tolerance.
   - **Gameplay parity** (§4.5): event-log equality.
4. **Track A on Windows:** a manual QA checklist (Remix renders cannot run in CI). It compares
   screenshots from the Remix runtime at golden poses with the FUSE preview of the same POCO
   replacements, to catch mapping errors (a wrong hash or scale).

---

## 5. Integration with the FUSE renderer and editor

### 5.1 Renderer tiers

- Track B content is authored for **T0 first**, per the renderer plan and the asset plan: discrete
  LODs, impostors, and SDF-traced DDGI. T1 adds meshlets and virtual geometry from the same FMSH v2.
  T2 adds RT shadows, reflections and hardware DDGI. T3 adds path tracing/ReSTIR GI and DLSS RR
  through the optional NVIDIA plugin.
- A **"Remaster" quality preset** per game sets the tier features plus a `.fuselook`. The Look
  system (`renderer/look/`: effect graph, LUT, CAS, grading) provides:
  - **"Classic"**: the original look, emulated with a LUT fitted to original captures, low texture
    filtering, no GI;
  - **"Remastered"**: the neutral grade;
  - **"Stylised"**: our art direction.

  Each look is a blend target, so players can switch at runtime. The ENB rule from the research doc
  applies: no ENB preset import and no ENB parameter names.
- **Upscalers:** the `TemporalUpscaler` registry (FSR1/NIS/CAS in-tree, DLSS through the optional
  plugin) is used unchanged. Remix has its own DLSS integration in track A [R10].
- **Legacy material fallbacks:** POCO materials without PBR maps render with the `Opaque` model,
  roughness from the tag class, and an albedo-range clamp. This keeps DDGI bounce plausible (asset
  plan §1.6).
- **[SPECULATIVE] Remix as a FUSE path-tracing backend.** The Remix runtime exposes a C API
  (`remix_c.h`, `remixapi.dll`) that an engine can push scene data into [R1]. A Windows-only optional
  plugin (same seam as `plugins/nvidia`: ABI header, loader, mock) could render FUSE scenes through
  Remix's path tracer. This is **not** planned for the first waves (open decision D5).

### 5.2 Content and cook integration

- `CookAssetKind` gains `Poco` (source) routes to the existing mesh, texture and audio cooks. The
  asset plan's new kinds (`Material`, `Anim`, `AudioBank`, `Look`) are reused.
- `ImportPipeline::plan_from_manifest` learns POCO sources and the replacement DB. The cook graph
  tracks `derived_from` edges, so a changed recipe re-cooks its dependents (`cook_dependency_graph`).
- Pack output: `remaster_<game>_ours.pak` (shippable) and `remaster_<game>_local.pak` (built on the
  user's machine from recipes, never distributed).

### 5.3 Editor: the Remaster workspace (Qt)

A new workspace in `Source/FUSE/Editor`, built from existing pieces (`asset_browser`,
`property_inspector`, `viewport_panel`, `material_editor_panel`, `command_stack`/undo):

| Panel | Function |
|---|---|
| **Game projects** | init/doctor, install path, capture list, cloud opt-in toggle (with confirmation), budget meter |
| **Original assets** | A table of `oaid`s with thumbnails (cache), hash keys, usage counts, tags, replacement status (none / draft / approved), and licence and distribution badges |
| **Replacement editor** | Pick a strategy (library / generate / upscale / edit / author), choose provider and model, see the cost estimate, run it, and compare side by side: original, candidate, and candidate in scene |
| **Review queue** | Draft recipes with FLIP/SSIM numbers, diff heat maps (from the FLIP harness), and approve/reject with a note; keyboard-driven batch review |
| **Scene compare** | A split viewport at golden poses: original capture image vs the FUSE render (track B) or the FUSE preview of Remix replacements (track A) |
| **Logic port** | Script module list, generated spec, generated Lua, parity-test status, links to the source lines (local only) |
| **Export** | Build a Remix mod layer or cook FUSE packs; a licence-lint summary that blocks export on violations; a patch package preview listing exactly which files ship |

Headless equivalents exist for every action (`fuse_remaster …`) so that agents and CI can drive them.

---

## 6. Legacy-format importer matrix

Ordered by priority, weighing feasibility, repo lineage and the size of the reusable prior art.
"In tree" means code already in this repository.

| # | Format / game family | What exists | Plan | Licence notes | Feasibility |
|---|---|---|---|---|---|
| 1 | **Torque DTS / DSQ** (TGE, TGEA, T3D shapes and sequences) | **In tree:** `Engine/source/ts/tsShape.cpp` (`TSShape::read`, versions via `smReadVersion`, older versions in `tsShapeOldRead.cpp`), `tsShapeLoader` and the `.cached.dts` path. Nothing on the FUSE side reads DTS yet. | Wrap the legacy reader in a small `fuse_legacy_ts` adapter (the Legacy/T3D shim pattern), or port only the reader, to emit POCO `Mesh`/`Skeleton`/`AnimClip` for every detail level; collision meshes → collision POCO. Fixtures: DTS files *written by* our own writer (TSShape has write support) from CC0 meshes. | Torque3D MIT (in tree) | **High**, first |
| 2 | **Torque missions and scripts** (`.mis`, `.cs`, datablocks) | **In tree:** `fuse_convert` (`.mis` → `.fuselevel`), `fuse_import` dry run, `t3d_datablock_resolve`, `t3d_asset_vfs`, `mission_load`, Lua `t3d:` route | Archetype table, entity mapping, logic port (§4.5) | MIT | **High** |
| 3 | **Torque DIF interiors** (TGE/TGEA) | **Not in tree.** T3D removed the Interior system (no `Engine/source/interior`). | Vendor or port HiGuy's `Dif` C++ reader (BSD-style licence [F1], [VERIFY exact text]); io_dif/hxDIF as cross-check [F2]. Emit POCO meshes (BSP brushes → triangles), lightmaps dropped, portals/zones → FUSE spatial hints. | BSD-style | **Medium-high** |
| 4 | **Torque 2D** (T2D modules) | **In tree:** `t2d_module_bridge`, T2D → `.fuselevel` | Sprites → POCO texture sets; 2D remaster via the `World2D` module | MIT | High (2D) |
| 5 | **Generic interchange** (FBX, glTF, OBJ, Blend) | **In tree:** Assimp 6.0.5 with these importers enabled | Assimp → POCO adapter (shared by 6–8) | BSD-3 | High |
| 6 | **Quake family: MDL/MD2/MD3/MD5, Q3 BSP** | **In tree (off):** Assimp sources contain MD2, MD3, MD5, MDL, HMP, Q3BSP loaders, but only FBX/glTF/OBJ/Blend are switched on in `Engine/lib/CMakeLists.txt` | Enable those loaders (`ASSIMP_BUILD_MD2/MD3/MD5/MDL/Q3BSP_IMPORTER`); add a BSP entity-lump parser for levels; `.pak`/`.pk3` VFS (PAK is trivial, PK3 is zip). Prior art: ioquake3 (GPL-2.0) is reference reading only. | Assimp BSD-3; engine re-implementations are GPL, so do not copy their code | **High** for models, medium for levels |
| 7 | **DirectX `.x`, 3DS, SMD, MS3D** | Assimp sources (off) | Enable as needed | BSD-3 | High |
| 8 | **Source / GoldSrc** (BSP v29/v30/v20+, MDL v10/v44+, VTF/VMT, VPK) | Assimp has HL1 MDL; nothing for Source BSP/VTF | New MIT readers from public format docs; VPK VFS; VTF decode (DXT); VMT → POCO `Material` | Write clean-room; avoid LGPL/GPL tool code (VTFLib is LGPL, **[VERIFY]**) | Medium |
| 9 | **Unreal 1/2** (`.unr`, `.utx`, `.usx`, `.ukx`, `.uax`, UPK/UMOD) | Assimp "Unreal" is only the old `.3d` vertex-mesh format | UE Viewer (umodel) is the prior art, but its "licence is not determined" [F4], so it **cannot be vendored or copied**. Options: user runs umodel themselves and FUSE imports its glTF/PSK output via Assimp (recommended), or a clean-room package reader later. | Undetermined licence, so external tool only | Medium (through the external export route) |
| 10 | **Gamebryo NIF / Morrowind ESM** | Nothing | nif.xml is the format description, but its repository licence is GPL-3 and there is an open question about the XML itself [F3]. Using it to *generate* code could impose GPL. Options: write a reader from the public spec by hand for the NIF versions needed (Morrowind 4.0.0.2), or run NifSkope (BSD [F3]) externally for export. OpenMW (GPL-3) is prior art for the ESM semantics, not a code source. | GPL risk, so the clean-room route | Medium-low |
| 11 | **Runtime capture of any DX8/9 fixed-function game** (Remix) | External Remix | §4.1: USD capture → POCO (geometry is per-draw and pre-skinned or pre-transformed in some games, so it is a *starting point*, not a clean import) | Remix MIT, external | High for track A; low as an import substitute |
| 12 | **Noesis-supported exotic formats** | External | Noesis is closed freeware [F5]. Users may export glTF/FBX themselves; FUSE imports the result. Never automated by us. | Freeware, closed | User-side only |
| 13 | Adventure/2D engines (SCUMM, AGI…), strategy (X-COM data) | Nothing | Out of scope. ScummVM and OpenXcom (GPL) are the right tools for those games; FUSE does not compete. Kept as prior-art notes for the "re-implementation plus user-supplied data" model. | GPL | n/a |

**Prior art on the remaster model** (for design, not code). These projects all use *engine
re-implementation plus user-supplied original data*, the same ship-as-patch principle as §0.1.3:

- OpenMW (Morrowind engine re-implementation), ioquake3, GZDoom, ScummVM and OpenXcom (all GPL
  family; **[VERIFY licences]**);
- the RTX Remix mod community (Portal RTX, Half-Life 2 RTX, Painkiller RTX, Morrowind RTX, and
  about 240 compatible games) [R9][R10][R21];
- the Skyrim/Morrowind PBR communities (for example the "True PBR" conventions).

The FUSE difference: an MIT engine with its own cooked formats, and POCO assets that can go out to
Remix.

---

## 7. Execution waves

Each task is about one PR in one agent session. **Gates are CPU/Lavapipe-testable** unless marked
**[WIN/RTX manual]**. ctest labels are `remaster;<area>`. Waves 0–2 are the critical path; waves
3–6 can overlap.

### Wave 0: foundations (≈ 8 tasks) [P0]

| Task | Output | Exit criteria (ctest) |
|---|---|---|
| W0.1 POCO schema + `fuse_poco` header-only C++ library + JSON (de)serialisers | `Source/FUSE/Remaster/include/fuse/remaster/poco.hpp`, `Content/schemas/poco/*.schema.json` | round-trip every kind; schema-vs-struct parity test; fuzz JSON reader (1k cases) |
| W0.2 Content-addressed blob store + canonical hashing (texture RGBA8 mip0, mesh streams, PCM) | `fuse_remaster_store` lib | identical content under different encodings (PNG vs TGA, reordered vertex attribs) → same canonical hash; different content → different hash |
| W0.3 Replacement DB (SQLite) + `remaster.lock.json` export/import | `fuse_remaster_db` | deterministic export (byte-identical twice); multi-key → one `oaid` test |
| W0.4 Licence lock extensions (§2.6) + `fuse_lint asset-licences` checks 8–11 | lint mode | a failing fixture per new check; the existing asset-licence tests still pass |
| W0.5 `fuse_ai` package skeleton: Provider protocol, recipe schema, cache (replay/record/mock), budgets, audit log | `Tools/FUSE/Remaster/fuse_ai/` | `pytest` in ctest: replay miss fails; budget refusal; mock providers deterministic |
| W0.6 Binary/original-asset gates | extend `nvidia_binary_gate.cmake` patterns (`d3d8.dll`, `d3d9.dll`, `dxvk*.dll`, `NvRemix*.exe`, `.trex/`, `remixapi.dll`, `*.rtex.dds` [VERIFY]) + new `remaster_no_originals` gate (tracked files vs the local original-hash index when present; `.gitignore` covers `build/remaster-cache/`, `build/ai-cache/`) | self-test with seeded bad and good paths; skip cleanly without git |
| W0.7 `fuse_remaster` CLI skeleton: `init`, `doctor`, `report` | `Tools/FUSE/fuse_remaster.cpp` | `init` writes a project with `cloud_upload: none`; `doctor` reports "remix: unavailable" on Linux |
| W0.8 Cloud opt-in guard | `fuse_ai` policy module | a request whose input is an `origin: original` blob, to a cloud provider, without opt-in → hard error (test) |

### Wave 1: Torque track B vertical slice (≈ 8 tasks) [P0]

| Task | Output | Exit criteria |
|---|---|---|
| W1.1 Synthetic Torque fixtures: write DTS/DSQ from CC0 meshes using the in-tree TSShape writer; tiny `.mis` + `.cs` | `Tests/fixtures/remaster/torque/` (small, generated at test time where possible) | fixtures regenerate byte-identically |
| W1.2 DTS/DSQ → POCO importer (mesh, detail levels, skeleton, sequences, collision) | `fuse_legacy_ts` adapter | round-trip: CC0 glTF → DTS (writer) → POCO → compare vertex positions ≤ 1e-5, joint count, sequence durations |
| W1.3 POCO → FMSH v2/.fusetex/.fusemat cook route (depends on asset plan W0.1/W0.3) | cook integration | cooked output passes the asset plan's `asset_*` gates |
| W1.4 Archetype table + `world_converter` extension (entities reference POCO assets) | converter | fixture `.mis` → `.fuselevel` with zero unexpected wiring stubs |
| W1.5 DIF reader (HiGuy Dif port/vendor after licence check) + synthetic DIF fixture (written by the same library) | `fuse_legacy_dif` | brush count and triangle area match; lightmaps ignored |
| W1.6 TorqueScript parser → AST + call graph + datablock tree | `fuse_remaster_tscript` | parses the Torque3D template scripts in-tree (MIT) without error |
| W1.7 Parity harness (headless run + event log compare) | ctest harness | fixture door/trigger/pickup scenario matches its expected log |
| W1.8 Golden render of the converted fixture level on Lavapipe | golden scene | deterministic re-render FLIP = 0; regression gate active |

### Wave 2: Remix interop (≈ 6 tasks) [P0/P1]

| Task | Output | Exit criteria |
|---|---|---|
| W2.1 Research spike: pin Remix hash rules, capture layout, mod layer layout, material inputs and DDS conventions from dxvk-remix/toolkit-remix source | `docs/remaster/remix-format-notes.md` + handcrafted `.usda` fixtures (ours) | every [VERIFY] in §2.5 resolved or turned into a documented open question |
| W2.2 Vendor TinyUSDZ/LightUSD with a `VERSION` pin (or enable Assimp USD) | `Engine/lib/tinyusdz` | `fuse_lint_vendored_pins_tinyusdz`; parse the fixture capture |
| W2.3 `ingest-remix`: capture USD → POCO + hash keys | CLI | fixture capture → the expected `oaid` set; remix64 ↔ sha256 links |
| W2.4 `export-remix`: POCO replacements → `mod.usda` + DDS | CLI | output validates against our schema; re-ingest of the mod layer finds every replacement; no original blobs referenced (lint) |
| W2.5 Remix REST client (optional live link) with a mock server | `fuse_ai` provider `remix-toolkit` | mock server contract tests |
| W2.6 **[WIN/RTX manual]** end-to-end on a real owned DX9 game: capture → 10 replacements → Remix loads them | checklist + screenshots (kept private) | checklist signed off |

### Wave 3: texture pipeline (≈ 6 tasks) [P1]

| Task | Output | Exit criteria |
|---|---|---|
| W3.1 CPU classifiers (normal/lightmap/UI/tiling/baked-light) | `fuse_ai` local tasks | labelled synthetic set: ≥ 90 % accuracy |
| W3.2 `local-onnx` provider + PBRify_Remix models pinned (sha256, CC0 record) | provider | CPU inference on a 64² → 256² fixture bit-stable across two runs |
| W3.3 PBR inference + calibration (asset plan §1.6 rules) | pipeline | outputs pass `asset_normal_map`, `asset_color_space`, `asset_pbr_sanity` |
| W3.4 Library matcher (colour stats + tags) | tool | on a synthetic set, the top-3 contains the known match ≥ 80 % |
| W3.5 Recipe rebuild with tolerance + fingerprint (§3.3) | tool | rebuild on a clean cache reproduces within tolerance; the fingerprint cannot reconstruct the input (PSNR vs the original < 15 dB at full resolution) |
| W3.6 Editor review queue (Qt) | panel | Qt offscreen test: approve/reject updates the DB and the recipe |

### Wave 4: Gemini integration (≈ 5 tasks) [P1]

| Task | Output | Exit criteria |
|---|---|---|
| W4.1 `gemini` provider (REST, pinned model ids, batch mode, context caching, retries, cost accounting) | provider | contract tests against recorded fixtures (replay); no network in CI |
| W4.2 TAG_IMAGE / INFER_MATERIAL prompts + JSON-schema-constrained outputs | prompts under `Content/recipes/ai/prompts/` | replayed outputs parse; schema violations are rejected |
| W4.3 SUMMARISE_CODE / PORT_SCRIPT with spec-first flow (§4.5) | pipeline | the fixture TorqueScript (MIT template code) → spec → Lua that passes the parity harness (replay mode) |
| W4.4 EDIT_IMAGE / GEN_IMAGE (Nano Banana) with SynthID noted in the lock | provider | the lock record includes `watermark: synthid`; recipes are recipe-only when the input is original |
| W4.5 Cost report + budget enforcement end to end | CLI `fuse_ai cost` | a budget breach is refused before any call |

### Wave 5: mesh, lighting, audio (≈ 7 tasks) [P2]

| Task | Output | Exit criteria |
|---|---|---|
| W5.1 Mesh refine recipes (smooth normals, subdivide + displace, LODs via meshoptimizer) | Blender headless scripts | Hausdorff vs the original ≤ tolerance; LOD gates |
| W5.2 Collision-vs-render check | gate | failing fixture detected |
| W5.3 Light calibration solver (§4.4) | tool | synthetic scene with a known scale → solver recovers it within 5 % |
| W5.4 Look presets Classic/Remastered/Stylised; LUT fit to captures | `.fuselook` + tool | fitted LUT reproduces a synthetic grade (ΔE mean < 2) |
| W5.5 Audio import + restore + loudness | pipeline | `asset_audio` gate passes on fixtures |
| W5.6 Emissive → proxy light heuristic at T0 | converter | fixture emissive surface yields a light |
| W5.7 Remix Logic graph export from event specs **[VERIFY format; may be dropped]** | exporter | schema-valid output for the fixture |

### Wave 6: editor workspace and packaging (≈ 5 tasks) [P2]

Remaster workspace panels (§5.3) built on existing editor widgets; a patch package builder
(`ours.pak` + recipes + mappings + `CREDITS.md`); a user-side "build local pack" command. Exit:
- Qt offscreen tests for each panel;
- the packager refuses to include recipe-only or original blobs (tests);
- the package manifest lists every file with its licence.

### Wave 7: FUSE D3D8/9 capture proxy (≈ 6 tasks) [P3, SPECULATIVE]

Recorder core (platform-free) + a synthetic command-stream mock on Linux + Windows COM shims.
Captures fixed-function state into POCO + fuse-capture64 hashes, with hash parity against Remix on
the same scene. Exit:
- the mock stream → the expected POCO set (CPU);
- **[WIN manual]** capture of a real game matches the Remix capture's texture set ≥ 95 % by
  canonical hash.

Runtime replacement inside the proxy is a separate decision (D6).

### Wave 8: more importers (per format, ≈ 2–4 tasks each) [P2–P3]

In matrix order (§6): Quake family (enable Assimp loaders + BSP entities + PAK/PK3 VFS), Source
(BSP/VTF/VMT/VPK), Unreal via user-run umodel export, NIF clean-room.

Each format needs:
- synthetic fixtures we generate ourselves (for example MD2/MD3 written by a tiny MIT writer);
- import round-trips;
- a golden render of a fixture level.

### Wave 9: world-model previs process (≈ 2 tasks) [P3]

`docs/remaster/previs.md` (how to use Project Genie, Veo and Marble for reference within §0.1 rules)
and a reference-board asset type (`LicenseRef-AI-*-Reference`, `distribution: never`) in the lock.
Exit: the lint blocks any reference asset from reaching a pack.

### 7.1 Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| **IP / derivative-work exposure** (upscaled originals shipped by mistake) | Legal | `distribution` computed, not set by hand; lint checks 8–11; packager refusal; the original-hash index gate |
| **Remix is Windows- and RTX-only**; CI cannot run it [R5] | Track A untestable in CI | Handcrafted USD fixtures, a mock REST server, manual WIN/RTX checklists; POCO → FUSE preview as a cross-check |
| **Remix format and hash rules change** between releases (1.3 → 1.5 in six months [R8]) | Broken mod layers | Pin the Remix version per game project; W2.1 notes; re-ingest tests; multi-key hash table |
| **Model churn / deprecations** (Imagen 4 shut down; Gemini 2.5 restricted [M1]) | Unreproducible builds | Recipes pin model ids; cache replay is the CI source of truth; committed artefacts are reviewed outputs |
| **AI hallucination in logic ports** | Gameplay bugs | Spec-first flow, parity harness, human review, no auto-merge |
| **Licence contamination** (NC weights like PBRFusion; GPL tools like chaiNNer, nif.xml, OpenMW) | Cannot ship / relicensing | Model allow-list (§3.4); GPL tools only run externally; clean-room readers |
| **Research-only training data** (Real-ESRGAN weights / DIV2K [T2]) | Legal uncertainty | Default to CC0-trained models; review gate for others |
| **Genie unavailable outside the US / no API** [G1] | Previs blocked for non-US users (the user is in Australia) | Veo (API, Australia supported [M4]), Marble or HY-World as alternatives; Genie is optional |
| **Cloud privacy** (free tier used for training [M3]) | Leak of user-owned assets | Opt-in plus a paid-tier-only guard for originals; upload audit log |
| **Cost overrun** | Budget | Budgets and estimates before calls; batch mode; Flash before Pro; local models first |
| **Legacy geometry quality** (captured geometry pre-transformed, per-draw) | Poor track B import from captures | Prefer format importers; capture only as a fallback |
| **Deterministic GPU inference** not bit-exact | Recipe mismatch | FLIP tolerance and CPU reference path |
| **Copyrightability of AI outputs** [L1] | Weak rights in our own AI assets | Record `human_authorship`; prefer human-curated or edited content for hero assets |
| **EULA / anti-circumvention** | Legal | No DRM circumvention; API interposition and file reading only; per-game review of the EULA (§0.1.9) |

### 7.2 Open decisions for the user

| # | Decision | Recommendation |
|---|---|---|
| D1 | **First target game(s).** Which owned Torque-era title (track B) and which DX8/9 fixed-function title (track A)? | One small Torque game (G1) plus one Remix-compatible game you own that is on the Remix compatibility list |
| D2 | **Remix: external vs vendored.** The MIT licence would allow vendoring source [R2]. | External (§0.1.7) |
| D3 | **Cloud default.** Keep `cloud_upload: none` by default? Which Gemini tier and billing account? | Default none; paid tier only for anything touching originals |
| D4 | **USD library:** TinyUSDZ/LightUSD (small, Apache-2.0) vs OpenUSD (TOST, heavy) | TinyUSDZ/LightUSD |
| D5 | **Remix as an optional FUSE path-tracing backend** via `remix_c.h` [SPECULATIVE] | Defer until after Wave 2 |
| D6 | **Build our own D3D8/9 capture proxy (Wave 7)** and, later, runtime replacement? | Capture-only, and only if Remix gaps appear |
| D7 | **Shipping raw AI media** (Veo clips, Nano Banana images) in releases vs reference only | Reference only for Veo; images allowed after review and edit |
| D8 | **Voice policy:** allow synthetic (non-cloned) TTS placeholder VO in releases? | Placeholder only, labelled; no cloning |
| D9 | **Model allow-list:** accept Real-ESRGAN official weights after review? | No by default; PBRify_Remix first |
| D10 | **Genie subscription** (AI Ultra, US-only) worth it given that the user is in Australia? | Not needed; use Veo/Marble for previs |
| D11 | **Distribution channel** for patches (ModDB/Nexus/GitHub releases) and the patch installer UX | Decide at Wave 6 |
| D12 | **Windows + RTX test machine** for the manual gates | Required for track A sign-off |

### 7.3 Cloud API cost estimates [SPECULATIVE: token counts assumed]

Paid-tier list prices from [M2] (September 2026). Batch mode (−50 %) is used where latency does not
matter. The figures assume a mid-size 2000s game: about 5,000 unique textures, 1,500 meshes, 150k
lines of script and 40 levels.

| Job | Model | Volume assumption | Estimate |
|---|---|---|---|
| Texture tagging + material inference (thumbnails) | `gemini-3.8-flash` batch | 5,000 × (≈ 1,500 in + 300 out tokens) | ≈ 5,000 × $0.00225 = **$11**, batch **≈ $6** (at 2027 prices ≈ $12) |
| Script summarisation → specs | `gemini-3.1-pro-preview` | 150k lines ≈ 2M tokens source; 3 passes with context ≈ 6M in, 1.5M out | 6 × $2 + 1.5 × $12 = **≈ $30**; with context caching of the shared API text, ≈ $25 |
| Spec → Lua port | `gemini-3.1-pro-preview` (hard modules), `gemini-3.8-flash` (simple ones) | 3M in, 2M out, mixed | **≈ $20–40** |
| Iteration and re-runs (review rejections) | mixed | × 2–3 on the above | **≈ $100–150 total** for logic |
| Image re-authoring (hero textures, signs) | Nano Banana 2 at 2K | 500 textures × 3 candidates | 1,500 × $0.101 = **≈ $150** (batch ≈ $76) |
| Concept art / new assets | Nano Banana Pro at 2K | 300 images | 300 × $0.134 = **≈ $40** |
| Reference / cutscene previs | Veo 3.1 Fast (≈ $0.10–0.30/s) or standard ($0.40/s) | 20 clips × 8 s × 4 takes = 640 s | **$64–192** (standard ≈ $256) |
| Transcription of original VO (opt-in) | `gemini-3.5-transcribe` | 5 h audio | Order of **$10** [VERIFY per-minute pricing] |
| Genie | Google AI Ultra subscription (consumer, US only) | per month | Subscription price per the Google store [VERIFY; reports range US$99.99–249.99/month] |
| **Total for one mid-size remaster** | | | **≈ $400–700** in cloud spend, plus a local GPU for texture work (upscaling/PBR is $0 cloud with local models) |

Budget defaults in `ai-budget.json`: $50 per run, $300 per month per game, both overridable.

---

## 8. Repository survey (2026-09-23) this plan builds on

| Area | What exists | Used for |
|---|---|---|
| Torque DTS reader/writer | `Engine/source/ts/tsShape*.cpp`, `tsShapeOldRead.cpp`, `loader/tsShapeLoader.*`, Assimp and COLLADA shape loaders under `Engine/source/ts/` | W1.1–W1.2 |
| DIF interiors | Not present (no `Engine/source/interior`) | W1.5 needs an external BSD reader |
| Legacy conversion | `Tools/FUSE/fuse_convert.cpp`, `fuse_import.cpp`; `Source/FUSE/Project/*/world_converter.*`, `import_pipeline.*`, `t3d_asset_vfs.hpp`, `t3d_datablock_resolve.hpp`, `t2d_module_bridge.hpp`, `mission_load.hpp` | Track B scenes |
| Cook | `Tools/FUSE/Cook/src/mesh_cook.cpp` (Assimp → FMSH v1), `texture_cook.cpp`, `cook_content_hash.hpp`, `cook_manifest.hpp`, `cook_dependency_graph.hpp` | POCO → cooked |
| Assimp | `Engine/lib/assimp` 6.0.5; FBX/glTF/OBJ/Blend enabled; MD2/MD3/MD5/MDL/HMP/Q3BSP/SMD/X/3DS/USD sources present but disabled | Importer matrix rows 5–7 |
| NVIDIA plugin seam | `Source/FUSE/Renderer/plugins/nvidia/` (ABI header, loader, providers, `mock/`, `cmake/nvidia_binary_gate.cmake`) | The pattern for external Remix, the mock-provider approach, and the binary gate extension |
| Look system | `Source/FUSE/Renderer/src/look/` (`look_schema`, `look_params`, `lut3d`, `look_post_chain`, CAS bridge) | Classic/Remastered/Stylised looks |
| Quality metrics | `renderer/quality/image_metrics.hpp` (FLIP, SSIM, PSNR), `tests/harness/golden.*` | QA gates |
| Scripting | `Source/FUSE/Script` (Lua 5.2 sandbox, `ScriptRuntime`, legacy `t3d:`/`t2d:` routes) | Logic port target |
| Editor | `Source/FUSE/Editor` (asset browser, inspector, material editor, viewport, undo) | Remaster workspace |
| SQLite | `Engine/lib/sqlite` | Replacement DB |

---

## 9. Sources (all accessed 2026-09-23)

**NVIDIA RTX Remix**
- [R1] dxvk-remix repository (runtime; D3D9 implementation; bridge folder; Windows build requirements; Remix API docs): https://github.com/NVIDIAGameWorks/dxvk-remix
- [R2] rtx-remix LICENSE (MIT, © 2021–2026 NVIDIA): https://github.com/NVIDIAGameWorks/rtx-remix/blob/main/LICENSE.txt
- [R3] bridge-remix (MIT; archived 2025-05-05, moved into dxvk-remix): https://github.com/NVIDIAGameWorks/bridge-remix
- [R4] toolkit-remix (Apache-2.0; powered by Omniverse): https://github.com/NVIDIAGameWorks/toolkit-remix
- [R5] Remix technical requirements (Windows 10/11; any RTX GPU minimum; RTX 4070/12 GB recommended): https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/introduction/intro-requirements.html
- [R6] Remix FAQ (DX8/9 fixed function; shaders not reconstructable; capture to OpenUSD): https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/remix-faq.html
- [R7] Remix game compatibility (d3d8to9 for DX8): https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/introduction/intro-compatibility.html ; https://github.com/NVIDIAGameWorks/rtx-remix/wiki/Compatibility
- [R8] rtx-remix releases (1.3.6 Jan 2026 Logic; 1.4.2 Apr 2026 particles; 1.5.2 Jun 2026 packaging, smooth normals): https://github.com/NVIDIAGameWorks/rtx-remix/releases
- [R9] RTX Remix Logic announcement (30+ events, ~900 triggers): https://www.nvidia.com/en-us/geforce/news/rtx-remix-logic-new-game-mods-new-plugins/
- [R10] Gamescom 2026 (DLSS 4.5 RR + MFG in Remix; Painkiller RTX; Morrowind RTX): https://www.nvidia.com/en-us/geforce/news/gamescom-2026-nvidia-geforce-rtx-dlss-4-5-announcements/
- [R11] Remix runtime open source (MIT; DLSS/NRD/RTXDI stay under their own SDK licences): https://www.nvidia.com/en-us/geforce/news/rtx-remix-runtime-open-source-download/
- [R12] Remix Toolkit open source + REST API + app connectors: https://www.nvidia.com/en-us/geforce/news/rtx-remix-rest-api-app-connectors/
- [R13] Using the REST API: https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.2.4/docs/howto/learning-restapi.html
- [R14] Remix AI tools (ComfyUI based; models downloaded from Hugging Face): https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/1.5.2-0/docs/howto/learning-aitools.html
- [R15] ComfyUI-RTX-Remix: https://github.com/NVIDIAGameWorks/ComfyUI-RTX-Remix
- [R16] PBRFusion model card (CC-BY-NC-SA-4.0): https://huggingface.co/NightRaven109/PBRFusion
- [R17] PBRify_Remix (CC0; trained on ambientCG CC0): https://github.com/Kim2091/PBRify_Remix
- [R18] Remix asset/material replacement how-tos: https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/howto/learning-assets.html ; https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/docs/howto/learning-materials.html
- [R19] Community Remix tools (AperturePBR_Opacity, `mesh_HASH` prims): https://github.com/Ekozmaster/NvidiaOmniverseRTXRemixTools
- [R20] blender-remix (USD import/export for Remix): https://github.com/sambow23/blender-remix
- [R21] GDC 2026 GeForce announcements (Remix updates): https://www.nvidia.com/en-us/geforce/news/gdc-2026-nvidia-geforce-rtx-announcements/

**Google (Genie, Gemini, Veo)**
- [G1] Project Genie announcement (2026-01-29; US AI Ultra; 18+; 60 s; video download): https://blog.google/innovation-and-ai/models-and-research/google-deepmind/project-genie/
- [G2] Genie 3 (DeepMind): https://deepmind.google/blog/genie-3-a-new-frontier-for-world-models/
- [G3] Project Genie (Wikipedia timeline, limitations): https://en.wikipedia.org/wiki/Project_Genie_(website) ; https://en.wikipedia.org/wiki/Genie_(world_model)
- [G4] 9to5Google, Project Genie rollout: https://9to5google.com/2026/01/29/google-project-genie/
- [M1] Gemini API models (ids, status, Imagen 4 shut down): https://ai.google.dev/gemini-api/docs/models
- [M2] Gemini API pricing: https://ai.google.dev/gemini-api/docs/pricing
- [M3] Gemini API Additional Terms (ownership, paid vs unpaid data use, EEA/CH/UK, 18+): https://ai.google.dev/gemini-api/terms
- [M4] Gemini API available regions (Australia listed): https://ai.google.dev/gemini-api/docs/available-regions
- [M5] Image generation (Nano Banana, SynthID): https://ai.google.dev/gemini-api/docs/image-generation
- [M6] Veo 3.1 in the Gemini API: https://ai.google.dev/gemini-api/docs/veo
- [M7] Veo responsible AI and usage guidelines: https://docs.cloud.google.com/gemini-enterprise-agent-platform/models/video/responsible-ai-and-usage-guidelines

**Other world models**
- [W1] World Labs Marble (splat/GLB export; plans): https://docs.worldlabs.ai/marble/export/gaussian-splat/index ; https://www.worldlabs.ai/blog/marble-world-model
- [W2] Tencent HunyuanWorld 1.0 (open weights, mesh export): https://github.com/Tencent-Hunyuan/HunyuanWorld-1.0
- [W3] Microsoft Muse / WHAM (open weights): https://huggingface.co/microsoft/wham ; https://www.microsoft.com/en-us/research/blog/introducing-muse-our-first-generative-ai-model-designed-for-gameplay-ideation/

**Tools and libraries**
- [T1] Real-ESRGAN licence (BSD-3-Clause): https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE
- [T2] DIV2K dataset ("academic research purpose only"): https://data.vision.ee.ethz.ch/cvl/DIV2K/
- [T3] chaiNNer (GPL-3.0, CLI): https://github.com/chaiNNer-org/chaiNNer
- [T4] OpenModelDB licence guidance: https://openmodeldb.info/docs/licenses
- [T5] INRIA 3DGS licence (non-commercial); gsplat (Apache-2.0): https://github.com/graphdeco-inria/gaussian-splatting/blob/main/LICENSE.md ; https://docs.gsplat.studio/main/
- [T6] TinyUSDZ (Apache-2.0 + MIT helpers; continued as LightUSD): https://github.com/lighttransport/tinyusdz ; https://github.com/lighttransport/LightUSD
- [T7] OpenUSD licence renamed TOST: https://forum.aousd.org/t/upcoming-openusd-license-update/1561

**Legacy formats and prior art**
- [F1] HiGuy Dif library (BSD-style): https://github.com/HiGuyMB/Dif
- [F2] io_dif / hxDIF / DifBuilder: https://github.com/RandomityGuy/io_dif ; https://github.com/RandomityGuy/hxDIF ; https://github.com/RandomityGuy/DifBuilder
- [F3] nif.xml (GPL-3 repo; licence question) and NifSkope (BSD): https://github.com/niftools/nifxml ; https://github.com/niftools/nifxml/issues/86 ; https://github.com/niftools/nifskope
- [F4] UE Viewer ("licence is not determined yet"): https://www.gildor.org/en/projects/umodel ; https://github.com/gildor2/UEViewer
- [F5] Noesis (freeware; "don't distribute content that you don't own"): https://richwhitehouse.com/noesis/nms/index.php?content=readme ; https://en.wikipedia.org/wiki/Noesis_(software)
- Prior-art engines (licences to re-verify in W8): https://openmw.org ; https://github.com/ioquake/ioq3 ; https://github.com/ZDoom/gzdoom ; https://github.com/scummvm/scummvm ; https://github.com/OpenXcom/OpenXcom

**Law and labour**
- [L1] US Copyright Office, Copyright and AI Part 2: Copyrightability (2025-01-29): https://copyright.gov/ai/Copyright-and-Artificial-Intelligence-Part-2-Copyrightability-Report.pdf
- [L2] SAG-AFTRA 2025 Interactive Media Agreement (digital replica consent): https://www.sagaftra.org/contracts-industry-resources/interactive/2025-interactive-media-video-game-agreement
- [L3] hxTorqueScript (TorqueScript parser prior art): https://github.com/RandomityGuy/hxTorqueScript
