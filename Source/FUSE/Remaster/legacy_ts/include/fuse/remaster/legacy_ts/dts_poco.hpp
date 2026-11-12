// FUSE Remaster W1.2: fuse_legacy_ts, Torque DTS / DSQ -> POCO (docs/plans/FUSE_REMASTER_PLAN.md §2.3, §7 W1.2).
//
// The adapter reads shapes with the clean-room reader (Tools/FUSE/Cook dts_import.hpp; the Torque3D TSShape code is
// not in this tree) and writes POCO records + blobs into a BlobStore:
//
//   <prefix>.mesh.lod<N>      Mesh, one per visible detail level, largest size first (LOD 0 lists the others in
//                             lodIds). Streams Position / Normal / Uv0 (F32x3 / F32x3 / F32x2) and, for skins,
//                             Joints0 (U16x4, joint = index in the skeleton record) + Weights0 (F32x4, <= 4 per
//                             vertex, renormalised); indices32; one submesh per material ("materialSlot" = the
//                             material record id, "" for none); bounds; skeletonId for skins.
//   <prefix>.collision.<N>    Mesh tagged "collision": each negative-size detail (Collision-N / LOS-N), positions +
//                             indices only (there is no collision kind in §2.3; the tag marks it).
//   <prefix>.skeleton         Skeleton: every node in dts_joint_order (names, parents) + inverseBind (16 f32 per
//                             joint, column-major, FUSE space: the inverse of the node's default-pose world).
//   <prefix>.clip.<sequence>  AnimClip per sequence of the shape and of each `.dsq` given (bound to the shape's
//                             skeleton by node name): tracks = the `.fuseanim` bytes (media "fuse/anim", sampled at
//                             `sampleRate`), duration, rootMotion, events (triggers).
//   <prefix>.material.<name>  Material per material-list name (Opaque; textureSetId set when a texture was found).
//   <prefix>.texture.<name>   TextureSet with albedo = `<name>.png` / `.jpg` / `.tga` next to the shape, if present.
//
// Geometry is in FUSE space (Y up, counter-clockwise front, metres): units {length m, metersPerUnit 1, up +Y,
// handedness right}; the conversion is dts_detail_to_cooked_mesh's p' = (x, z, -y), so a POCO mesh equals what the
// cook would have produced from the same detail. Record ids are deterministic, records are written canonically,
// and blobs are content addressed, so importing twice gives byte-identical stores.
//
// Provenance: origin `original` (legacy game data; distribution is computed, so `never`) unless the caller says
// otherwise (the synthetic fixtures are `authored` + CC0-1.0). With a Database, the shape file is registered as an
// original asset (oaid from (game, "sha256", sha256 of the .dts bytes)) and the LOD 0 mesh record becomes its
// tier T0 replacement (variant "default", source "fuse_legacy_ts"); `.dsq` files register likewise (kind
// "anim_clip", their first clip as replacement).
#pragma once

#include <fuse/cook/dts_import.hpp>
#include <fuse/remaster/db.hpp>
#include <fuse/remaster/poco.hpp>
#include <fuse/remaster/store.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fuse::remaster::legacy_ts {

struct ImportOptions {
    std::string gameId = "game";
    std::string idPrefix;              ///< record id prefix; empty = "<gameId>.<file stem>"
    poco::Origin origin = poco::Origin::Original;
    std::string licenceId;             ///< e.g. "CC0-1.0" for synthetic fixtures
    std::string tool = "fuse_legacy_ts";
    f32 sampleRate = 30.f;             ///< AnimClip sampling
    bool rootMotion = true;
};

struct ImportedShape {
    std::string oaid;                       ///< set when a Database was given
    std::vector<poco::Record> lods;         ///< visible details, LOD 0 first
    std::vector<poco::Record> collision;
    std::optional<poco::Record> skeleton;
    std::vector<poco::Record> clips;
    std::vector<poco::Record> materials;
    std::vector<poco::Record> textureSets;
    std::vector<std::string> dsqOaids;      ///< one per `.dsq` (with a Database)
    /// Every record, in write order.
    [[nodiscard]] std::vector<const poco::Record*> all() const;
};

/// Import `dts` (+ `dsqs`, bound to its skeleton) into `store` (records written) and, when `database` is given,
/// register the originals. False + `error` on any read / convert / store failure (nothing is half-registered:
/// records may exist in the store, but the DB is only touched after every record was written).
bool importTorqueShape(const std::filesystem::path& dts, const std::vector<std::filesystem::path>& dsqs,
                       store::BlobStore& store, db::Database* database, const ImportOptions& options,
                       ImportedShape& out, std::string* error = nullptr);

/// Decoded mesh streams of a POCO Mesh record (reads the blobs back; F32 streams only).
struct PocoMeshData {
    std::vector<f32> positions, normals, uvs, weights;
    std::vector<u16> joints;
    std::vector<u32> indices;
};
bool loadPocoMesh(const store::BlobStore& store, const poco::Mesh& mesh, PocoMeshData& out, std::string* error = nullptr);

} // namespace fuse::remaster::legacy_ts
