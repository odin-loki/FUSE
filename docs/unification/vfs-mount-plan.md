# FUSE VFS Mount Plan (WP-04 stub)

**Phase:** U3 / WP-04 start  
**Status:** Stub registry landed in `fuse::io::VirtualFileSystem`

---

## Goal

One logical namespace for hybrid 2D/3D content without merging legacy trees:

| Virtual prefix | MountKind | Physical source (future) |
|----------------|-----------|---------------------------|
| `/game/` | `Game` | Project `Assets/` cooked output |
| `/t3d/` | `T3DLegacy` | Quarantined T3D asset roots |
| `/t2d/` | `T2DLegacy` | Quarantined T2D asset roots |

## Threading

- Blocking read/decompress on I/O lane (future WP-04 slice).
- Workers publish `fuse::Handle<Asset>`; game thread commits into scene.

## Current stub

`Source/FUSE/Core/include/fuse/io/vfs.hpp` — mount registry + path resolve only. No async I/O yet.

## Next slices

1. Async read job + frame allocator scratch buffer  
2. Handle table for published assets  
3. TSan nightly on I/O handoff path
