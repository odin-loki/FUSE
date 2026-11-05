// FUSE Remaster W1.1: synthetic Torque fixtures (docs/plans/FUSE_REMASTER_PLAN.md §7 Wave 1).
//
// Every binary fixture is generated from procedural meshes written in this file (CC0: authored here, no source
// asset) through the clean-room TSShape writer (Tools/FUSE/Cook dts_write.hpp), so nothing derived from a game is
// committed. The text fixtures (a tiny mission and a script) are committed under Tests/fixtures/remaster/torque/
// next to `generated.sha256`, the pinned hashes of the generated files (they must regenerate byte-identically).
//
// Generated files (paths relative to the fixture root, Torque space: Z up, 1 unit = 1 m):
//   shapes/crate.dts       v24 (16-bit indices), rigid: 1 m cube standing on z = 0, details detail128 (8 x 8 quads
//                          per face, 768 triangles), detail32 (2 x 2, 48 triangles) and Collision-1 (size -1, a
//                          12-triangle box on the Col-1 object), material "crate_mat"
//   shapes/crate_mat.png   512 x 512 RGBA checker (64 px tiles), the crate's albedo (512 px/m on the 1 m faces)
//   shapes/arm.dts         v26 (32-bit indices), skinned: a 0.2 m square tube 1.6 m tall over the joints root ->
//                          upper -> lower -> tip, details detail64 (8 rings, 68 triangles) and detail16 (2 rings,
//                          20 triangles), material "arm_mat"; one embedded sequence "wave" (cyclic, 8 keys, 1 s:
//                          upper / lower rotations, root translation, 4 ground frames, trigger 1 on / off)
//   shapes/arm_anims.dsq   v26, nodes listed tip, lower, upper, root (reversed: binds by name): "walk" (one-shot,
//                          5 keys, 0.8 s, lower rotation + tip aligned scale, footstep triggers 1 and 2) and "idle"
//                          (cyclic, 4 keys, 2 s, upper rotation + root uniform scale)
#pragma once

#include <fuse/cook/dts_import.hpp>
#include <fuse/types.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fuse::remaster::legacy_ts {

struct FixtureFile {
    std::string path; ///< relative, generic separators
    std::vector<u8> bytes;
};

/// The procedural shapes (as written).
[[nodiscard]] cook::DtsShape fixtureCrateShape();
[[nodiscard]] cook::DtsShape fixtureArmShape();
[[nodiscard]] cook::DsqFile fixtureArmDsq();
/// The crate's albedo as RGBA8 rows top-down (512 x 512).
[[nodiscard]] std::vector<u8> fixtureCrateAlbedoRgba(u32& width, u32& height);

/// Uncompressed (stored-deflate) PNG of RGBA8 rows: deterministic, readable by any PNG decoder.
[[nodiscard]] std::vector<u8> encodePngRgba8(const u8* rgba, u32 width, u32 height);

/// Every generated fixture file, sorted by path. Deterministic. Empty + `error` if the writer refuses a shape.
std::vector<FixtureFile> buildTorqueFixtures(std::string* error = nullptr);

/// buildTorqueFixtures written below `root` (directories created). False + `error` on failure.
bool writeTorqueFixtures(const std::filesystem::path& root, std::string* error = nullptr);

/// "sha256  path\n" lines for `files` (sorted by path): the `generated.sha256` format.
[[nodiscard]] std::string fixtureManifest(const std::vector<FixtureFile>& files);

/// The committed text fixtures copied next to the generated ones (fixture.mis, fixture.cs).
[[nodiscard]] std::vector<std::string> committedTorqueFixtureNames();

} // namespace fuse::remaster::legacy_ts
