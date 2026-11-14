# FUSE patches to the vendored libtheora

`vendor/xiph-theora-v1.2.0.tar.gz` stays the verbatim upstream release archive (its sha256 is pinned in
`../CMakeLists.txt`). At configure time the archive is extracted into `<build>/_fuse_ui_xiph/theora-1.2.0` and the
patches below are applied there, in order, with `git apply` (`patch -p1` where git is not installed; a changed patch
re-extracts and re-applies). Each patch is a behaviour-preserving fix; drop it when upstream carries the change.

| Patch | File | Why |
| --- | --- | --- |
| `0001-theora-encoder-shift-ub.patch` | encoder only: `lib/analyze.c` (`oc_mb_activity` edge energy), `lib/enquant.c` (`oc_enc_quantize_c`), `lib/fdct.c` (`oc_enc_fdct8x8_c`), `lib/mcenc.c` (full-pel to half-pel motion vectors), `lib/tokenize.c` (64-bit token flag masks) | Shifts with undefined behaviour, found by UBSan in `fuse_ui_theora_video` under the `fuse-asan` preset: left shifts of negative signed values (`x<<n` becomes `x*2^n`, the same value) and `(ogg_int64_t)1<<63` (now an unsigned shift converted to the signed type, the same bits). No behaviour change; the decoder (`fuse_ui_theora`, what the video widget ships) is untouched. |
