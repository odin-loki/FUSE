# FUSE Remaster Plan: importing legacy games and upgrading them with FUSE Remix, Gemini, open-source world models, local AI and POCO assets

Status: plan, not yet started. Author date 2026-09-23, revised 2026-09-23 for the FOSS-first rule
(§0.1.0), the local RTX 3090 profile (§3.5) and the FUSE Remix port. Companion to
[`FUSE_ASSET_PLAN.md`](FUSE_ASSET_PLAN.md) (formats, licence lock, budgets, gates),
[`FUSE_REMIX_PORT_PLAN.md`](FUSE_REMIX_PORT_PLAN.md) (the port of dxvk-remix into FUSE's own stack;
track A's runtime),
[`FUSE_RENDERER_PLAN.md`](FUSE_RENDERER_PLAN.md) (tiers T0–T3, golden images),
[`FUSE_MASTER_PLAN.md`](FUSE_MASTER_PLAN.md) and the licensing research in
[`../research/upscaling-framegen-and-post-injectors.md`](../research/upscaling-framegen-and-post-injectors.md).

**Goal.** Build a *remaster stack* for FUSE. It takes an old game that the user owns and either
(A) **wraps it at runtime**, running the original executable through **FUSE Remix** (FUSE's port of
NVIDIA's open-source dxvk-remix runtime, planned in `FUSE_REMIX_PORT_PLAN.md`) with upgraded
replacements, or (B) **imports and ports it**, converting its assets, levels and logic into native
FUSE content. Both tracks use one library of **POCO assets** (plain data records with no behaviour)
and one **replacement database** keyed by content hash. Upgrades come from the FUSE asset library,
new procedural or hand-authored content, **FOSS local models on the user's RTX 3090** (texture
upscaling and PBR, world models, audio, 3D), and **Gemini as the AI agent** (script and logic
porting, tagging, material inference), with Nano Banana images and Veo video as cloud options.
Everything goes through reproducible recipes and human review.

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

0. **FOSS-first (the top rule).** The user's standing rule is "always pick a FOSS alternative";
   the reference machine is one local **NVIDIA RTX 3090** (24 GB VRAM, Ampere sm_86). Everything FUSE
   vendors, links, runs locally, or names as a *default* in a recipe must be FOSS, which here means
   an OSI-approved licence for the **code and for the model weights** (weights under a public-domain
   dedication such as CC0 also count). One explicit, user-approved exception is recorded below.
   - **Allow-list (code and weights):** Apache-2.0, MIT, BSD-2/3-Clause, ISC, zlib, MPL-2.0,
     LGPL-2.1/3.0, CC0-1.0 / Unlicense (weights and data). GPL-2.0/3.0 and AGPL-3.0 tools are allowed
     **only as separate processes** (invoked through a CLI, a local server or a file hand-off); they
     are never linked, imported into a FUSE process or vendored (ComfyUI, chaiNNer, Blender,
     piper1-gpl).
   - **Deny-list ("open weights" that are not FOSS):** Tencent Hunyuan / HY-World community licences,
     Gemma terms, the Llama community licences, the Stability AI community licence, the NVIDIA Open
     Model License, the NVIDIA Source Code License (non-commercial, for example nvdiffrast), any
     CC-BY-NC / -ND / -NC-SA, OpenRAIL / RAIL-M, the LTX-Video / LTX-2 open-weights and community
     licences, the Skywork community licence, the DINOv3 licence, the Bria RMBG licences, the
     Microsoft Research License, and anything "research only" or of undetermined licence. Such
     models may appear in this plan only in the *non-FOSS* list (§3.4.2), with the reason, and never
     as a default or inside a recipe we ship.
   - **A pipeline is FOSS only if every sub-model it loads is.** Background removers, text
     encoders, image encoders and rasterisers count (TRELLIS.2 fails this test because it loads
     DINOv3, RMBG-2.0 and nvdiffrast; §3.4.2).
   - **Training-data provenance is recorded separately** (`training_data` in the lock). FOSS
     weights trained on third-party game footage (Matrix-Game: Unreal Engine and GTA V footage;
     Oasis: Minecraft) are allowed for previs and reference, and their outputs default to
     `distribution: never` (§4.7).
   - **The exception: Gemini is the project's AI agent.** The user has chosen the Google Gemini API
     (proprietary cloud) as the default agent for script and logic porting, summarising, tagging and
     material inference, with Nano Banana images and Veo video as cloud options. They are plugins
     behind the `fuse_ai` provider interface: nothing is linked, CI never calls them (mocks and
     replay), and every Gemini task has a FOSS local fallback on the 3090 (§3.4.1). Other
     proprietary services (Project Genie, World Labs Marble, Omniverse Kit apps such as the Remix
     Toolkit) are **opt-in, optional references only, never defaults**.
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

   Local FOSS models on the user's 3090 (§3.5) are not cloud services. They may read originals
   without the opt-in, because nothing leaves the machine; their outputs still follow points 2–4.
6. **AI output licensing per provider** (recorded per output in the licence lock, §2.6):

   | Provider | Output ownership / terms | Data use | Constraints | Class in `licences.lock.json` |
   |---|---|---|---|---|
   | Gemini API (text, code), paid tier | Google claims no ownership of generated content, but "may generate the same or similar content for others" [M3] | Paid tier: not used to improve products; logs kept for abuse detection and legal compliance [M3] | 18+; for professional/business use; must not be used to build competing models [M3]; users in the EEA, Switzerland and the UK may only use paid services [M3]; Australia is a supported region [M4] | `LicenseRef-AI-Gemini` + model id + prompt hash |
   | Gemini API, **unpaid tier** | as above | **Used to improve Google products; human review possible** [M3] | Our own content only, never originals | same, flagged `tier: free` |
   | Nano Banana image models (`gemini-3.1-flash-image`, `gemini-3-pro-image`) | as Gemini API | as tier | Every image carries an invisible **SynthID** watermark [M5]; prohibited-use policy applies | `LicenseRef-AI-Gemini-Image` |
   | Veo 3.1 (`veo-3.1-generate-preview`) | as Gemini API; preview model | as tier | SynthID watermark; no realistic depictions of identifiable real people without consent [M7] | `LicenseRef-AI-Veo` (reference/cutscene only) |
   | Project Genie (Genie 3), **optional, opt-in** | Terms of the consumer product (Google AI Ultra); no API [G1] | Consumer product | US only, 18+, 60-second sessions; video download only [G1]; not available to the user in Australia | `LicenseRef-AI-Genie-Reference`, **never shipped** (reference only) |
   | Local FOSS models on the 3090 (§3.4) | Depends on model licence *and* training-data licence; weights must pass §0.1.0 | Local, nothing uploaded | See the allow-list in §3.4 | model SPDX id + `training_data` field |
   | Local FOSS world models (Matrix-Game, LingBot-World v1; §4.7) | MIT / Apache-2.0 weights; trained partly on third-party game footage [WM1][WM3] | Local | Outputs are previs and reference; human-reviewed | `LicenseRef-AI-WorldModel-Reference` + model SPDX id, `distribution: never` by default |

   **Copyrightability caveat.** The US Copyright Office's January 2025 report says that purely
   AI-generated material made from prompts alone is not copyrightable, while assistive use that
   keeps human authorship is [L1]. The lock therefore records a `human_authorship` field
   (`none | selection | substantial`). Assets marked `none` are treated as uncopyrightable,
   unprotected content: fine to ship, but not something we can enforce rights in.
7. **FUSE Remix, and the NVIDIA binary rules.** Track A runs on **FUSE Remix**, FUSE's own port of
   the dxvk-remix runtime, planned in [`FUSE_REMIX_PORT_PLAN.md`](FUSE_REMIX_PORT_PLAN.md). The
   dxvk-remix source is FOSS: its `LICENSE` carries the zlib licence of the DXVK base, and NVIDIA's
   code is MIT [R1][R2] **[VERIFY per-file headers in the port plan's first task]**. The port plan
   owns vendoring, the Windows D3D8/9 interposer and the renderer backend. This plan only consumes
   its capture and mod-layer formats. The boundaries that stay:
   - NVIDIA SDK components used by stock Remix (DLSS, NRD, RTXDI, NRC) stay under their own NVIDIA
     licences [R11]. They are optional plugins behind the existing NVIDIA plugin seam
     (`docs/nvidia-plugin.md`), never required and never vendored.
   - The Remix **Toolkit** has Apache-2.0 source [R4] but runs on **Omniverse Kit**, which is not
     FOSS. FUSE does not depend on it. The FOSS authoring path is FUSE's own Remaster workspace
     (§5.3) writing USD layers through TinyUSDZ (Apache-2.0) [T6]. The Toolkit REST link (W2.5) is an
     optional convenience for users who already run it.
   - No stock NVIDIA binary (`d3d9.dll` from dxvk-remix releases, `d3d8to9`, bridge
     `NvRemixLauncher32.exe`/`.trex/`, `remixapi.dll`, Kit apps, DLSS DLLs) is ever committed. The
     stock runtime may be installed by the user as a **parity reference** for FUSE Remix. An
     extended `nvidia_binary_gate.cmake` pattern set enforces this (§7, W0.6).
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
2. **G2:** for DX8/9 fixed-function games, a user can run the original through FUSE Remix and use
   FUSE to author, AI-upgrade, review and export a Remix-format mod layer (track A). The same layer
   also loads in stock RTX Remix. CI never needs Windows or an RTX GPU.
3. **G3:** one POCO asset model and one replacement database serve both tracks, the FUSE asset
   library and newly generated content.
4. **G4:** every AI-derived asset can be rebuilt from a recipe, or replayed from a cache in CI,
   with its cost, model version and reviewer recorded.
5. **G5:** QA uses golden captures and the renderer's existing FLIP/SSIM metrics
   (`Source/FUSE/Renderer/include/fuse/renderer/quality/image_metrics.hpp`).
6. **G6:** every local AI step runs on one RTX 3090 with FOSS code and weights (§3.5), and every
   Gemini step has a FOSS offline fallback, so a remaster can be rebuilt with no cloud account.

### 0.3 Non-goals

- Replacing a game engine at runtime with a world model. The best FOSS world model that fits a
  3090, Matrix-Game 2.0, renders 352×640 at 25 fps *on an H100* and drifts over minutes [WM2];
  Matrix-Game 3.0 reaches 720p/40 fps only on 8+1 data-centre GPUs [WM4]; Genie 3 has no API
  [G1][G2][G3]. World models are previs, reference and prototype tools only (§4.7).
- Shader-model games (most DX9.0c and later, DX10+, OpenGL). Remix, and so FUSE Remix, cannot
  reconstruct scenes from arbitrary shaders [R6]. For these, track B (data import) is the only path.
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
     -> FUSE Remix (port of dxvk-remix;         Assimp formats, BSP, NIF...)
        Windows D3D8/9 interposer)                 -> POCO assets (+ source hashes)
     -> capture (USD + DDS)                        -> scenes (.fuselevel)
        -> remix_import -> POCO assets            -> logic -> FUSE Lua (Gemini agent)
             |                                            |
             +--------------> replacement DB <------------+
                     (original-asset id <-> hash keys <-> POCO replacement)
                                   |
                     upgrade pipelines (§4): library pick, procedural,
                     local FOSS AI on the 3090, Gemini agent (cloud),
                     human authoring, review
                                   |
             +---------------------+----------------------+
             |                                            |
   emit Remix mod layer (mod.usda + DDS)        cook FUSE content (FMSH v2, .fusetex,
   + Remix Logic graphs                         .fusemat, .fuseanim, .fusebank, .fuselevel)
   -> ship as patch (our content + recipes)     -> run in FUSE renderer T0–T3
```

### 1.1 Track A: runtime wrap

- **A1, FUSE Remix (primary).** The user runs the game through FUSE Remix, the port of dxvk-remix
  (including its bridge for 32-bit games; the separate `bridge-remix` repo was archived on
  2025-05-05 and merged into dxvk-remix [R3]) into FUSE's own stack, as specified in
  [`FUSE_REMIX_PORT_PLAN.md`](FUSE_REMIX_PORT_PLAN.md). FUSE Remix keeps Remix's OpenUSD capture
  and mod-layer formats [R6], so this plan's tools read the capture (USD via TinyUSDZ/LightUSD,
  §2.5), register every captured texture and mesh hash in the replacement DB, produce upgrades as
  POCO assets, and **write a Remix-format mod layer** (USD) that FUSE Remix loads, and that stock
  RTX Remix also loads. The authoring surface is FUSE's own Remaster workspace (§5.3). The
  Omniverse-based Remix Toolkit and its REST API [R12][R13] are an optional live link for users who
  already run it, never a dependency.
- **A2, capture proxy: folded into FUSE Remix.** The earlier idea of a separate FUSE-owned
  `d3d9.dll`/`d3d8.dll` capture proxy is superseded, because the port owns the interposer. What
  this plan still needs from it, and asks of the port plan:
  - hash parity checks between FUSE Remix and stock Remix on the same scene;
  - a capture route into track B (captured geometry becomes the starting point for an import when
    no format importer exists);
  - a capture mode that works without RTX ray tracing (capture only, no path tracing);
  - recording logic split from the COM plumbing, so a synthetic D3D9 command-stream mock can test
    it on Linux and CPU only (the NVIDIA mock pattern).
- **Logic in track A** stays in the original executable. Upgrades react to game events through Remix
  Logic (FUSE Remix keeps it, per the port plan): more than 30 event types and about 900 triggers, without source code, since Remix 1.3 in
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
| USD library | TinyUSDZ (Apache-2.0 with some MIT helper code; now continued as LightUSD) [T6] is small and has no dependencies. It can be vendored with a `VERSION` pin. Assimp 6.0.5 in `Engine/lib/assimp` already contains a TinyUSDZ-based USD importer, currently switched off (`ASSIMP_BUILD_USD_IMPORTER off`). OpenUSD itself is under the TOST licence (Apache 2.0 with a changed trademark section) [T7]. It is heavier and optional (adopted decision A4, §7.2). |

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
  `LicenseRef-AI-Genie-Reference`, `LicenseRef-AI-WorldModel-Reference`,
  `LicenseRef-RightsHolderGrant-<id>`.
- New model fields on every `ai` record: `code_licence`, `weights_licence`, `submodels` (each with
  its own licence) and `foss: true|false`, computed from the §0.1.0 lists.
- `fuse_lint asset-licences` gains checks:
  - (8) no `LicenseRef-Original-*` or `recipe-only` blob is reachable from a pack or tracked by git;
  - (9) every `ai` record has a recipe with model id, version or date, seed and output hash;
  - (10) every record with an original in its derivation chain and a cloud upload has an opt-in
    record;
  - (11) forbidden model licences fail, per §0.1.0 and §3.4: any deny-listed licence on the model
    *or on any sub-model* (NC or ND weights such as PBRFusion; community or research licences such
    as Hunyuan, LTX, DINOv3, RMBG), and research-only training data without review;
  - (12) a recipe whose provider is not `gemini`, `mock` or a §3.4.1 FOSS model is flagged as
    non-default, and a shippable output from it fails.

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
    id: str                                  # "gemini", "local-onnx", "local-torch", "local-llm", "worldmodel", "comfyui", "mock"...
    def capabilities(self) -> set[Task]: ... # Task: TAG_IMAGE, INFER_MATERIAL, UPSCALE, PBR_MAPS,
                                             #       EDIT_IMAGE, GEN_IMAGE, SUMMARISE_CODE, PORT_SCRIPT,
                                             #       GEN_VIDEO, SEGMENT, TRANSCRIBE, TTS_SYNTH,
                                             #       WORLD_ROLLOUT, IMAGE_TO_3D, SEPARATE_STEMS, AUDIO_SR
    def vram_mb(self, req: Request) -> int: ...  # peak VRAM estimate, used by the 3090 scheduler (§3.5)
    def estimate(self, req: Request) -> Cost: ...
    def run(self, req: Request) -> Result: ...   # pure function of (req, model version, seed) as far as the provider allows
```

Providers. The **default** column says which provider a task uses when the recipe does not name
one. All local providers run on the user's RTX 3090 through local processes or local servers on
`127.0.0.1`, scheduled by the 3090 profile (§3.5). Cloud providers are plugins. CI uses `mock` and
CPU-only replay, never a GPU and never the network.

| Provider | Tasks | Where | Default for | Notes |
|---|---|---|---|---|
| `mock` | all | CI (CPU) | CI | Deterministic CPU stand-ins, like `fuse_nvplugin_mock`: bicubic ×4 for UPSCALE, Sobel-from-luma for PBR normal, a keyword table for TAG_IMAGE, canned Lua for PORT_SCRIPT, a fixed frame sequence for WORLD_ROLLOUT. They test plumbing, not quality. |
| `local-onnx` | UPSCALE, PBR_MAPS, SEGMENT | CPU (CI-capable at small sizes) or the 3090 (CUDA EP) | UPSCALE, PBR_MAPS | ONNX Runtime (MIT) with pinned model files from §3.4.1: PBRify_Remix (CC0) now, the FUSE-SR model later (§4.2). Models are loaded through spandrel (MIT) and exported to ONNX once. |
| `local-torch` | IMAGE_TO_3D, GEN_IMAGE, EDIT_IMAGE, GEN_VIDEO, AUDIO_SR, SEPARATE_STEMS, TTS_SYNTH, TRANSCRIBE | the 3090, one `fuse_ai worker` process per model | IMAGE_TO_3D, AUDIO_SR, SEPARATE_STEMS, TTS_SYNTH, TRANSCRIBE | PyTorch (BSD-3) workers for the FOSS models in §3.4.1 (TripoSR, Demucs, AudioSR, Kokoro, Whisper; FLUX.1-schnell, Qwen-Image-Edit and Wan2.2-TI2V-5B as local image/video options). |
| `worldmodel` | WORLD_ROLLOUT | the 3090 | WORLD_ROLLOUT | Matrix-Game 2.0 (MIT) by default; Matrix-Game 3.0 5B and LingBot-World v1 NF4 as quality options (§4.7). Outputs are reference assets. |
| `gemini` | TAG_IMAGE, INFER_MATERIAL, SUMMARISE_CODE, PORT_SCRIPT, EDIT_IMAGE, GEN_IMAGE, GEN_VIDEO (Veo), TRANSCRIBE, TTS_SYNTH | cloud, paid key | **The agent:** TAG_IMAGE, INFER_MATERIAL, SUMMARISE_CODE, PORT_SCRIPT; EDIT_IMAGE/GEN_IMAGE (Nano Banana); GEN_VIDEO (Veo) | The user-approved exception to §0.1.0. Our own content goes to Gemini freely; originals only with the per-game opt-in (§0.1.5). Model ids pinned per recipe (§3.2). |
| `local-llm` | TAG_IMAGE, INFER_MATERIAL, SUMMARISE_CODE, PORT_SCRIPT | the 3090, llama.cpp `llama-server` (MIT) on an OpenAI-compatible local port | none (offline fallback) | Used when the user declines the cloud opt-in, has no key, or works offline. Qwen3-Coder-30B-A3B and Qwen3-VL-8B (Apache-2.0) GGUF builds (§3.4.1). vLLM (Apache-2.0) and Ollama (MIT) are accepted alternative servers. |
| `comfyui` | UPSCALE, PBR_MAPS, GEN_IMAGE | the 3090, user-run | none | ComfyUI is GPL-3.0 [T8], so it is only ever driven over its HTTP API as a separate process. The same engine the Remix AI tools use [R14][R15]. The workflow JSON is pinned in the recipe, and every model it loads must pass §0.1.0. |
| `remix-toolkit` | ingest/validate textures, push replacements | Windows + Omniverse Toolkit | none | Optional live link through the REST API [R13] for users who already run the Toolkit. Not FOSS at the Kit layer (§0.1.7). |
| `reference` | none automated | human | none | Project Genie, World Labs Marble and other proprietary outputs are logged by hand as *reference* assets (§4.7). Opt-in only. |

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
  - Consequence: Genie cannot be a pipeline provider, and it is not available in Australia. It is
    an optional, human-operated reference tool that produces videos.
- **FOSS world models** (checked 2026-09-23 on the Hugging Face model cards and GitHub `LICENSE`
  files; details and 3090 fit in §3.4.1 and §4.7):
  - **Matrix-Game 2.0** (Skywork): MIT code and weights, 1.8B parameters, 352×640 at 25 fps on one
    H100, keyboard and mouse actions, minute-long rollouts [WM1][WM2]. The default.
  - **Matrix-Game 3.0** (Skywork, March 2026): Apache-2.0 weights, a 5B base and a 5B distilled
    model; 720p at up to 40 fps needs 8 GPUs for the DiT plus 1 for the VAE [WM3][WM4]. The
    announced 2×14B MoE model is not published yet. Matrix-Game 3.5 (arXiv, August 2026) has no
    weights yet [WM5] **[VERIFY]**.
  - **LingBot-World v1** (Ant Group / Robbyant, January–April 2026): Apache-2.0 code and weights;
    Wan2.2-A14B-based MoE with two ~14B experts (28B total, one expert active per step), 480p/720p,
    camera-pose control (Base-Cam), action control (Base-Act) and a KV-cached Fast variant (16 fps
    under 1 s latency on "one GPU node") [WM6][WM7]. A community NF4 quant (Apache-2.0, ~31 GB on
    disk) exists [WM8].
  - **LingBot-World v2 / "Infinity"** (July–September 2026, 14B and 1.3B causal-fast) moved to
    **CC-BY-NC-SA-4.0**, so it is **not FOSS** [WM9]. The licence change inside one family is why
    §7.1 tracks licence churn.
  - **MineWorld** (Microsoft): MIT code, 300M–1.2B, Minecraft only, 4–7 fps on A100/H100; the
    checkpoints were "temporarily taken down" in May 2025 and the Hugging Face repo is not public
    [WM10]. **DIAMOND**: MIT code; its published Atari and CS:GO checkpoints carry no weights licence
    and were trained on third-party game footage [WM11]. **Open-Oasis**: MIT code and weights
    (500M, gated download), Minecraft only [WM12]. These three matter as *trainable architectures*,
    not as ready models (§4.7).
- **Non-FOSS world models** (reference only, never defaults; §3.4.2): Hunyuan-GameCraft 1/2,
  HunyuanWorld 1.0 / HY-World 2.0, HunyuanWorld-Voyager (Tencent community licences) [W2][WM13];
  Microsoft Muse/WHAM (Microsoft Research License, "academic research purposes only") [W3]; NVIDIA
  Cosmos (NVIDIA Open Model License) [WM14]; World Labs Marble (proprietary cloud; splat and GLB
  export, commercial rights on the paid plan) [W1].
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
  "cost": {"usd": 0.0, "tokens_in": 0, "tokens_out": 0, "seconds": 3.2,
           "gpu": "rtx3090", "peak_vram_mb": 2100, "kwh": 0.0004},
  "review": {"state": "approved", "by": "…", "date": "…"}
}
```

- **Cache.** Results are stored by `sha256(canonical recipe without output/review)` in
  `build/ai-cache/`, alongside blobs. Three modes:
  - `replay` (CI default): only use cached outputs; a cache miss is a failure.
  - `record`: call the provider and store the result.
  - `mock`: CI plumbing tests.
- **Determinism.** Local CPU ONNX with a fixed thread count and fixed tiling is expected to be
  bit-stable. GPU inference on the 3090 (even with `torch.use_deterministic_algorithms`) and cloud
  calls are not, so recipes carry an output **tolerance**. A
  rebuild on the user's machine passes if FLIP/SSIM against the recorded hash's image stays within
  the tolerance. The reference image is kept only in the *author's* cache, never shipped when it is
  a derivative. For patches we ship a small perceptual fingerprint instead: a 16×16 downsample plus a
  histogram, which does not reproduce the original **[SPECULATIVE: adequacy of fingerprints]**.
- **Seeds and nondeterminism.** Gemini text calls use `temperature: 0` plus a seed where the API
  supports one **[VERIFY seed support per model]**. Because outputs can still drift, *the committed
  artefact is the reviewed output* (Lua code, tags, material parameters), and the recipe is the audit
  trail. Code produced by Gemini is committed as source; it is not regenerated at build time.
- **Budgets.** `Content/remaster/<game>/ai-budget.json` sets per-provider monthly USD caps (Gemini),
  per-run caps, request-per-minute limits, and for local providers a GPU-hours cap per run.
  `fuse_ai` refuses to run when an estimate exceeds the remaining budget, and records the actual
  spend (USD, or GPU seconds and kWh) in the recipe.
- **Review gates.** A recipe is `draft` until a human approves it in the editor review queue
  (§5.3). Emitters (Remix layer, cook) include only `approved` recipes, except in local preview mode.
  Every shippable AI output needs a reviewer name. There is no auto-approve for anything that
  affects gameplay logic.

### 3.4 Local model allow-list (FOSS, checked 2026-09-23)

Every row was checked against the GitHub `LICENSE` file and the Hugging Face model card on
2026-09-23 (§9). "3090 fit" is for one RTX 3090 (24 GB, Ampere sm_86). Ampere has no FP8 tensor
cores and cannot run FlashAttention 3, so fp8 checkpoints run as weight-only storage (or through
GGUF/int8 kernels) and Hopper-tuned speed claims do not carry over. Speeds marked [SPECULATIVE] are
estimates to be measured in W9.1 and W3.8.

#### 3.4.1 Allowed (defaults in bold)

| Area | Model / tool | Code licence | Weights licence | Size | VRAM and 3090 fit | Speed on a 3090 | Status |
|---|---|---|---|---|---|---|---|
| Texture | **PBRify_Remix** (upscale, normal, roughness, height) [R17] | chaiNNer chain / ComfyUI nodes (run externally) | CC0-1.0; trained only on ambientCG CC0 | small ESRGAN-class models [VERIFY arch] | < 2 GB; native (fp16), also CPU ONNX | well under 1 s per 1K texture [SPECULATIVE] | **Default** texture path |
| Texture | **FUSE-SR** (our own upscaler, W3.7) | ours (MIT) + traiNNer-redux (Apache-2.0) [T9] | ours, CC0; trained on ambientCG + Poly Haven CC0 textures | compact (SPAN/Compact class) or larger (RealPLKSR/ESRGAN class) [VERIFY arch licences] | native | training ≈ 1–2 days (compact) or 4–7 days (large) [SPECULATIVE] | Planned replacement for general upscaling |
| Texture | spandrel (model loader) [T10]; ONNX Runtime; BasicSR [T11] | MIT; MIT; Apache-2.0 | n/a | | | | Allowed (linked in Python tools) |
| Texture | Real-ESRGAN code [T1] | BSD-3-Clause | official weights trained on DIV2K ("academic research purpose only") [T2] | 16.7M | native | fast | Code allowed; official weights **not** used (A9) |
| Texture | chaiNNer [T3] | GPL-3.0 | n/a | | | | External process only |
| World model | **Matrix-Game 2.0** [WM1][WM2] | MIT | MIT (fine-tuned from SkyReels-V2-I2V-1.3B, whose card says "skywork-license" [WM15]; **[VERIFY lineage]**); uses the Wan2.1 VAE and an OpenCLIP XLM-R ViT-H/14 image encoder (MIT [VERIFY]) | 1.8B; ≈ 11.8 GB download for one variant (6.5 GB DiT + 4.8 GB CLIP + 0.5 GB VAE) | "at least 24 GB" (A100/H100 tested); bf16 DiT ≈ 4 GB, so it fits natively | 25 fps at 352×640 on one H100; ≈ 6–12 fps on a 3090 [SPECULATIVE] | **Default world model** |
| World model | Matrix-Game 3.0 (5B base / 5B distilled) [WM3][WM4] | Apache-2.0 (repo MIT) | Apache-2.0; uses a UMT5-XXL text encoder (11.4 GB) | 5B; 56.6 GB repo (distilled path ≈ 41 GB) | int8 DiT ≈ 6 GB + LightVAE; text encoder on CPU or prompts pre-encoded; fits with offload [VERIFY] | 720p/40 fps needs 8+1 GPUs; on a 3090 offline, ≈ 1–5 fps at 720p or faster at 480p [SPECULATIVE] | Quality option (offline clips) |
| World model | LingBot-World v1 Base-Cam / Base-Act / Fast [WM6][WM7] | Apache-2.0 | Apache-2.0 | 2 × ~14B MoE (28B total, 14B active); 160 GB bf16 repo | too big natively | Fast: 16 fps at 480p on "one GPU node" | Quality option via the NF4 quant |
| World model | LingBot-World Base-Cam **NF4** (community) [WM8] | Apache-2.0 | Apache-2.0 (quantised from v1) | two 9.6 GB NF4 experts + 10.6 GB T5; ≈ 31 GB | card says ≈ 32 GB; on a 3090 only with T5 on CPU and one expert resident at a time (sequential expert offload), 480p [VERIFY] | minutes per 5 s clip [SPECULATIVE] | Quality option (offline) |
| World model | Open-Oasis 500M [WM12] | MIT | MIT (gated download) | 500M | native | real time on a 3090 likely [SPECULATIVE] | Allowed; Minecraft only, so a training baseline |
| World model | DIAMOND [WM11] | MIT | none published under a licence; ours after training | ≈ 0.4B for CS:GO [VERIFY] | native | the CS:GO config took 12 days on an RTX 4090; ≈ 2–3 weeks on a 3090 [SPECULATIVE] | Allowed as code, to train on FUSE captures (§4.7) |
| World model | MineWorld [WM10] | MIT | MIT, but checkpoints taken down (May 2025) | 300M–1.2B | native | 4–7 fps (A100/H100) | Code only; Minecraft only |
| 3D | **TripoSR** [3D1] | MIT | MIT; trained on Objaverse renders (per-object licences vary: training-data review) | < 1B [VERIFY] | ≈ 6 GB [VERIFY]; native | about a second per object | **Default** image-to-3D (blockouts from *our* concept images) |
| 3D | TripoSG [3D2] | MIT | MIT | 1.44B | ≥ 8 GB; native | seconds | Allowed only with BiRefNet (MIT) replacing its default RMBG-1.4 background remover |
| 3D | TRELLIS v1 image-large [3D3] | MIT | MIT; DINOv2 (Apache-2.0) conditioning | ≈ 1.2B [VERIFY] | ≥ 16 GB; native | tens of seconds [SPECULATIVE] | Allowed only without its non-FOSS extras: its setup pulls nvdiffrast (NVIDIA Source Code License, non-commercial) and a mip-splatting Gaussian rasteriser (INRIA-derived) [VERIFY]; bake textures in Blender and render splats with gsplat instead |
| 3D | Instant Meshes [3D4]; QuadriFlow [3D5]; meshoptimizer | BSD-3-Clause; BSD-style [VERIFY exact text]; MIT | n/a | | CPU | seconds to minutes | Allowed (retopology; external CLIs) |
| 3D | gsplat / nerfstudio [T5] | Apache-2.0 | n/a | | native | | Allowed; the INRIA 3DGS code is non-commercial and forbidden [T5] |
| Image (local option) | FLUX.1-schnell [I1] | Apache-2.0 | Apache-2.0 (T5-XXL Apache-2.0, CLIP-L MIT) | 12B | fp8 weights ≈ 12 GB, native; bf16 needs T5 offload | ≈ 3–8 s per 1024² at 4 steps [SPECULATIVE] | Optional local GEN_IMAGE (Nano Banana stays the cloud default) |
| Image (local option) | Qwen-Image-2512 / Qwen-Image-Edit-2511 [I2] | Apache-2.0 | Apache-2.0 (Qwen2.5-VL-7B text encoder, Apache-2.0) | 20.4B DiT | bf16 ≈ 41 GB does not fit; fp8 ≈ 20 GB tight; GGUF Q4/Q5 ≈ 12–15 GB fits | ≈ 1–2 min per 1024² edit [SPECULATIVE] | Optional local EDIT_IMAGE (texture re-authoring without the cloud) |
| Video (local option) | Wan2.2-TI2V-5B [V1] | Apache-2.0 | Apache-2.0 | 5B | card: 720p/24 fps runs on a 24 GB GPU with `--offload_model --convert_model_dtype --t5_cpu` | ≈ 10–20 min per 5 s 720p clip [SPECULATIVE; the card only names a 4090] | Optional local GEN_VIDEO (Veo stays the cloud default) |
| Video (local option) | Wan2.1-T2V-1.3B [V2] | Apache-2.0 | Apache-2.0 | 1.3B | 8.19 GB | 5 s 480p ≈ 4 min on a 4090; ≈ 6 min on a 3090 [SPECULATIVE] | Optional quick previs clips |
| Audio | **Demucs** v4 [AU1] | MIT | MIT; trained on MUSDB18-HQ (research dataset: training-data review) | ≈ 80M [VERIFY] | < 4 GB; native, also CPU | faster than real time | **Default** SEPARATE_STEMS (recipe-only outputs) |
| Audio | **AudioSR** [AU2] | MIT [VERIFY: the LICENSE file has an unrelated copyright line] | Apache-2.0 | ≈ 1B [VERIFY] | fits [VERIFY] | slower than real time [SPECULATIVE] | **Default** AUDIO_SR (recipe-only outputs) |
| Audio | **Kokoro-82M** [AU3] | Apache-2.0 | Apache-2.0 | 82M | < 1 GB | far faster than real time | **Default** TTS_SYNTH (synthetic voices only, §0.1.8) |
| Audio | Piper [AU4] | MIT (rhasspy/piper, archived); the successor piper1-gpl is GPL-3.0 (external only) | per voice; the voices repo is tagged MIT but each voice's dataset licence varies (per-voice review) | ≈ 15–60M | CPU | real time on CPU | Allowed per voice |
| Audio | **Whisper large-v3-turbo** via whisper.cpp or faster-whisper [AU5] | MIT | MIT | 809M | ≈ 6 GB [VERIFY]; native | many times real time | **Default** TRANSCRIBE (local, so originals never leave the machine) |
| LLM fallback | Qwen3-Coder-30B-A3B-Instruct [Q4] | Apache-2.0 | Apache-2.0 | 30.5B total, 3.3B active (MoE) | GGUF Q4_K_M 18.6 GB fits with ≈ 16–32k context | tens of tokens/s [SPECULATIVE] | Offline fallback for SUMMARISE_CODE / PORT_SCRIPT |
| LLM fallback | Qwen3-VL-8B-Instruct [Q5] | Apache-2.0 | Apache-2.0 | 8.8B | Q8 ≈ 9 GB; native | fast | Offline fallback for TAG_IMAGE / INFER_MATERIAL |
| LLM fallback | Devstral-Small-2-24B; gpt-oss-20b; Qwen3-VL-30B-A3B; OLMo 2 / Molmo [Q6] | Apache-2.0 | Apache-2.0 | 24B; 21B; 31B; 7–32B | Q4 ≈ 13–19 GB; fit | | Accepted alternatives (OLMo/Molmo also publish their training data) |
| Embedding | SigLIP / SigLIP 2 so400m [Q7] | Apache-2.0 | Apache-2.0 | 0.9–1.1B | < 4 GB | fast | "Find similar" and library matching |
| Serving | llama.cpp [Q1]; vLLM [Q2]; Ollama [Q3] | MIT; Apache-2.0; MIT | n/a | | | | Allowed local servers |
| USD / Remix | dxvk-remix [R1]; TinyUSDZ/LightUSD [T6]; OpenUSD [T7] | zlib + MIT; Apache-2.0; TOST (Apache-2.0 with a changed trademark section; **[VERIFY OSI status]**) | n/a | | | | Allowed; OpenUSD only as an optional external validator |
| Tools | ComfyUI [T8]; Blender | GPL-3.0; GPL-2.0+ | n/a | | | | External processes only |

#### 3.4.2 Checked and not FOSS (never defaults, never in shipped recipes)

| Model / tool | Licence found (2026-09-23) | Why it matters here |
|---|---|---|
| PBRFusion 3/4 [R16] | CC-BY-NC-SA-4.0 | The model NVIDIA's Remix AI tools promote; forbidden in recipes |
| LingBot-World v2 "Infinity" (14B, 1.3B causal-fast) [WM9] | CC-BY-NC-SA-4.0 | v1 stays Apache-2.0; do not upgrade across the licence change |
| Hunyuan-GameCraft 1/2, HunyuanWorld 1.0, HY-World 2.0, HunyuanWorld-Voyager/Mirror, Hunyuan3D 2/2.1 [W2][WM13][3D6] | Tencent community licences (territory exclusions, user-count clauses) | Non-FOSS world and 3D models |
| Microsoft Muse / WHAM [W3] | Microsoft Research License, "academic research purposes only" | Non-FOSS; trained on one Xbox game |
| NVIDIA Cosmos-Predict 2.5 [WM14] | NVIDIA Open Model License | Non-FOSS world model |
| TRELLIS.2-4B [3D7] | MIT weights, but its pipeline loads DINOv3 (DINOv3 licence), RMBG-2.0 (Bria licence) and nvdiffrast (NVIDIA Source Code License, non-commercial) | Fails the sub-model rule (§0.1.0) as shipped [VERIFY a clean swap later] |
| Stable Fast 3D, Stable Diffusion family [3D8] | Stability AI community licence | Non-FOSS |
| SAM 3D Objects [3D9] | SAM licence ("other") [VERIFY text] | Non-FOSS pending review |
| FLUX.1-dev / Kontext-dev [I1] | FLUX.1 [dev] non-commercial licence | Only schnell is Apache-2.0 |
| HiDream-I1 / E1 [I3] | MIT transformer, but loads Llama-3.1-8B-Instruct as a text encoder (Llama 3.1 community licence) | Fails the sub-model rule |
| LTX-Video 0.9.x, LTX-2 [V3] | LTX-Video open-weights licences; LTX-2 community licence (revenue cap) | Non-FOSS video |
| Real-ESRGAN official weights [T2] | Repo BSD-3; training data DIV2K is academic-only | Review-required, not used (A9) |
| RMBG-1.4 / 2.0 [3D10] | Bria licences | Use BiRefNet (MIT) instead |
| Gemma, Llama-family LLMs | Gemma terms; Llama community licences | Not used; Qwen/Mistral/OLMo cover the fallback |
| Remix Toolkit runtime layer [R4] | Apache-2.0 source on Omniverse Kit (NVIDIA proprietary) | Optional link only; FUSE's own tooling is the FOSS path |
| Project Genie, World Labs Marble, Gemini, Nano Banana, Veo | Proprietary cloud | Gemini family: the user-approved agent exception; Genie and Marble: opt-in reference only |

### 3.5 The 3090 profile

The local AI tier is designed for **one RTX 3090 (24 GB VRAM, Ampere sm_86)** in a desktop with at
least 64 GB RAM (the Matrix-Game repos ask for 64 GB) and an NVMe model store.

- **One large model resident at a time.** `fuse_ai` runs a small scheduler: every provider reports
  `vram_mb(req)`, and the scheduler loads one "large" model (> 6 GB) at a time, unloads it before the
  next stage, and co-schedules only small models (PBRify, Kokoro, Demucs, SigLIP) beside it. The
  VRAM budget is 22 GB for models and activations, leaving about 2 GB for the desktop and CUDA
  context. A request that estimates above 22 GB is refused with the quantisation or offload option
  to use instead.
- **Sequential pipeline stages.** Batches run stage by stage over the whole game (all upscales,
  then all PBR maps, then all 3D blockouts, then world-model rollouts), not asset by asset, so each
  model loads once per batch. Rollouts and video run overnight in the background queue.
- **Quantisation choices:**
  - small models (PBRify, FUSE-SR, Demucs, Kokoro, Whisper, TripoSR): fp16/bf16, no quantisation;
  - Matrix-Game 2.0: bf16 (fits natively);
  - Matrix-Game 3.0 5B: int8 DiT, text encoder on CPU or pre-encoded prompts, LightVAE;
  - LingBot-World v1: NF4, T5 on CPU, one expert resident at a time, 480p;
  - FLUX.1-schnell: fp8 weight storage; Qwen-Image(-Edit): GGUF Q4_K/Q5_K;
  - LLM fallback: GGUF Q4_K_M (llama.cpp) or AWQ int4 (vLLM).
  - Ampere has no FP8 tensor cores and no FlashAttention 3, so use FlashAttention 2 or PyTorch SDPA,
    and treat fp8 as a storage format only.
- **Approximate throughput** [SPECULATIVE until W3.8 and W9.1 measure it]:

  | Stage | Throughput on one 3090 |
  |---|---|
  | PBRify upscale + normal + roughness, 512² → 2K | ≈ 1–3 s per texture; 5,000 textures ≈ 2–4 GPU-hours |
  | TripoSR blockout | ≈ 1–2 s per object |
  | Demucs stems / AudioSR | ≈ 5 h of audio ≈ 0.5–3 GPU-hours |
  | Whisper large-v3-turbo transcription | ≈ 5 h of audio in < 30 min |
  | Matrix-Game 2.0 rollout, 352×640 | ≈ 6–12 fps; a 60 s clip in ≈ 2–4 min |
  | LingBot-World v1 NF4, 480p | minutes per 5 s clip |
  | Wan2.2-TI2V-5B, 720p | ≈ 10–20 min per 5 s clip |
  | Qwen3-Coder-30B-A3B Q4 (fallback porting) | tens of tokens/s; 150k lines ≈ days, not hours |

- **Local servers.** Workers listen only on `127.0.0.1`: llama.cpp `llama-server` (OpenAI-compatible)
  for the LLM fallback, ComfyUI's HTTP API when a user runs it, and `fuse_ai worker` processes for
  PyTorch models. No local endpoint is exposed to the network.
- **CI stays CPU-only.** CI runs `mock` and `replay` only (§3.3), plus the small CPU ONNX PBRify
  test at ≤ 256². GPU results are recorded on the 3090 into `build/ai-cache/` and replayed in CI
  within tolerance.
- **Model store.** Weights live outside git in `FUSE_MODEL_DIR` (default
  `~/.cache/fuse/models/`), pinned by `Content/models/models.lock.json` (Hugging Face repo, revision
  hash, file sha256s, code and weights licences, sub-models). Downloads happen only on demand, per
  stage, after a size prompt (adopted decision A13).

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
2. The user runs the game through FUSE Remix (built from the FUSE tree per
   `FUSE_REMIX_PORT_PLAN.md`). Stock Remix's requirements are the reference: Windows 10/11 and any
   RTX GPU at minimum, recommended RTX 4070 with 12 GB VRAM and 32 GB RAM [R5]; the user's RTX 3090
   (24 GB) exceeds that. DX8 games go through d3d8to9 into DX9 [R7]. `remaster doctor` reports
   the FUSE Remix build and, if present, the stock Remix version used for parity checks. **It never
   downloads NVIDIA binaries.**
3. Capture several representative scenes in FUSE Remix (captures are written as USD [R6]).
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
2. **Gemini agent tagging** via `TAG_IMAGE` on a thumbnail, not the full texture, unless the user
   allows more. Originals need the per-game cloud opt-in (§0.1.5). It returns tags, a material class
   (`stone`, `wood`, `metal/painted`…), a physical category for the asset plan's `.fusemat`, and "is
   this text or a logo". Batch mode at half price. Without the opt-in, the same JSON-schema prompt
   runs on the `local-llm` fallback (Qwen3-VL-8B, Apache-2.0) on the 3090. The results are stored as
   tag rows, and a human spot-checks 5 %.
3. **Embeddings (optional, [SPECULATIVE])** from a local SigLIP 2 model (Apache-2.0) on the 3090 by
   default, or `gemini-embedding-2-preview` [M1] for our own content, give "find similar textures"
   searches and library matches (§4.2 step 3).

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
   - **Upscale the original** (recipe-only): PBRify_Remix 4× (CC0; CPU/ONNX in CI at ≤ 256², the
     3090 for users), then de-JPEG/de-DXT clean-up in the same model [R17]. Once trained (W3.7), the
     FUSE-SR model (ours, CC0, trained on ambientCG and Poly Haven CC0 textures with synthetic
     BC1/DXT and JPEG degradations that match legacy artefacts) becomes a second local option.
     Real-ESRGAN's official weights are not used (DIV2K is academic-only [T2]).
   - **Re-author with image editing** (recipe-only when the original is an input): Nano Banana 2
     edit with a style prompt, at 1K or 2K, with the cloud opt-in for originals. The output is
     SynthID-marked [M5]. The local FOSS option is Qwen-Image-Edit-2511 (Apache-2.0, GGUF Q4/Q5 on
     the 3090), which needs no opt-in. Useful for signs, posters and hand-painted detail. Every
     result gets a human check.
   - **Generate new** from our own prompt without the original as input (shippable): Nano Banana
     (SynthID-marked) or FLUX.1-schnell / Qwen-Image (Apache-2.0) on the 3090.
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
     meshoptimizer LODs, and retopology where the topology is broken. Blender headless (GPL,
     external process), with the recipe pinning the Blender version and script sha. Retopology uses
     Instant Meshes (BSD-3) or QuadriFlow (BSD-style) as external CLIs [3D4][3D5].
   - **Author a new hero mesh** by hand. Shippable.
   - **Image-to-3D blockout** (new props only, never from an original image): TripoSR (MIT) on the
     3090 by default, TripoSG (MIT, with BiRefNet) as the quality option, from *our own* concept
     images. The result is `origin: ai`, a starting point that is retopologised, UV'd and textured
     by hand or by the library pipeline before shipping [SPECULATIVE quality]. Hunyuan3D, TRELLIS.2
     and Stable Fast 3D are not used (§3.4.2).
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
3. **Summarise with the Gemini agent** (`gemini-3.1-pro-preview` or `gemini-3.8-flash`). Original
   script text goes to Gemini only with the per-game cloud opt-in on a paid key (§0.1.5); without
   it, the `local-llm` fallback (Qwen3-Coder-30B-A3B, Apache-2.0, on the 3090) runs the same prompts
   more slowly and with more review. Each module goes in as source plus the call-graph context, and comes out as a structured spec: state, events,
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
   - **Bandwidth extension or AI upsampling** of the original (recipe-only, [SPECULATIVE] quality):
     AudioSR (Apache-2.0 weights) on the 3090. For mixed music and effects tracks, Demucs (MIT)
     first separates stems so each is restored separately and remixed. Both outputs are
     derivatives of originals, so recipe-only.
   - Music: keep the original (recipe-only reference) or commission or compose new music. The Lyria
     models [M1] are an option for *our own* new music under the Gemini terms, with human selection
     (`human_authorship: selection`).
3. **Voice:** original VO stays as the user's files, recipe-only. New VO comes from consented human
   actors or clearly labelled synthetic voices: Kokoro-82M (Apache-2.0) by default, or Piper voices
   whose dataset licence passes review. No cloning (§0.1.8). Subtitle transcription of original VO
   uses local Whisper large-v3-turbo (MIT) on the 3090 by default, so the audio never leaves the
   machine; `gemini-3.5-transcribe` is an opt-in alternative. Transcripts are derived text, so they
   are recipe-only.
4. **Spatialisation:** in track B, map legacy 3D sound emitters to FUSE spatial mixer emitters and
   reverb zones. In track A, audio is untouched.

### 4.7 World models: FOSS on the 3090 for previs, reference and prototypes

**Default stack** (all FOSS, all local, provider `worldmodel`, task `WORLD_ROLLOUT`):

| Role | Model | Licence (code / weights) | 3090 fit | Use |
|---|---|---|---|---|
| **Default** | Matrix-Game 2.0, 1.8B [WM1][WM2] | MIT / MIT | native bf16 | Interactive keyboard/mouse rollouts at 352×640; about 6–12 fps on a 3090 [SPECULATIVE], so near-interactive |
| Quality option A | Matrix-Game 3.0 5B distilled [WM3][WM4] | MIT repo, Apache-2.0 weights | int8 DiT, text encoder on CPU [VERIFY] | Offline 720p clips with long-horizon memory; not real time on one card |
| Quality option B | LingBot-World v1 Base-Cam/Act, NF4 quant [WM6][WM8] | Apache-2.0 / Apache-2.0 | NF4, T5 on CPU, one expert at a time, 480p [VERIFY] | Offline camera-path or action clips with minute-level consistency; minutes per clip |
| Trainable | DIAMOND [WM11], Open-Oasis [WM12] | MIT code | native | Train a small, game-specific world model on *our own* FUSE captures (below) |
| Optional reference only | Project Genie, World Labs Marble | Proprietary cloud | n/a | Opt-in, human-operated; Genie is US-only and not available to the user in Australia |

All inputs are *our own* prompts, concept images or FUSE renders. An original game frame may be
used as a starting image only on the user's machine, and every output is then recipe-only or
`distribution: never`. Outputs are recorded as `LicenseRef-AI-WorldModel-Reference` with the model
SPDX id and `training_data` (Matrix-Game's data includes Unreal Engine and GTA V footage [WM2]).

What is **feasible**:

- **Mood and previs.** From a concept image or a FUSE screenshot of the remastered area, roll out a
  walk-through with keyboard/mouse actions (Matrix-Game 2.0) or a camera path (LingBot-World) to
  explore lighting moods, weather and alternative palettes before building anything.
- **Playable "feel" prototypes.** Matrix-Game 2.0 is close enough to interactive on a 3090 to let
  a stakeholder walk through a proposed area for a minute. It is not a game: no rules, no
  collision, and it drifts.
- **Reference footage** for animators and lighting artists (camera moves, foliage motion, water),
  rendered offline with Matrix-Game 3.0 or LingBot-World for quality.
- **Geometry from world-model video.** `Tools/FUSE/WorldExtract` (video → 3D world, being built
  separately) turns a rollout into splats and blockout meshes. Those are imported as POCO `Mesh`
  with `origin: ai`, then rebuilt or heavily edited by hand before anything ships [SPECULATIVE].
- **Training and reference data from FUSE captures.** FUSE can render unlimited, fully licensed
  footage of our own remastered levels with exact actions and camera poses. That footage can:
  - fine-tune Matrix-Game 2.0 (MIT) so its rollouts look like *our* remaster, for previs of areas
    not yet built [SPECULATIVE; LoRA-style fine-tunes on a 24 GB card need verifying];
  - train a small DIAMOND-style model from scratch (the CS:GO configuration took 12 days on an RTX
    4090 [WM11]; ≈ 2–3 weeks on a 3090 [SPECULATIVE]), whose weights and training data are then
    entirely ours;
  - serve as paired reference (FUSE render vs world-model rollout) for the QA reviewers.
- **Video (Veo 3.1, cloud; Wan2.2-TI2V-5B, local).** Short reference clips and animatic cutscene
  drafts from our own prompts. Final in-game cutscenes are rendered by FUSE (the cinematics module)
  or remain the original's (recipe-only). Veo output is reference only (A7), SynthID-marked [M6][M7].

What is **not feasible** (and not planned):

- Replacing the engine at runtime with any world model: resolutions are 352×640 to 720p, frame rates
  on one 3090 are below real time for everything but Matrix-Game 2.0, consistency lasts about a
  minute, and there is no state, collision or rule system [WM2][WM4][WM7].
- Getting exact level geometry, collision or gameplay rules out of world-model video. WorldExtract
  gives blockouts, not level data.
- Feeding original game footage or assets into any cloud world model (§0.1.5).
- Any CI step that needs a GPU. CI replays recorded rollouts and uses the `mock` provider.

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
4. **Track A on Windows:** a manual QA checklist on the user's 3090 machine (FUSE Remix renders
   of a real game cannot run in CI). It compares screenshots from FUSE Remix at golden poses with the
   FUSE preview of the same POCO replacements, and, where stock Remix is installed, with stock
   Remix, to catch mapping errors (a wrong hash or scale).

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
  plugin) is used unchanged. Stock Remix has its own DLSS integration [R10]; FUSE Remix uses FUSE's
  registry, with DLSS only through the optional NVIDIA plugin (port plan).
- **Legacy material fallbacks:** POCO materials without PBR maps render with the `Opaque` model,
  roughness from the tag class, and an albedo-range clamp. This keeps DDGI bounce plausible (asset
  plan §1.6).
- **Remix path tracing inside FUSE.** The stock Remix runtime exposes a C API (`remix_c.h`,
  `remixapi.dll`) that an engine can push scene data into [R1]. Porting Remix into FUSE's own stack,
  including its path tracer as a FUSE renderer backend, is now the job of
  [`FUSE_REMIX_PORT_PLAN.md`](FUSE_REMIX_PORT_PLAN.md) (adopted decision A5). This plan only
  requires that track B content and track A mod layers render the same through it.

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
| **Game projects** | init/doctor, install path, capture list, cloud opt-in toggle (with confirmation), Gemini budget meter, 3090 queue (stage, loaded model, VRAM, GPU-hours) |
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
| 11 | **Runtime capture of any DX8/9 fixed-function game** (FUSE Remix) | FUSE Remix (port plan); stock Remix as a parity reference | §4.1: USD capture → POCO (geometry is per-draw and pre-skinned or pre-transformed in some games, so it is a *starting point*, not a clean import) | dxvk-remix zlib + MIT, ported | High for track A; low as an import substitute |
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
| W0.4 Licence lock extensions (§2.6) + `fuse_lint asset-licences` checks 8–12 + the §0.1.0 allow/deny lists as data (`Content/models/licence-policy.json`) | lint mode | a failing fixture per new check, including a deny-listed sub-model; the existing asset-licence tests still pass |
| W0.5 `fuse_ai` package skeleton: Provider protocol, recipe schema, cache (replay/record/mock), budgets, audit log | `Tools/FUSE/Remaster/fuse_ai/` | `pytest` in ctest: replay miss fails; budget refusal; mock providers deterministic |
| W0.6 Binary/original-asset gates | extend `nvidia_binary_gate.cmake` patterns (`d3d8.dll`, `d3d9.dll`, `dxvk*.dll`, `NvRemix*.exe`, `.trex/`, `remixapi.dll`, `*.rtex.dds` [VERIFY]) + new `remaster_no_originals` gate (tracked files vs the local original-hash index when present; `.gitignore` covers `build/remaster-cache/`, `build/ai-cache/`) | self-test with seeded bad and good paths; skip cleanly without git |
| W0.7 `fuse_remaster` CLI skeleton: `init`, `doctor`, `report` | `Tools/FUSE/fuse_remaster.cpp` | `init` writes a project with `cloud_upload: none`; `doctor` reports "fuse-remix: unavailable (Windows only)" on Linux and "gpu: none" without CUDA |
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

### Wave 2: Remix-format interop, with the FUSE Remix port (≈ 6 tasks) [P0/P1]

The capture and mod-layer formats are shared with `FUSE_REMIX_PORT_PLAN.md`; W2.1 is done jointly
with that plan's format tasks, and neither plan duplicates the other's code.

| Task | Output | Exit criteria |
|---|---|---|
| W2.1 Research spike: pin Remix hash rules, capture layout, mod layer layout, material inputs and DDS conventions from dxvk-remix/toolkit-remix source | `docs/remaster/remix-format-notes.md` + handcrafted `.usda` fixtures (ours) | every [VERIFY] in §2.5 resolved or turned into a documented open question |
| W2.2 Vendor TinyUSDZ/LightUSD with a `VERSION` pin (or enable Assimp USD) | `Engine/lib/tinyusdz` | `fuse_lint_vendored_pins_tinyusdz`; parse the fixture capture |
| W2.3 `ingest-remix`: capture USD → POCO + hash keys | CLI | fixture capture → the expected `oaid` set; remix64 ↔ sha256 links |
| W2.4 `export-remix`: POCO replacements → `mod.usda` + DDS | CLI | output validates against our schema; re-ingest of the mod layer finds every replacement; no original blobs referenced (lint) |
| W2.5 Remix Toolkit REST client (optional live link for users who run the Omniverse Toolkit) with a mock server | `fuse_ai` provider `remix-toolkit` | mock server contract tests |
| W2.6 **[WIN/RTX manual]** end-to-end on a real owned DX9 game on the user's 3090 machine: FUSE Remix capture → 10 replacements → FUSE Remix loads them (and stock Remix, for parity) | checklist + screenshots (kept private) | checklist signed off |

### Wave 3: texture pipeline (≈ 6 tasks) [P1]

| Task | Output | Exit criteria |
|---|---|---|
| W3.1 CPU classifiers (normal/lightmap/UI/tiling/baked-light) | `fuse_ai` local tasks | labelled synthetic set: ≥ 90 % accuracy |
| W3.2 `local-onnx` provider + PBRify_Remix models pinned (sha256, CC0 record) | provider | CPU inference on a 64² → 256² fixture bit-stable across two runs |
| W3.3 PBR inference + calibration (asset plan §1.6 rules) | pipeline | outputs pass `asset_normal_map`, `asset_color_space`, `asset_pbr_sanity` |
| W3.4 Library matcher (colour stats + tags) | tool | on a synthetic set, the top-3 contains the known match ≥ 80 % |
| W3.5 Recipe rebuild with tolerance + fingerprint (§3.3) | tool | rebuild on a clean cache reproduces within tolerance; the fingerprint cannot reconstruct the input (PSNR vs the original < 15 dB at full resolution) |
| W3.6 Editor review queue (Qt) | panel | Qt offscreen test: approve/reject updates the DB and the recipe |
| W3.7 FUSE-SR: CC0 training set (ambientCG + Poly Haven, pinned sha256s) with synthetic BC1/JPEG/downscale degradations, traiNNer-redux config, ONNX export, CC0 model record | `Tools/FUSE/Remaster/train/` + model card | CPU smoke test trains 10 iterations on 8 patches; **[3090 manual]** full run beats bicubic by ≥ 2 dB PSNR on a held-out CC0 set |
| W3.8 3090 profile: VRAM-aware scheduler, stage batching, model store + `models.lock.json` (repo, revision, sha256, code and weights licences, sub-models), on-demand download with size prompt | `fuse_ai` scheduler + `fuse_ai models pull/verify` | CPU tests with fake `vram_mb`: never two large models resident; a deny-listed model or sub-model is refused; **[3090 manual]** throughput table of §3.5 measured and written back into the plan |

### Wave 4: Gemini integration (≈ 5 tasks) [P1]

| Task | Output | Exit criteria |
|---|---|---|
| W4.1 `gemini` provider (REST, pinned model ids, batch mode, context caching, retries, cost accounting) | provider | contract tests against recorded fixtures (replay); no network in CI |
| W4.2 TAG_IMAGE / INFER_MATERIAL prompts + JSON-schema-constrained outputs | prompts under `Content/recipes/ai/prompts/` | replayed outputs parse; schema violations are rejected |
| W4.3 SUMMARISE_CODE / PORT_SCRIPT with spec-first flow (§4.5) | pipeline | the fixture TorqueScript (MIT template code) → spec → Lua that passes the parity harness (replay mode) |
| W4.4 EDIT_IMAGE / GEN_IMAGE (Nano Banana) with SynthID noted in the lock | provider | the lock record includes `watermark: synthid`; recipes are recipe-only when the input is original |
| W4.5 Cost report + budget enforcement end to end (USD for Gemini; GPU-hours and kWh for local) | CLI `fuse_ai cost` | a budget breach is refused before any call |
| W4.6 `local-llm` offline fallback: llama.cpp server client, the same prompts and JSON schemas as W4.2/W4.3 | provider | replayed fixtures parse; a project without the cloud opt-in routes originals to `local-llm`, never to `gemini` (test) |

### Wave 5: mesh, lighting, audio (≈ 7 tasks) [P2]

| Task | Output | Exit criteria |
|---|---|---|
| W5.1 Mesh refine recipes (smooth normals, subdivide + displace, LODs via meshoptimizer) | Blender headless scripts | Hausdorff vs the original ≤ tolerance; LOD gates |
| W5.2 Collision-vs-render check | gate | failing fixture detected |
| W5.3 Light calibration solver (§4.4) | tool | synthetic scene with a known scale → solver recovers it within 5 % |
| W5.4 Look presets Classic/Remastered/Stylised; LUT fit to captures | `.fuselook` + tool | fitted LUT reproduces a synthetic grade (ΔE mean < 2) |
| W5.5 Audio import + restore + loudness; local Demucs, AudioSR, Whisper and Kokoro workers | pipeline | `asset_audio` gate passes on fixtures; workers mocked in CI; **[3090 manual]** one real run per model |
| W5.6 Emissive → proxy light heuristic at T0 | converter | fixture emissive surface yields a light |
| W5.7 Remix Logic graph export from event specs **[VERIFY format; may be dropped]** | exporter | schema-valid output for the fixture |
| W5.8 Image-to-3D blockouts (TripoSR default, TripoSG + BiRefNet option) + Instant Meshes retopology hand-off | `local-torch` worker + recipe | mock in CI; licence lint rejects a pipeline with RMBG or nvdiffrast |

### Wave 6: editor workspace and packaging (≈ 5 tasks) [P2]

Remaster workspace panels (§5.3) built on existing editor widgets; a patch package builder
(`ours.pak` + recipes + mappings + `CREDITS.md`); a user-side "build local pack" command. Exit:
- Qt offscreen tests for each panel;
- the packager refuses to include recipe-only or original blobs (tests);
- the package manifest lists every file with its licence.

### Wave 7: capture parity with FUSE Remix (≈ 2 tasks) [P2]

The separate FUSE D3D8/9 capture proxy is dropped (adopted decision A6): the port plan owns the
interposer, the recorder core and its Linux command-stream mock. This wave only consumes them:
- W7.1 `ingest-remix` accepts FUSE Remix captures and writes `fuse-capture64` keys next to
  `remix64`. Exit: the port plan's mock stream → the expected POCO set (CPU).
- W7.2 **[WIN/RTX manual]** a FUSE Remix capture of a real game matches the stock Remix capture's
  texture set ≥ 95 % by canonical hash.

### Wave 8: more importers (per format, ≈ 2–4 tasks each) [P2–P3]

In matrix order (§6): Quake family (enable Assimp loaders + BSP entities + PAK/PK3 VFS), Source
(BSP/VTF/VMT/VPK), Unreal via user-run umodel export, NIF clean-room.

Each format needs:
- synthetic fixtures we generate ourselves (for example MD2/MD3 written by a tiny MIT writer);
- import round-trips;
- a golden render of a fixture level.

### Wave 9: FOSS world models on the 3090 (≈ 5 tasks) [P2–P3]

| Task | Output | Exit criteria |
|---|---|---|
| W9.1 `worldmodel` provider with Matrix-Game 2.0 (MIT): action traces in, frames out; recorded rollouts replayed in CI | `fuse_ai` provider + worker | CI: the mock and replay paths pass; **[3090 manual]** 60 s rollout at 352×640 with fps and peak VRAM recorded in §3.5 |
| W9.2 Quality options: Matrix-Game 3.0 5B (int8, text encoder on CPU) and LingBot-World v1 NF4 (T5 on CPU, expert offload) | worker configs + `models.lock.json` entries | **[3090 manual]** one 480p clip from each, or a documented "does not fit" with the measured peak VRAM |
| W9.3 Hand-off to `Tools/FUSE/WorldExtract` (rollout video + camera poses → splats/blockout) and import as POCO `origin: ai` | CLI `fuse_remaster worldmodel extract` | fixture video → a POCO mesh with a `LicenseRef-AI-WorldModel-Reference` record |
| W9.4 FUSE-capture dataset writer (frames + actions + poses from our own levels) and a DIAMOND/Matrix-Game fine-tune recipe [SPECULATIVE] | dataset tool + recipe | CPU: dataset round-trip; **[3090 manual]** a short fine-tune run completes |
| W9.5 `docs/remaster/previs.md` (local world models first; Veo, Genie and Marble as opt-in references) and the reference-board asset type | doc + lock type | the lint blocks any reference asset from reaching a pack |

### 7.1 Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| **IP / derivative-work exposure** (upscaled originals shipped by mistake) | Legal | `distribution` computed, not set by hand; lint checks 8–12; packager refusal; the original-hash index gate |
| **Track A needs Windows and an RTX GPU**; CI cannot run it [R5] | Track A untestable in CI | Handcrafted USD fixtures, the port plan's command-stream mock, manual WIN/RTX checklists on the user's 3090 machine; POCO → FUSE preview as a cross-check |
| **Remix format and hash rules change** between releases (1.3 → 1.5 in six months [R8]) | Broken mod layers, FUSE Remix drifting from stock Remix | Pin the Remix format version per game project; W2.1 notes shared with the port plan; re-ingest tests; multi-key hash table; W7.2 parity check |
| **Model churn / deprecations** (Imagen 4 shut down; Gemini 2.5 restricted [M1]) | Unreproducible builds | Recipes pin model ids; cache replay is the CI source of truth; committed artefacts are reviewed outputs; every Gemini task has a local fallback (G6) |
| **Open-model licence churn** (LingBot-World went from Apache-2.0 in v1 to CC-BY-NC-SA in v2 [WM9]; Matrix-Game 2.0 is fine-tuned from a Skywork-licensed base [WM15]) | A default silently stops being FOSS | `models.lock.json` pins repo *revision* and records the licence seen at pin time; W3.8 re-checks licences on every `models pull`; upgrades across a licence change need an explicit review; the MG2 lineage question is [VERIFY] in W9.1 |
| **Non-FOSS sub-models inside FOSS pipelines** (TRELLIS.2 loads DINOv3, RMBG-2.0, nvdiffrast; HiDream loads Llama 3.1) | Licence contamination | The sub-model rule (§0.1.0) and lint check 11; `submodels` recorded per model; FOSS swaps (BiRefNet, DINOv2, gsplat, Blender bake) |
| **3090 limits** (24 GB, no FP8 tensor cores, no FlashAttention 3; vendor speeds measured on H100/A100/4090) | Slower or out-of-memory runs; world models below real time | The §3.5 scheduler (one large model at a time), int8/NF4/GGUF choices, CPU offload of text encoders, 480p for large models, overnight batch queue; W3.8/W9.1 measure real numbers |
| **World-model training data** (Matrix-Game: UE + GTA V footage; Oasis/MineWorld: Minecraft) | Outputs resemble third-party games | Reference-only outputs (`distribution: never`); fine-tune or train on our own FUSE captures (W9.4) |
| **Local model download size and disk** (≈ 12 GB default, up to ≈ 150 GB with every option) | Slow first run, disk pressure | On-demand pulls per stage with a size prompt; only the defaults are pulled by `models pull --default` (A13) |
| **AI hallucination in logic ports** | Gameplay bugs | Spec-first flow, parity harness, human review, no auto-merge |
| **Licence contamination** (NC weights like PBRFusion; GPL tools like chaiNNer, ComfyUI, nif.xml, OpenMW) | Cannot ship / relicensing | Allow/deny lists (§0.1.0, §3.4); GPL tools only as separate processes; clean-room readers |
| **Research-only training data** (Real-ESRGAN weights / DIV2K [T2]; Demucs / MUSDB18-HQ; Objaverse for TripoSR) | Legal uncertainty | CC0-trained defaults for anything that shapes shipped pixels (PBRify, FUSE-SR); models whose outputs are recipe-only or blockouts are allowed with the training data recorded |
| **Genie unavailable outside the US / no API** [G1] | None now | Genie is optional reference only; the FOSS world models run locally in Australia |
| **Cloud privacy** (free tier used for training [M3]) | Leak of user-owned assets | Opt-in plus a paid-tier-only guard for originals; upload audit log; local fallbacks for every task |
| **Cost overrun** | Budget | Gemini budgets and estimates before calls; batch mode; Flash before Pro; local models for all pixel work |
| **Legacy geometry quality** (captured geometry pre-transformed, per-draw) | Poor track B import from captures | Prefer format importers; capture only as a fallback |
| **Deterministic GPU inference** not bit-exact | Recipe mismatch | FLIP tolerance and CPU reference path |
| **Copyrightability of AI outputs** [L1] | Weak rights in our own AI assets | Record `human_authorship`; prefer human-curated or edited content for hero assets |
| **EULA / anti-circumvention** | Legal | No DRM circumvention; API interposition and file reading only; per-game review of the EULA (§0.1.9) |

### 7.2 Adopted decisions

The user asked for the best option to be picked for every open decision. These are adopted and
binding for the waves (A1–A12 replace the former open decisions D1–D12 one for one; A13 is new).
Each can be revisited through a normal plan revision.

| # | Decision | Adopted | Rationale |
|---|---|---|---|
| A1 | **First targets** | Track B: the Torque vertical slice (Wave 1) on synthetic fixtures and the in-tree MIT Torque3D template content, then the first owned Torque-era game. Track A: one owned DX9 fixed-function game from the Remix compatibility list. | Wave 1 needs no game at all; the specific owned titles are an external input (B3). |
| A2 | **Remix runtime** | FUSE Remix, the port of dxvk-remix into FUSE's own stack, per `FUSE_REMIX_PORT_PLAN.md`. Stock Remix is a parity reference only; NVIDIA SDK parts stay optional plugins. | The source is zlib + MIT, so a port is allowed and removes the dependency on NVIDIA's release cadence and Omniverse; user direction. |
| A3 | **Cloud and agent default** | Gemini (paid tier) is the default agent for our own content with no prompt. Originals go to Gemini only after the per-game opt-in (`cloud_upload` stays `none` by default). Without the opt-in, originals route to the `local-llm` fallback on the 3090. Nano Banana and Veo are cloud options under the same rule. | User direction ("use Gemini for the agent") plus the legal rule that originals stay on the machine unless the user says otherwise. |
| A4 | **USD library** | TinyUSDZ/LightUSD (Apache-2.0), vendored with a `VERSION` pin. OpenUSD (TOST) only as an optional external validator (`usdchecker`) on developer machines. | Small, dependency-free, FOSS; OpenUSD is heavy and its licence is Apache-2.0 with a modified trademark section. |
| A5 | **Remix path tracing as a FUSE backend** | Yes, delivered by the port plan, not by this plan. | The port makes it a native backend instead of a Windows-only plugin. |
| A6 | **Own D3D8/9 capture proxy** | Dropped; FUSE Remix's interposer is the capture path (Wave 7 reduced to parity). | One interposer instead of two. |
| A7 | **Shipping raw AI media** | Veo, Wan and world-model video: reference only, never shipped. Nano Banana, FLUX.1-schnell and Qwen-Image images of *our own* content: shippable after human review and edit. | Video is only ever previs here; images are useful textures once reviewed. |
| A8 | **Synthetic voice** | Kokoro-82M (Apache-2.0) placeholder VO allowed in releases only when labelled in credits and in-game; final VO from consented actors; never cloning. | FOSS, non-cloned, honest labelling; complies with §0.1.8 and SAG-AFTRA terms [L2]. |
| A9 | **Texture model allow-list** | PBRify_Remix (CC0) is the default now; FUSE-SR (ours, CC0-trained on the 3090, W3.7) is added when it beats bicubic by the W3.7 margin; Real-ESRGAN official weights and PBRFusion are rejected. | The only upscalers whose weights *and* training data are clean are CC0-trained ones. |
| A10 | **World model** | Matrix-Game 2.0 (MIT, fits the 3090 natively) is the default; LingBot-World v1 (Apache-2.0, NF4 4-bit) is the quality option; Matrix-Game 3.0 5B is a second quality option; Genie is not purchased (US-only; the user is in Australia) and stays an optional reference, as does Marble. | FOSS-first rule; MG2 is the only real-time-class FOSS world model that fits 24 GB. |
| A11 | **Patch distribution** | GitHub Releases of the patch package (our content + recipes + mappings + `CREDITS.md`) with `fuse_remaster build-local` as the installer; ModDB/Nexus mirror the same package. | Free, versioned, scriptable; the package contains nothing that needs a closed host. |
| A12 | **Test machine** | The user's RTX 3090 machine is the GPU and track A test machine. | It meets the Remix minimum and the §3.5 profile; only its Windows availability is external (B2). |
| A13 | **Local model storage and download size** | Store in `FUSE_MODEL_DIR` (default `~/.cache/fuse/models/`), outside git, pinned by `Content/models/models.lock.json`. `fuse_ai models pull --default` fetches only the defaults: PBRify (< 0.1 GB), Matrix-Game 2.0 universal (≈ 11.8 GB), TripoSR (≈ 1–2 GB [VERIFY]), Kokoro (0.33 GB), Whisper large-v3-turbo (≈ 1.6 GB), Demucs and SigLIP 2 (≈ 3 GB) ≈ **18 GB**. Options are pulled per stage after a size prompt: LingBot-World NF4 ≈ 31 GB, Matrix-Game 3.0 ≈ 41 GB, Qwen3-Coder GGUF Q4 18.6 GB, Qwen-Image-Edit GGUF ≈ 12–15 GB, Wan2.2-TI2V-5B ≈ 20–35 GB [VERIFY]. Budget ≈ 150 GB of NVMe for everything. | Keeps git small (asset plan rule), makes the default install modest, and pins revisions against licence churn. |

### 7.3 External blockers (only the user can resolve these)

| # | Blocker | Needed for |
|---|---|---|
| B1 | Run the **[3090 manual]** gates on the user's RTX 3090 (W3.7, W3.8, W5.5, W9.1–W9.4) and record the measured throughput | Replacing the [SPECULATIVE] speeds in §3.5 |
| B2 | Confirm the 3090 machine can boot **Windows 10/11** for track A (or name another Windows + RTX machine) | W2.6, W7.2 |
| B3 | Name the **owned games** for track A and track B | A1, W2.6 and the first real import |
| B4 | A paid-tier Gemini API key and billing account | The Gemini agent on originals (A3); not needed for CI or local work |

### 7.4 Cost estimates: Gemini spend plus local electricity and time [SPECULATIVE: token counts and throughput assumed]

Gemini list prices from [M2] (September 2026), batch mode (−50 %) where latency does not matter.
Local costs assume a 3090 desktop drawing ≈ 0.45 kW under load and an Australian household tariff of
about AUD 0.35/kWh **[VERIFY the user's tariff]**, so ≈ **AUD 0.16 per GPU-hour**. The figures
assume a mid-size 2000s game: about 5,000 unique textures, 1,500 meshes, 150k lines of script, 5 h of
audio and 40 levels.

| Job | Where / model | Volume assumption | Cost | Time |
|---|---|---|---|---|
| Texture tagging + material inference (thumbnails) | Gemini `gemini-3.8-flash` batch (agent) | 5,000 × (≈ 1,500 in + 300 out tokens) | ≈ US$6 batch (≈ $12 at 2027 prices) | hours (batch) |
| Script summarisation → specs | Gemini `gemini-3.1-pro-preview` | 6M in, 1.5M out | ≈ US$25–30 | hours |
| Spec → Lua port | Gemini Pro (hard modules) + Flash (simple) | 3M in, 2M out | ≈ US$20–40 | hours |
| Iteration and re-runs | Gemini, mixed | × 2–3 on the above | **≈ US$100–150 total** for logic | |
| Same logic work offline (fallback) | `local-llm` Qwen3-Coder-30B-A3B Q4 on the 3090 | ≈ 12M tokens in and out, × 2–3 | ≈ 40–120 GPU-hours ≈ AUD 6–20 | days |
| Texture upscale + PBR maps | PBRify_Remix on the 3090 | 5,000 textures | ≈ 2–4 GPU-hours ≈ AUD 0.30–0.65 | an evening |
| FUSE-SR training (once, reused across games) | traiNNer-redux on the 3090 | compact model, then a larger one | ≈ 24–170 GPU-hours ≈ AUD 4–27 | 1–7 days |
| Image re-authoring (hero textures, signs) | Nano Banana 2 at 2K, or Qwen-Image-Edit on the 3090 | 500 × 3 candidates | ≈ US$150 (batch ≈ $76), or ≈ 25–50 GPU-hours ≈ AUD 4–8 locally | hours / 1–2 days |
| Concept art / new assets | Nano Banana Pro at 2K, or FLUX.1-schnell on the 3090 | 300 images | ≈ US$40, or < 1 GPU-hour locally | |
| Image-to-3D blockouts | TripoSR on the 3090 | 300 new props | < 1 GPU-hour | |
| Audio restore, stems, super-resolution, transcription | Demucs, AudioSR, Whisper on the 3090 | 5 h audio | ≈ 1–4 GPU-hours ≈ AUD 0.20–0.65 | an evening |
| World-model previs | Matrix-Game 2.0 (default), LingBot-World NF4 / Matrix-Game 3.0 (quality) on the 3090 | 40 levels × 5 rollouts × 60 s, plus 40 quality clips | ≈ 10–20 GPU-hours ≈ AUD 2–3; quality clips ≈ 10–30 GPU-hours more | overnight runs |
| Reference / cutscene previs video | Veo 3.1 Fast or standard, or Wan2.2-TI2V-5B on the 3090 | 20 clips × 8 s × 4 takes | US$64–256, or ≈ 30–60 GPU-hours ≈ AUD 5–10 | |
| Genie | not purchased (A10) | | US$0 | |
| **Total for one mid-size remaster** | | | **Gemini ≈ US$150–250** for the agent work (logic + tagging), **plus ≈ US$0–450** if the cloud image/video options are used instead of the local ones; **local ≈ 100–300 GPU-hours ≈ AUD 15–50 of electricity** | about 1–3 weeks of background GPU time |

Budget defaults in `ai-budget.json`: US$50 per run and US$300 per month per game for Gemini, and
24 GPU-hours per run for local providers, all overridable.

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
- [R1] dxvk-remix repository (runtime; D3D9 implementation; bridge folder; Windows build requirements; Remix API docs; `LICENSE` = zlib/libpng text of the DXVK base, © Philip Rebohle, Joshua Ashton): https://github.com/NVIDIAGameWorks/dxvk-remix ; https://raw.githubusercontent.com/NVIDIAGameWorks/dxvk-remix/main/LICENSE
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

**Other world models (proprietary or non-FOSS)**
- [W1] World Labs Marble (splat/GLB export; plans): https://docs.worldlabs.ai/marble/export/gaussian-splat/index ; https://www.worldlabs.ai/blog/marble-world-model
- [W2] Tencent HunyuanWorld 1.0 (Tencent community licence; mesh export): https://github.com/Tencent-Hunyuan/HunyuanWorld-1.0
- [W3] Microsoft Muse / WHAM (Microsoft Research License; "academic research purposes only"): https://huggingface.co/microsoft/wham ; https://www.microsoft.com/en-us/research/blog/introducing-muse-our-first-generative-ai-model-designed-for-gameplay-ideation/

**FOSS world models** (licences read from the GitHub `LICENSE` file and the Hugging Face card metadata)
- [WM1] Matrix-Game 2.0 model card (MIT; 1.8B; base model SkyReels-V2-I2V-1.3B; checkpoint files) and repository (MIT `LICENSE`; "at least 24 GB", A100/H100 tested, 64 GB RAM; 352×640 latent shape in `configs/inference_yaml`): https://huggingface.co/Skywork/Matrix-Game-2.0 ; https://github.com/SkyworkAI/Matrix-Game/tree/main/Matrix-Game-2
- [WM2] Matrix-Game 2.0 paper (25 fps on a single H100; 352×640; 1.8B; ~1,200 h of Unreal Engine and GTA V data): https://arxiv.org/abs/2508.13009 ; https://arxiv.org/html/2508.13009
- [WM3] Matrix-Game 3.0 model card (Apache-2.0; 5B base and distilled; UMT5-XXL encoder; INT8; 2×14B "coming soon") and repository ("A/H series GPUs are tested", 64 GB RAM): https://huggingface.co/Skywork/Matrix-Game-3.0 ; https://github.com/SkyworkAI/Matrix-Game/tree/main/Matrix-Game-3
- [WM4] Matrix-Game 3.0 paper (720p up to 40 fps with 8 GPUs for DiT inference and 1 for VAE decoding): https://arxiv.org/abs/2604.08995
- [WM5] Matrix-Game 3.5 paper (August 2026; no weights found on Hugging Face): https://arxiv.org/abs/2608.29910
- [WM6] LingBot-World v1 model cards (Apache-2.0; Base-Cam, Base-Act, Fast; 480p/720p; 16 fps < 1 s latency) and repository (Apache-2.0 `LICENSE.txt`): https://huggingface.co/robbyant/lingbot-world-base-cam ; https://huggingface.co/robbyant/lingbot-world-fast ; https://huggingface.co/robbyant/lingbot-world-base-act-preview ; https://github.com/robbyant/lingbot-world
- [WM7] LingBot-World technical report (Wan2.2 14B base; two ~14B experts, 28B total; KV cache; Fast at 480p on "one GPU node"): https://arxiv.org/abs/2601.20540
- [WM8] LingBot-World Base-Cam NF4 community quant (Apache-2.0; ≈ 31 GB; "fits in 32GB VRAM"): https://huggingface.co/cahlen/lingbot-world-base-cam-nf4
- [WM9] LingBot-World v2 / Infinity (CC-BY-NC-SA-4.0; 14B and 1.3B causal-fast): https://huggingface.co/robbyant/lingbot-world-v2-14b-causal-fast ; https://huggingface.co/robbyant/lingbot-world-v2-1.3b-causal-fast
- [WM10] MineWorld repository (MIT; 300M–1.2B checkpoints; 4–7 fps; checkpoints taken down May 2025): https://github.com/microsoft/mineworld
- [WM11] DIAMOND repository (MIT; Atari and CS:GO; "12 days on a RTX 4090" in the csgo branch) and checkpoints (no licence tag): https://github.com/eloialonso/diamond ; https://github.com/eloialonso/diamond/tree/csgo ; https://huggingface.co/eloialonso/diamond
- [WM12] Open-Oasis (MIT code; Oasis 500M weights MIT, gated): https://github.com/etched-ai/open-oasis ; https://huggingface.co/Etched/oasis-500m
- [WM13] Hunyuan-GameCraft (Tencent Hunyuan community licence in `LICENSE`), HY-World 2.0, HunyuanWorld-Voyager and -Mirror (Tencent community licences): https://github.com/Tencent-Hunyuan/Hunyuan-GameCraft-1.0 ; https://huggingface.co/tencent/HY-World-2.0 ; https://huggingface.co/tencent/HunyuanWorld-Voyager ; https://huggingface.co/tencent/HunyuanWorld-Mirror
- [WM14] NVIDIA Cosmos-Predict 2.5 (NVIDIA Open Model License): https://huggingface.co/nvidia/Cosmos-Predict2.5-2B
- [WM15] SkyReels-V2-I2V-1.3B-540P (the Matrix-Game 2.0 base; "skywork-license"): https://huggingface.co/Skywork/SkyReels-V2-I2V-1.3B-540P

**Local LLM fallback and serving**
- [Q1] llama.cpp (MIT): https://github.com/ggml-org/llama.cpp
- [Q2] vLLM (Apache-2.0): https://github.com/vllm-project/vllm
- [Q3] Ollama (MIT): https://github.com/ollama/ollama
- [Q4] Qwen3-Coder-30B-A3B-Instruct (Apache-2.0) and GGUF builds (Q4_K_M 18.56 GB): https://huggingface.co/Qwen/Qwen3-Coder-30B-A3B-Instruct ; https://huggingface.co/unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF
- [Q5] Qwen3-VL-8B-Instruct (Apache-2.0; 8.77B) and Qwen3-VL-30B-A3B-Instruct (Apache-2.0): https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct ; https://huggingface.co/Qwen/Qwen3-VL-30B-A3B-Instruct
- [Q6] Devstral-Small-2-24B-Instruct-2512 (Apache-2.0), gpt-oss-20b (Apache-2.0), OLMo 2 32B and Molmo (Apache-2.0): https://huggingface.co/mistralai/Devstral-Small-2-24B-Instruct-2512 ; https://huggingface.co/openai/gpt-oss-20b ; https://huggingface.co/allenai/OLMo-2-0325-32B-Instruct ; https://huggingface.co/allenai/Molmo-7B-D-0924
- [Q7] SigLIP / SigLIP 2 so400m (Apache-2.0): https://huggingface.co/google/siglip-so400m-patch14-384 ; https://huggingface.co/google/siglip2-so400m-patch14-384

**Local image and video**
- [I1] FLUX.1-schnell (Apache-2.0) vs FLUX.1-dev (non-commercial licence): https://huggingface.co/black-forest-labs/FLUX.1-schnell ; https://huggingface.co/black-forest-labs/FLUX.1-dev
- [I2] Qwen-Image-2512 and Qwen-Image-Edit-2511 (Apache-2.0; 20.4B): https://huggingface.co/Qwen/Qwen-Image-2512 ; https://huggingface.co/Qwen/Qwen-Image-Edit-2511
- [I3] HiDream-I1 (MIT transformer; Llama-3.1-8B-Instruct text encoder under the Llama 3.1 licence): https://huggingface.co/HiDream-ai/HiDream-I1-Full
- [V1] Wan2.2-TI2V-5B (Apache-2.0; 720p/24 fps; 24 GB with offload flags, e.g. a 4090) and Wan2.2 repository (Apache-2.0): https://huggingface.co/Wan-AI/Wan2.2-TI2V-5B ; https://github.com/Wan-Video/Wan2.2
- [V2] Wan2.1-T2V-1.3B (Apache-2.0; 8.19 GB; 5 s 480p in about 4 min on a 4090): https://huggingface.co/Wan-AI/Wan2.1-T2V-1.3B
- [V3] LTX-Video (per-version open-weights licences) and LTX-2 (LTX-2 community licence): https://huggingface.co/Lightricks/LTX-Video ; https://huggingface.co/Lightricks/LTX-2

**3D**
- [3D1] TripoSR (MIT code and weights; trained on Objaverse renders): https://github.com/VAST-AI-Research/TripoSR ; https://huggingface.co/stabilityai/TripoSR
- [3D2] TripoSG (MIT; 1.44B; ≥ 8 GB VRAM; uses RMBG-1.4 by default): https://github.com/VAST-AI-Research/TripoSG ; https://huggingface.co/VAST-AI/TripoSG
- [3D3] TRELLIS v1 (MIT; DINOv2 conditioning; ≥ 16 GB; setup installs nvdiffrast, kaolin and a mip-splatting rasteriser): https://github.com/microsoft/TRELLIS ; https://huggingface.co/microsoft/TRELLIS-image-large
- [3D4] Instant Meshes (BSD-style `LICENSE.txt`): https://github.com/wjakob/instant-meshes
- [3D5] QuadriFlow (BSD-style `LICENSE.txt`): https://github.com/hjwdzh/QuadriFlow
- [3D6] Hunyuan3D 2 / 2.1 (Tencent Hunyuan community licence): https://huggingface.co/tencent/Hunyuan3D-2.1
- [3D7] TRELLIS.2-4B (MIT weights; `pipeline.json` loads facebook/dinov3-vitl16 and briaai/RMBG-2.0; needs nvdiffrast) and the nvdiffrast licence (NVIDIA Source Code License, non-commercial use): https://huggingface.co/microsoft/TRELLIS.2-4B ; https://github.com/microsoft/TRELLIS.2 ; https://github.com/NVlabs/nvdiffrast/blob/main/LICENSE.txt ; https://huggingface.co/facebook/dinov3-vitl16-pretrain-lvd1689m
- [3D8] Stable Fast 3D (Stability AI community licence): https://huggingface.co/stabilityai/stable-fast-3d
- [3D9] SAM 3D Objects (licence "other"): https://huggingface.co/facebook/sam-3d-objects
- [3D10] Bria RMBG-1.4 / RMBG-2.0 (Bria licences) and BiRefNet (MIT): https://huggingface.co/briaai/RMBG-2.0 ; https://huggingface.co/ZhengPeng7/BiRefNet

**Audio**
- [AU1] Demucs (MIT; the maintained fork): https://github.com/adefossez/demucs
- [AU2] AudioSR (repository `LICENSE` is MIT text; weights Apache-2.0): https://github.com/haoheliu/versatile_audio_super_resolution ; https://huggingface.co/haoheliu/audiosr_basic
- [AU3] Kokoro-82M (Apache-2.0 code and weights): https://huggingface.co/hexgrad/Kokoro-82M ; https://github.com/hexgrad/kokoro
- [AU4] Piper (MIT, archived) and its successor piper1-gpl (GPL-3.0); piper-voices (per-voice model cards): https://github.com/rhasspy/piper ; https://github.com/OHF-Voice/piper1-gpl ; https://huggingface.co/rhasspy/piper-voices
- [AU5] Whisper large-v3-turbo (MIT; 809M): https://huggingface.co/openai/whisper-large-v3-turbo ; https://github.com/ggml-org/whisper.cpp ; https://github.com/SYSTRAN/faster-whisper

**Tools and libraries**
- [T1] Real-ESRGAN licence (BSD-3-Clause): https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE
- [T2] DIV2K dataset ("academic research purpose only"): https://data.vision.ee.ethz.ch/cvl/DIV2K/
- [T3] chaiNNer (GPL-3.0, CLI): https://github.com/chaiNNer-org/chaiNNer
- [T4] OpenModelDB licence guidance: https://openmodeldb.info/docs/licenses
- [T5] INRIA 3DGS licence (non-commercial); gsplat (Apache-2.0): https://github.com/graphdeco-inria/gaussian-splatting/blob/main/LICENSE.md ; https://docs.gsplat.studio/main/
- [T6] TinyUSDZ (Apache-2.0 + MIT helpers; continued as LightUSD): https://github.com/lighttransport/tinyusdz ; https://github.com/lighttransport/LightUSD
- [T7] OpenUSD licence renamed TOST: https://forum.aousd.org/t/upcoming-openusd-license-update/1561
- [T8] ComfyUI (GPL-3.0): https://github.com/Comfy-Org/ComfyUI
- [T9] traiNNer-redux (Apache-2.0; super-resolution training): https://github.com/the-database/traiNNer-redux
- [T10] spandrel (MIT; model loader used by chaiNNer and ComfyUI): https://github.com/chaiNNer-org/spandrel
- [T11] BasicSR (Apache-2.0): https://github.com/XPixelGroup/BasicSR

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
