/* FUSE Relight RL-6.2: the FUSE-native C API of Relight (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.10, Wave R6).
 *
 * Relight's d3d9.dll exports these functions (and remixapi_InitializeLibrary, remixapi_compat.h); the
 * fuse_relight_api static library defines the same symbols for in-process use and the tests. Both front ends drive
 * one process-wide runtime (api/src/api_runtime.hpp):
 *
 *   materials  -> RL-3.2 MaterialParams (every AperturePBR table parameter by USD token) -> RL-4.3 BSDF material
 *   meshes     -> surfaces with the RL-1.3 geometry hash components computed from the data (so RL-3.4 mods can
 *                 replace an API mesh by its asset hash, mesh_<H>) and an object-space bounding box
 *   lights     -> RL-4.4 RlLight through lightFromUsd (UsdLux parameters by token) or the typed constructors
 *   instances  -> one RL-1.7 SceneModel draw per surface at EndFrame / Present, looked up in the RL-3.4
 *                 replacement engine when one is attached (mesh by asset hash, material by the material's hash);
 *                 emissive surfaces add their triangles to the RL-4.4 RelightLightSet
 *   options    -> SetOption writes an RL-0.6 option layer "Relight API" (priority 10,000,000: above rtx.conf and the
 *                 environment, below the user / quality layers), applied at the next EndFrame / Present
 *   frame      -> EndFrame builds the frame (scene model endFrame, light set build) and fills a FrameRecord
 *
 * Versioning: every top-level struct starts with `structSize` (sizeof as the caller compiled it). Inputs must be at
 * least the size of the 1.0 struct, or the call fails with FUSE_RELIGHT_ERROR_STRUCT_SIZE; larger inputs are accepted
 * (fields a newer 1.x header appends are ignored). Outputs are written up to min(structSize, the runtime's sizeof).
 * Array elements (fuse_relight_SurfaceDesc, *Param) are frozen for major version 1. Negotiate the version first:
 * fuse_relight_Initialize fails with FUSE_RELIGHT_ERROR_INCOMPATIBLE_VERSION when the caller's major differs or its
 * minor is newer than the runtime's.
 *
 * Threads: every entry point may be called from any thread; calls are serialized by one lock.
 */
#ifndef FUSE_RELIGHT_API_H
#define FUSE_RELIGHT_API_H

#include <stddef.h>
#include <stdint.h>

#ifndef FUSE_RELIGHT_API_EXPORT
#  if defined(_WIN32) && defined(FUSE_RELIGHT_API_BUILD_DLL)
#    define FUSE_RELIGHT_API_EXPORT __declspec(dllexport)
#  else
#    define FUSE_RELIGHT_API_EXPORT
#  endif
#endif
#if defined(_WIN32) && !defined(_WIN64)
#  define FUSE_RELIGHT_CALL __cdecl
#else
#  define FUSE_RELIGHT_CALL
#endif

#define FUSE_RELIGHT_API_VERSION_MAJOR 1u
#define FUSE_RELIGHT_API_VERSION_MINOR 0u
#define FUSE_RELIGHT_API_VERSION_PATCH 0u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fuse_relight_Result { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    FUSE_RELIGHT_SUCCESS = 0,
    FUSE_RELIGHT_ERROR_INVALID_ARGUMENT = 1,
    FUSE_RELIGHT_ERROR_STRUCT_SIZE = 2,
    FUSE_RELIGHT_ERROR_INCOMPATIBLE_VERSION = 3,
    FUSE_RELIGHT_ERROR_NOT_INITIALIZED = 4,
    FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE = 5,
    FUSE_RELIGHT_ERROR_UNKNOWN_OPTION = 6,
    FUSE_RELIGHT_ERROR_UNSUPPORTED = 7,
    FUSE_RELIGHT_ERROR_GENERAL = 8
} fuse_relight_Result;

/* 0 is never a valid handle. Handles are process-wide and not reused within a process. */
typedef uint64_t fuse_relight_Handle;

typedef struct fuse_relight_InitInfo { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    uint32_t structSize;
    uint32_t versionMajor; /* FUSE_RELIGHT_API_VERSION_* the caller was built with */
    uint32_t versionMinor;
    uint32_t versionPatch;
    /* Replacement engine: 0 = none (the runtime is standalone), 1 = from the relight.replace.* options (search roots,
     * mod order; nothing is attached when no mod exists). */
    uint32_t replacementMode;
    uint32_t reserved;
    /* A mod directory to load in addition (UTF-8; NULL for none): tests and tools. */
    const char* modDirectory;
} fuse_relight_InitInfo;

/* One material parameter: the AperturePBR USD token without "inputs:" (e.g. "diffuse_color_constant"). Scalars use
 * value[0]; bools / integers are stored as floats. `texture` (UTF-8 asset path) for texture parameters. */
typedef struct fuse_relight_MaterialParam { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    const char* name;
    float value[3];
    const char* texture;
} fuse_relight_MaterialParam;

typedef enum fuse_relight_SurfaceType { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    FUSE_RELIGHT_SURFACE_OPAQUE = 0,
    FUSE_RELIGHT_SURFACE_TRANSLUCENT = 1,
    FUSE_RELIGHT_SURFACE_PORTAL = 2
} fuse_relight_SurfaceType;

typedef struct fuse_relight_MaterialDesc { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    uint32_t structSize;
    uint32_t surfaceType;      /* fuse_relight_SurfaceType */
    uint64_t hash;             /* the replacement key (mat_<hash>); 0: none */
    const fuse_relight_MaterialParam* params;
    uint32_t paramCount;
    uint32_t reserved;
} fuse_relight_MaterialDesc;

/* Vertices are separate streams; normals / texcoords / colors may be NULL. */
typedef struct fuse_relight_SurfaceDesc { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    const float* positions;     /* xyz per vertex, positionStride bytes apart (0: 12) */
    uint32_t positionStride;
    uint32_t vertexCount;
    const float* normals;       /* xyz, normalStride (0: 12) */
    uint32_t normalStride;
    uint32_t texcoordStride;    /* 0: 8 */
    const float* texcoords;     /* uv */
    const uint32_t* indices;    /* triangle list; NULL: non-indexed (vertexCount / 3 triangles) */
    uint32_t indexCount;
    uint32_t reserved;
    fuse_relight_Handle material; /* 0: the default opaque material */
} fuse_relight_SurfaceDesc;

typedef struct fuse_relight_MeshDesc { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    uint32_t structSize;
    uint32_t surfaceCount;
    uint64_t hash;              /* caller's id (reported); the replacement key is the geometry asset hash */
    const fuse_relight_SurfaceDesc* surfaces;
} fuse_relight_MeshDesc;

/* A UsdLux light: usdType SphereLight / RectLight / DiskLight / CylinderLight / DistantLight, parameters by token
 * ("intensity", "color", "radius", "shaping:cone:angle", ...; unset ones take the table defaults), placed by
 * `transform` (row-major, row vectors: p' = p M; the UsdLux light looks along -Z). */
typedef struct fuse_relight_LightParam { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    const char* name;
    float value[3];
} fuse_relight_LightParam;

typedef struct fuse_relight_LightDesc { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    uint32_t structSize;
    uint32_t paramCount;
    uint64_t hash;
    const char* usdType;
    const fuse_relight_LightParam* params;
    float transform[16];
} fuse_relight_LightDesc;

typedef struct fuse_relight_InstanceDesc { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    uint32_t structSize;
    uint32_t categoryFlags;     /* Remix API instance category bits (REMIXAPI_INSTANCE_CATEGORY_BIT_*) */
    fuse_relight_Handle mesh;
    float transform[16];        /* object to world, row-major, row vectors (p' = p M) */
    uint32_t doubleSided;
    uint32_t reserved;
} fuse_relight_InstanceDesc;

typedef struct fuse_relight_CameraDesc { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    uint32_t structSize;
    uint32_t type;              /* 0 main, 1 sky, 2 view model */
    float view[16];             /* world to view, D3D (row-vector) layout */
    float projection[16];       /* view to projection, D3D layout */
} fuse_relight_CameraDesc;

/* What EndFrame / Present built. */
typedef struct fuse_relight_FrameRecord { /* fuse-lint-allow(namespace): FUSE-native C ABI type, global by definition */
    uint32_t structSize;
    uint32_t reserved;
    uint64_t frame;             /* API frame number (0 for the first EndFrame) */
    uint32_t instancesDrawn;    /* DrawInstance calls accepted this frame */
    uint32_t surfacesDrawn;     /* SceneModel draws (one per surface of each instance) */
    uint32_t sceneInstances;    /* RL-1.7 instances alive after the frame */
    uint32_t createdInstances;  /* instances created this frame */
    uint32_t lights;            /* RL-4.4 light-set entries */
    uint32_t authoredLights;    /* DrawLight lights accepted into the set */
    uint32_t emissiveTriangles;
    uint32_t meshReplaced;      /* RL-3.4: draws with a mesh replacement */
    uint32_t materialReplaced;  /* RL-3.4: kept draws with a material replacement */
    uint32_t hiddenDraws;       /* RL-3.4: originals hidden by a replacement */
    uint32_t replacementParts;  /* RL-3.4: replacement mesh parts placed */
    uint32_t replacementActive; /* 1 when a replacement engine is attached */
    uint32_t liveMeshes;
    uint32_t liveMaterials;
    uint32_t liveLights;
    uint32_t rejectedCalls;     /* calls that failed validation since the previous frame */
    uint32_t cameraValid;
    uint32_t optionWrites;      /* SetOption / SetConfigVariable values applied at this frame */
    uint64_t sceneDigest;       /* XXH64 over (instance id, mesh hash, material hash, objectToWorld) of the frame */
    uint64_t lightDigest;       /* XXH64 over the light table */
} fuse_relight_FrameRecord;

FUSE_RELIGHT_API_EXPORT void FUSE_RELIGHT_CALL fuse_relight_GetVersion(uint32_t* major, uint32_t* minor, uint32_t* patch);
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_Initialize(const fuse_relight_InitInfo* info);
/* Destroys every object and the option layer. Initialize may be called again afterwards. */
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_Shutdown(void);

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_CreateMaterial(const fuse_relight_MaterialDesc* desc, fuse_relight_Handle* out);
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DestroyMaterial(fuse_relight_Handle material);
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_CreateMesh(const fuse_relight_MeshDesc* desc, fuse_relight_Handle* out);
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DestroyMesh(fuse_relight_Handle mesh);
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_CreateLight(const fuse_relight_LightDesc* desc, fuse_relight_Handle* out);
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DestroyLight(fuse_relight_Handle light);

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_SetCamera(const fuse_relight_CameraDesc* desc);
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DrawInstance(const fuse_relight_InstanceDesc* desc);
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DrawLight(fuse_relight_Handle light);
/* An option by name (relight.* or its rtx.* twin, or an alias) from its string form. */
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_SetOption(const char* key, const char* value);
/* The option's resolved value as a string (NUL-terminated, truncated to bufferSize). */
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_GetOption(const char* key, char* buffer, uint32_t bufferSize);

/* Ends the API frame; `out` (may be NULL) receives its record. */
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_EndFrame(fuse_relight_FrameRecord* out);
/* The record of the last ended frame (FUSE_RELIGHT_ERROR_NOT_INITIALIZED before the first). */
FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_GetFrameRecord(fuse_relight_FrameRecord* out);

typedef void (FUSE_RELIGHT_CALL* PFN_fuse_relight_GetVersion)(uint32_t*, uint32_t*, uint32_t*);
typedef fuse_relight_Result (FUSE_RELIGHT_CALL* PFN_fuse_relight_Initialize)(const fuse_relight_InitInfo*);
typedef fuse_relight_Result (FUSE_RELIGHT_CALL* PFN_fuse_relight_GetFrameRecord)(fuse_relight_FrameRecord*);
typedef fuse_relight_Result (FUSE_RELIGHT_CALL* PFN_fuse_relight_GetOption)(const char*, char*, uint32_t);

#ifdef __cplusplus
}
#endif

#endif /* FUSE_RELIGHT_API_H */
