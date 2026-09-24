# FUSE patches to the vendored GDeflate codec

Every edit sits between `/* FUSE-GDEFLATE begin: <patch-id> ... */` and `/* FUSE-GDEFLATE end */`
markers. Removing the marked lines gives the upstream file byte for byte; `VERSION` pins both the
patched file (`sha256:`) and the upstream one (`upstream_sha256:`). Licence: the patched file is
MIT (libdeflate) + Apache-2.0 (NVIDIA GDeflate); this record is the Apache-2.0 §4(b) notice of change.

## input-bounds (libdeflate/lib/gdeflate_decompress_template.h)

Found by the RL-3.3 malformed-stream fuzz (`rl_mods_assets_fuzz_asan`, AddressSanitizer:
heap-buffer-overflow read in the GDeflate decode loop). Upstream `ENSURE_BITS` refills a sub-stream
with `get_unaligned_le32(in_next)` without checking `in_end`, so a corrupt tile makes the decoder read
past its input without bound (the upstream wrapper assumes trusted data; DirectStorage's GPU path has
the same contract). The patch:

- reads a final partial packet zero-padded and never dereferences past `in_end`;
- past the end, feeds zero packets (as upstream libdeflate's DEFLATE decoder does) and returns
  `LIBDEFLATE_BAD_DATA` after `FUSE_GDEFLATE_MAX_OVERRUN` (4 * NUM_STREAMS) such packets.

Valid streams are unaffected: the reference round trip (`rl_mods_assets_unit`) decodes every
reference-compressed stream identically with the patched decoder, the FUSE tile walker and the
unmodified-wrapper `GDeflate::Decompress`.
