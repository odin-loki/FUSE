// FUSE Relight RL-5.1: the records and constants of the single-source path-tracing core (pt_reference_core.h has the
// file comment: the estimator, legacy alpha, PSR, portals, demodulation and the packed layouts). Included by every
// dialect before its scene accessors (which return these records), then pt_reference_core.h.

// ---- constants --------------------------------------------------------------------------------------------------

PT_CONST uint kPtParamWords = 11u;
PT_CONST uint kPtInstanceWords = 4u;
PT_CONST uint kPtTriangleWords = 10u;
PT_CONST uint kPtMaterialWords = 13u;
PT_CONST uint kPtPortalWords = 3u;
PT_CONST uint kPtMaxPortals = 16u;

// PtParams.flags
PT_CONST uint kPtFlagJitter = 1u;        ///< jitter primary rays in the pixel (off: pixel centres)
PT_CONST uint kPtFlagNee = 2u;           ///< next-event estimation
PT_CONST uint kPtFlagBsdfLights = 4u;    ///< BSDF-sampled rays collect light-set emission (MIS when NEE is on)
PT_CONST uint kPtFlagRussianRoulette = 8u;
PT_CONST uint kPtFlagPsr = 16u;          ///< primary surface replacement (G-buffer only)
PT_CONST uint kPtFlagMis = 32u;          ///< power-heuristic MIS (off with NEE + BSDF lights: both weighted 1/2)
PT_CONST uint kPtFlagsDefault = 63u;
PT_CONST uint kPtFlagRestirDi = 64u;     ///< RL-5.2: the frame's first sample takes the G-buffer vertex's direct light
                                         ///< from ReSTIR DI (ptRestirDiVertex; render/pathtrace/restir_di*)
PT_CONST uint kPtFlagDiRecord = 128u;    ///< RL-5.2 surface pass: the path stops at the G-buffer vertex after handing
                                         ///< it to ptRestirDiVertex (record mode)
PT_CONST uint kPtFlagRestirGi = 256u;    ///< RL-5.3: the frame's first sample takes the G-buffer vertex's indirect light
                                         ///< from ReSTIR GI (ptRestirGiVertex; render/pathtrace/restir_gi*)
PT_CONST uint kPtFlagGiRecord = 512u;    ///< RL-5.3 surface pass: the path stops at the G-buffer vertex after handing
                                         ///< it (and its bounce index) to ptRestirGiVertex (record mode)

// Instance flags (PtInstance)
PT_CONST uint kPtInstanceVisible = 1u;
PT_CONST uint kPtInstanceShadow = 2u;

// Material flags (w12.x)
PT_CONST uint kPtMatVertexColor = 1u;    ///< COLOR0 multiplies the base colour and alpha
PT_CONST uint kPtMatUnlit = 2u;          ///< emits its base colour, does not scatter (sky, UI-like draws)
PT_CONST uint kPtMatAlphaTest = 4u;      ///< legacy alpha test (w12.y reference, w12.z VkCompareOp)
PT_CONST uint kPtMatAlphaBlend = 8u;     ///< legacy alpha blend: opacity = alpha
PT_CONST uint kPtMatTextured = 16u;      ///< w11 names a texture (multiplies base colour and alpha)
PT_CONST uint kPtMatTwoSidedEmission = 32u;

// Cull masks (WP-6.0 RtInstanceMask)
PT_CONST uint kPtMaskVisible = 1u;
PT_CONST uint kPtMaskShadow = 2u;

PT_CONST float kPtMinAlbedo = 1e-3f;
PT_CONST float kPtFar = 1e30f;
PT_CONST uint kPtInvalid = 4294967295u;
PT_CONST uint kPtDimsPerBounce = 16u;

// PtSample.flags
PT_CONST uint kPtSampleHit = 1u;         ///< the G-buffer vertex is a surface (else sky: depth 0)
PT_CONST uint kPtSampleSpecularHit = 2u; ///< the G-buffer vertex's continuation was a specular lobe

// ---- records ----------------------------------------------------------------------------------------------------

struct PtParams {
    float3 camOrigin;
    float3 camRight;    ///< unit right x tan(half horizontal fov)
    float3 camUp;       ///< unit up x tan(half vertical fov)
    float3 camForward;  ///< unit
    float3 prevOrigin;  ///< previous frame's camera (motion)
    float3 prevRight;
    float3 prevUp;
    float3 prevForward;
    float3 sky;         ///< uniform sky radiance seen by rays leaving the scene
    uint width;
    uint height;
    uint frameSeed;
    uint maxBounces;    ///< path vertices after the camera (1: direct lighting only)
    uint rrStart;
    uint flags;         ///< kPtFlag*
    uint lightCount;
    uint analyticCount; ///< lights [0, analyticCount) of the set are analytic (tested by BSDF rays); the rest are
                        ///< emissive triangles found by the traversal
    uint psrMaxBounces;
    float rayEps;       ///< origin offset scale (x (1 + max |p|))
    float psrMirrorRoughness;
    uint sampleBase;    ///< index of the first sample of this frame (accumulation)
    uint samplesPerPixel;
    uint maxAlphaSkips;
    uint portalCount;
};

struct PtRawHit {
    bool hit;
    float t;
    uint instance; ///< GPU-scene slot (TLAS instance custom index)
    uint primitive;
    float u;       ///< barycentric of vertex 1
    float v;       ///< barycentric of vertex 2
};

struct PtLightPick {
    uint light;     ///< kPtInvalid: none
    float pdf;      ///< set density (pmf x solid angle pdf; delta: pmf)
    float3 wi;
    float dist;
    float3 radiance;
    bool delta;
};

struct PtSurface {
    float3 position;
    float3 geoNormal;     ///< unit, as authored (winding)
    float3 normal;        ///< unit shading normal, as authored
    BsdfMaterial m;
    float3 emission;      ///< emitted radiance (0: none)
    uint flags;           ///< material flags
    uint light;           ///< light-set index of an emissive triangle (kPtInvalid: none)
    uint material;
    uint portal;          ///< kPtInvalid: none
    float alpha;
};

/// One sample's contribution to a pixel.
struct PtSample {
    float3 radiance;      ///< total
    float3 emissive;      ///< collected before the G-buffer vertex
    float3 diffuse;       ///< after it, demodulated by albedoD
    float3 specular;      ///< after it, demodulated by albedoS
    float3 albedoD;
    float3 albedoS;
    float3 normal;        ///< G-buffer normal (world, facing the viewer); 0 for the sky
    float roughness;
    float depth;          ///< view depth of the G-buffer vertex (unfolded path length x cos to the view axis; 0: sky)
    float hitDist;        ///< distance G-buffer vertex -> next vertex (0: none)
    float motionX;        ///< UV motion (previous - current) of the G-buffer point
    float motionY;
    uint instance;        ///< G-buffer vertex instance slot (kPtInvalid: sky)
    uint psr;             ///< PSR chain length
    uint flags;           ///< kPtSample*
};

