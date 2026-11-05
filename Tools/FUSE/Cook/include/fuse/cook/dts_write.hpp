#pragma once

// Remaster W1.1: clean-room writer for Torque `.dts` shapes (versions 24..26) and `.dsq` sequence files, the
// inverse of dts_import.hpp (whose header describes the layout). It exists to make synthetic fixtures from our own
// (CC0) meshes: the Torque3D TSShape writer is not in this tree (no Engine/source/ts), and the reader is clean-room,
// so the writer is too. Output is deterministic (same DtsShape -> same bytes) and reads back through parse_dts /
// parse_dsq to the same data.
//
// What the writer derives instead of taking from DtsShape:
//   * the name table (node, object, detail, then sequence names, first appearance, deduplicated);
//   * node first object / first child / next sibling, object next sibling (-1) and first decal (-1);
//   * one sub-shape covering every node and object;
//   * one default object state per object (visible, frame 0, material frame 0), no decals, no IFL materials;
//   * shape and mesh bounds, centres and radii from the vertices (rigid meshes through their node's default pose);
//   * detail errors (-1) and, for v26, zeroed billboard settings;
//   * skin initial transforms (inverse default-pose world of each bone node) when the mesh carries none;
//   * the material list's per-material tables (wrap flags, own reflectance map, no bump / detail map, scale 1).
// Mesh types: Standard, Skin and Null (decal / sorted meshes are refused, like the reader does). Skins are written
// with their bind-pose vertices in the skin block (the standard block carries uvs and primitives only).

#include <fuse/cook/dts_import.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

struct DtsWriteOptions {
    u32 version = 26;          ///< 24, 25 or 26 (v27+ shared vertex buffers are not written)
    u32 exporter_version = 0;  ///< high 16 bits of the version word
};

/// Serialize `shape` (nodes, objects, details, meshes, keyframe pools, sequences, triggers, materials). Empty
/// result + `error` when the shape is inconsistent (indices out of range, unsupported mesh types, bad version).
std::vector<u8> write_dts(const DtsShape& shape, const DtsWriteOptions& options = {}, std::string* error = nullptr);

/// Serialize a `.dsq` (version from `dsq.version`, 22..28; 0 means 26).
std::vector<u8> write_dsq(const DsqFile& dsq, std::string* error = nullptr);

/// Quantise a unit quaternion (x, y, z, w) to Quat16 (round to nearest, clamped).
[[nodiscard]] DtsQuat16 dts_quat16(f32 x, f32 y, f32 z, f32 w);

/// Write `bytes` to `path` (creating parent directories). False on I/O failure.
bool write_binary_file(const std::string& path, const std::vector<u8>& bytes, std::string* error = nullptr);

} // namespace fuse::cook
