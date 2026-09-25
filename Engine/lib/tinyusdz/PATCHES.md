# FUSE patches to the vendored TinyUSDZ subset

Upstream: https://github.com/lighttransport/tinyusdz at tag `v0.9.4` (commit
`dc7684519883358379964a9e6f925969d7477df3`), Apache-2.0. Every edit sits between
`/* FUSE-TINYUSDZ begin: <id> */` and `/* FUSE-TINYUSDZ end */`; `VERSION` pins each patched file twice
(`sha256:` as vendored, `upstream_sha256:` with the marked lines removed, which equals the upstream file).
Nothing else in the tree differs from upstream.

| id | file | why | used by |
|---|---|---|---|
| `usdc-primspec-fields` | `src/usdc-reader-reconstruct.cc` | The USDC (crate) layer reader built PrimSpecs without their specifier (every `over` / `class` read back as `def`) and dropped the crate `primChildren` order. Sets both from the parsed prim fields. Verified on pxr-written crate files from TinyUSDZ's own `tests/usdc` (`over-prim.usdc`). | runtime reader (RL-3.1) |
| `layer-writer-sublayers` | `src/sconv-layer.cc` | The Layer -> crate writer (`CrateWriter::ConvertLayerToSpecs`) wrote no `subLayers` and no root `primChildren` (the Stage writer does). Adds both (sublayer offsets are not written: the writer cannot encode `LayerOffset[]` out of line). | fixture USDC twins |
| `layer-writer-kind-token` | `src/sconv-layer.cc` | `kind` was written as a raw token index (`uint`), which the reader rejects; writes the token. | fixture USDC twins |
| `layer-writer-unknown-apischemas` | `src/sconv-layer.cc` | API schemas TinyUSDZ does not know (e.g. Remix's `ParticleSystemAPI`) were dropped, leaving an empty list op the reader rejects; writes them too. | fixture USDC twins |
| `layer-writer-primchildren` | `src/sconv-layer.cc` | Writes each prim's `primChildren` so a USDC twin keeps the authored child order. | fixture USDC twins |
| `layer-writer-clips` | `src/sconv-layer.cc` | Writes the `clips` prim metadata (dictionary), which the writer dropped. | fixture USDC twins |
| `layer-writer-variant-arcs` | `src/sconv-layer.cc` | Variant specs lost their `references`, `payload` and `variantSelection` fields; writes them. | fixture USDC twins |
| `writer-declared-attributes` | `src/stage-converter.cc` | Attributes declared without a value (`token outputs:out`) were skipped; writes them (type, variability, custom). | fixture USDC twins |
| `writer-attr-rendertype` | `src/stage-converter.cc` | Writes the `renderType` attribute metadata, which the writer dropped. | fixture USDC twins |

Known upstream limitations that are not patched (the reader works around or reports them):

- USDA: `subLayers` entries with layer offsets (`@a.usd@ (offset = 10)`) are a parse error. FUSE strips them before
  parsing (`stripSubLayerOffsets` in Source/FUSE/Relight/mods/usd); the Remix profile ignores offsets anyway.
- USDA: scalar `inf` / `-inf` / `nan` literals are a parse error (array elements are fine).
- USDA: a `variantSet` authored directly inside a variant's body is dropped (USDC keeps it).
- USDA: `apiSchemas` accepts only explicit and `prepend` list edits; known and unknown schema names are stored in
  separate lists, so their relative order is not preserved.
