// FUSE Remaster W1.5: fuse_legacy_dif, a clean-room reader (and a fixture writer) for Torque interior files
// (.dif: InteriorResource), docs/plans/FUSE_REMASTER_PLAN.md §6, §7 Wave 1.
//
// Clean room: written from a description of the format only (no Torque, HiGuy or other third-party code was read
// or copied while writing this file). Real-game parity cannot be tested in this repository (no user-owned files,
// plan §0.1.1), so the layout below is the "FUSE DIF profile": a precise, documented byte layout that the reader
// and the writer share. Fields marked [VERIFY] are where the profile may differ from engine-written files; the
// reader fails with a clear error (never guesses silently) when a file does not fit the profile.
//
// Byte order: little-endian. Strings: U8 length + bytes. PNG blobs: the PNG signature, then chunks up to IEND
// (skipped by chunk length; the pixels are never decoded).
//
// InteriorResource
//   U32 resourceVersion            44 (TGE / TGEA / T3D) — confident; other values are rejected
//   U8  previewIncluded            confident; when set, one PNG follows (skipped)
//   U32 numDetailLevels, then Interior x numDetailLevels
//   U32 numSubObjects, numTriggers, numPathFollowers, numForceFields, numAISpecialNodes ... (trailing game
//       objects). Read leniently: the counts are reported as warnings; a non-zero sub-object count stops the
//       read there (the geometry of every detail level is already complete) [VERIFY trailing blocks]
//
// Interior (interior file versions 0..14 accepted)
//   U32 version; U32 detailLevel; U32 minPixels; Box3F (min xyz, max xyz); SphereF (center xyz, radius)
//   U8 hasAlarmState; U32 numLightStateEntries
//   normals        U32 n, Point3F            (the shared normal pool)                       confident
//   planes         U32 n, {U16 normalIndex, F32 distance}                                   confident
//   points         U32 n, Point3F                                                           confident
//   pointVis       U32 n, U8
//   texGenEQs      U32 n, {PlaneF x, PlaneF y} (8 F32)
//   bspNodes       U32 n, {U16 planeIndex, front, back (U32 for version >= 14, else U16)}   [VERIFY widths]
//   bspSolidLeaves U32 n, {U32 surfaceIndex, U16 surfaceCount}
//   materialList   U8 listVersion, U32 n, string x n                                        confident (names)
//   windings       U32 n, U32 point index                                                   [VERIFY: some
//                  versions pack indices as U16]
//   windingIndices U32 n, {U32 windingStart, U32 windingCount}
//   edges          version >= 12: U32 n, {S32 vertex0, vertex1, normal0, normal1}           [VERIFY]
//   zones          U32 n, {U16 portalStart, U16 portalCount, U32 surfaceStart, U32 surfaceCount,
//                  version >= 12: U32 staticMeshStart, U32 staticMeshCount}                 [VERIFY]
//   zoneSurfaces   U32 n, U16;  zonePortalList U32 n, U16
//   portals        U32 n, {U16 planeIndex, U16 triFanCount, U32 triFanStart, U16 zoneFront, U16 zoneBack}
//   surfaces       U32 n, {U32 windingStart, windingCount (U32 for version >= 13, else U8), U16 planeIndex
//                  (bit 15 = plane flipped), U16 textureIndex, U32 texGenIndex, U8 surfaceFlags, U32 fanMask,
//                  lightmap info: U16 finalWord, F32 texGenXDistance, F32 texGenYDistance, U16 lightCount,
//                  U32 lightStateInfoStart, mapOffsetX/Y + mapSizeX/Y (U32 for version >= 13, else U8)}
//                  windingStart / windingCount / planeIndex / textureIndex confident; the rest [VERIFY]
//   normalLMapIndices, alarmLMapIndices   U32 n, (U32 for version >= 13, else U8)          [VERIFY]
//   nullSurfaces   U32 n, {U32 windingStart, U16 planeIndex, U8 surfaceFlags, windingCount (U32 / U8 as above)}
//   lightmaps      U32 n, {PNG lightmap, PNG light-direction map (version >= 11), U8 keep}  IGNORED: skipped
//                  without decoding; only their count is reported                            [VERIFY dir map]
//   solidLeafSurfaces U32 n, U32
//   animatedLights U32 n, {U32 nameIndex, U32 stateIndex, U16 stateCount, U16 flags, U32 duration}
//   lightStates    U32 n, {U8 r, g, b, U32 activeTime, U32 dataIndex, U16 dataCount}
//   stateDatas     U32 n, {U32 surfaceIndex, U32 mapIndex, U16 lightStateIndex}
//   stateDataBuffer U32 n, U32 flags, U8 x n;  nameBuffer U32 n, U8 x n                     [VERIFY all four]
//   subObjects     U32 n — must be 0 (sub-objects are not supported: error)
//   convexHulls    U32 n, {U32 hullStart, U16 hullCount, F32 minX, maxX, minY, maxY, minZ, maxZ,
//                  U32 surfaceStart, U16 surfaceCount, U32 planeStart, U32 polyListPlaneStart,
//                  U32 polyListPointStart, U32 polyListStringStart, version >= 12: U8 staticMesh}
//                  hullStart/hullCount/surfaceStart/surfaceCount/planeStart and the bounds confident
//   hullEmitStrings U32 n, U8; hullIndices U32 n, U32; hullPlaneIndices U32 n, U16;
//   hullEmitStringIndices U32 n, U32; hullSurfaceIndices U32 n, U32;
//   polyListPlanes U32 n, U16; polyListPoints U32 n, U32; polyListStrings U32 n, U8
//   coordBins      256 x {U32 binStart, U32 binCount}; coordBinIndices U32 n, U16; U32 coordBinMode
//   ColorI baseAmbient, ColorI alarmAmbient (4 U8 each)
//   version >= 10: U32 numStaticMeshes — must be 0 (static meshes are not supported: error)  [VERIFY]
//   version >= 11: texNormals U32 n, Point3F; texMatrices U32 n, 3 x S32; texMatIndices U32 n, U32;
//                  U32 extendedLightMapData, when non-zero U32 lightMapBorderSize + U32 reserved [VERIFY]
//
// Geometry model: a surface's winding is stored in triangle-strip order (triangle k uses winding entries
// k, k+1, k+2, odd k with the first two swapped). Brushes are the convex hulls. Lightmaps are ignored.
//
// The reader never throws: every count is checked against the bytes left, every index against its table, and
// the result carries an error string (fatal) or warnings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fuse::remaster::dif {

inline constexpr std::uint32_t kResourceVersion = 44;
inline constexpr std::uint32_t kMaxInteriorVersion = 14;
inline constexpr std::uint16_t kPlaneFlipBit = 0x8000;

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    friend bool operator==(const Vec3&, const Vec3&) = default;
};

struct Plane {
    std::uint16_t normalIndex = 0;
    float distance = 0.f;
    friend bool operator==(const Plane&, const Plane&) = default;
};

struct WindingIndex {
    std::uint32_t windingStart = 0, windingCount = 0;
    friend bool operator==(const WindingIndex&, const WindingIndex&) = default;
};

struct Surface {
    std::uint32_t windingStart = 0;
    std::uint32_t windingCount = 0;
    std::uint16_t planeIndex = 0; ///< without the flip bit
    bool planeFlipped = false;
    std::uint16_t textureIndex = 0; ///< material list index
    std::uint32_t texGenIndex = 0;
    std::uint8_t surfaceFlags = 0;
    std::uint32_t fanMask = 0;
    friend bool operator==(const Surface&, const Surface&) = default;
};

struct ConvexHull {
    std::uint32_t hullStart = 0;   ///< into hullIndices (points)
    std::uint16_t hullCount = 0;
    Vec3 min, max;
    std::uint32_t surfaceStart = 0; ///< into hullSurfaceIndices
    std::uint16_t surfaceCount = 0;
    std::uint32_t planeStart = 0;   ///< into hullPlaneIndices (count = surfaceCount)
    friend bool operator==(const ConvexHull&, const ConvexHull&) = default;
};

struct Interior {
    std::uint32_t version = kMaxInteriorVersion;
    std::uint32_t detailLevel = 0;
    std::uint32_t minPixels = 250;
    Vec3 boundsMin, boundsMax;
    Vec3 sphereCenter;
    float sphereRadius = 0.f;
    bool hasAlarmState = false;

    std::vector<Vec3> normals;
    std::vector<Plane> planes;
    std::vector<Vec3> points;
    std::vector<std::string> materials;
    std::vector<std::uint32_t> windings;
    std::vector<Surface> surfaces;
    std::vector<ConvexHull> hulls;
    std::vector<std::uint32_t> hullIndices;
    std::vector<std::uint16_t> hullPlaneIndices;
    std::vector<std::uint32_t> hullSurfaceIndices;

    std::uint32_t lightmapsSkipped = 0; ///< read: lightmaps present in the file (never decoded)
    std::uint32_t nullSurfaces = 0;     ///< read: count only

    friend bool operator==(const Interior&, const Interior&) = default;
};

struct InteriorResource {
    std::uint32_t version = kResourceVersion;
    std::vector<Interior> detailLevels;
};

struct ReadResult {
    bool ok = false;
    InteriorResource resource;
    std::string error;                 ///< set when !ok (with the byte offset)
    std::vector<std::string> warnings; ///< non-fatal notes (ignored trailing blocks, ...)
};

/// Parses a .dif file. Never throws.
ReadResult readDif(std::span<const std::uint8_t> bytes);

struct WriteOptions {
    std::uint32_t dummyLightmaps = 0; ///< lightmap entries (tiny PNGs) per detail level: the reader must skip them
    bool preview = false;             ///< write a preview PNG after the header
};

/// Writes `resource` in the FUSE DIF profile (byte-deterministic). Empty when the model is inconsistent
/// (indices out of range).
std::vector<std::uint8_t> writeDif(const InteriorResource& resource, const WriteOptions& options = {});

/// Model consistency (every index in range, windings >= 3). Empty = consistent.
std::vector<std::string> validate(const Interior& interior);

struct Triangle {
    Vec3 a, b, c;
    std::uint32_t surface = 0;
    std::uint16_t material = 0;
};

/// The surfaces as triangles (strip order, see the header comment); degenerate windings are skipped.
std::vector<Triangle> triangulate(const Interior& interior);
double triangleArea(const Triangle& t);
double totalTriangleArea(const Interior& interior);
/// Brushes = convex hulls.
std::size_t brushCount(const Interior& interior);

/// Builder for synthetic fixtures: appends an axis-aligned box brush (8 points, 6 planes from the shared
/// normal pool, 6 quad surfaces with outward planes, one convex hull) using material `material` (added to the
/// material list when new). Bounds and sphere are updated.
void addBoxBrush(Interior& interior, Vec3 min, Vec3 max, const std::string& material);

/// Deterministic JSON summary: resource version, then per detail level version, brushes, surfaces, triangles,
/// area (6 decimals), points, planes, materials, lightmaps skipped.
std::string summaryJson(const InteriorResource& resource);

} // namespace fuse::remaster::dif
