// FUSE Relight RL-5.6: the records of the volumetrics / particle composite core (rl_vol_core.h). FUSE's own code
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.7); no NVIDIA shader source was used.
//
// SINGLE SOURCE, compiled three ways like the RL-4.4 light core (kernels/light_core.h), after it:
//   C++   render/volumetrics/src/vol_cpp.hpp        (the CPU reference, VolumetricsCpu)
//   Slang render/volumetrics/shaders/rl_vol.slang
//   GLSL  render/volumetrics/shaders/rl_vol.comp
// Required macros: VOL_FN, VOL_CONST, VOL_OUT(T), VOL_INOUT(T), VOL_CTX_PARAM / VOL_CTX_ARG (C++ `const VolCpuContext&
// ctx,` / `ctx,`; shaders empty), VOL_PARAM_WORDS(name) (float4[kVolParamWords]), VOL_INT_WORDS(name)
// (uint4[kVolIntWords]).
//
// The includer declares these accessors between this file and rl_vol_core.h:
//   float4 volLoad(VOL_CTX_PARAM uint buf, uint index);             buf = kVolBuf*
//   void   volStore(VOL_CTX_PARAM uint buf, uint index, float4 v);
//   float  volLoadDepth(VOL_CTX_PARAM uint pixel);                  view depth along the camera forward (0: sky)
//   VolVertex volLoadVertex(VOL_CTX_PARAM uint vertex);             RL-3.6 GpuParticleVertex
//   uint4  volLoadSystem(VOL_CTX_PARAM uint system);                (firstQuad, quadCount, blend, flags)
//   RlLight volLoadLight(VOL_CTX_PARAM uint light);
//   VolLightPick volSampleLight(VOL_CTX_PARAM float3 p, float u0, float u1, float u2);  RL-4.4 light set (tree)
//   bool   volOccluded(VOL_CTX_PARAM float3 o, float3 d, float tmin, float tmax);        shadow ray (cull mask 2)

VOL_CONST uint kVolParamWords = 11u;
VOL_CONST uint kVolIntWords = 3u;
VOL_CONST uint kVolMaxSystems = 8u;
VOL_CONST uint kVolMaxLayers = 16u;  ///< particle layers kept per pixel (the nearest ones)
VOL_CONST uint kVolMaxCandidates = 32u;
VOL_CONST uint kVolLinearGroup = 64u;
VOL_CONST uint kVolTile = 8u;

// Buffers (accessor `buf`; the GPU block's address slots in this order).
VOL_CONST uint kVolBufCurrent = 0u;    ///< f32x4 per froxel: sigma_s x in-scattered radiance, sigma_t
VOL_CONST uint kVolBufHistPrev = 1u;   ///< f32x4 per froxel: last frame's filtered values
VOL_CONST uint kVolBufHistCur = 2u;    ///< f32x4 per froxel: this frame's filtered values
VOL_CONST uint kVolBufResPrev = 3u;    ///< 2 x f32x4 per froxel: last frame's light reservoirs
VOL_CONST uint kVolBufResCur = 4u;     ///< 2 x f32x4 per froxel: this frame's light reservoirs
VOL_CONST uint kVolBufIntegrated = 5u; ///< f32x4 per froxel: in-scattering to the camera, transmittance (far boundary)
VOL_CONST uint kVolBufColorIn = 6u;    ///< f32x4 per pixel: the radiance to composite over
VOL_CONST uint kVolBufColorOut = 7u;   ///< f32x4 per pixel: the composite

// Stages (one kernel, push constant `stage`).
VOL_CONST uint kVolStageInject = 0u;
VOL_CONST uint kVolStageTemporal = 1u;
VOL_CONST uint kVolStageIntegrate = 2u;
VOL_CONST uint kVolStageApply = 3u;

// VolParams::flags
VOL_CONST uint kVolFlagHistory = 1u;    ///< the previous frame's history / reservoirs are valid
VOL_CONST uint kVolFlagReproject = 2u;  ///< history is reprojected through the previous camera (else same froxel)
VOL_CONST uint kVolFlagReuse = 4u;      ///< temporal reservoir reuse (needs kVolFlagHistory)
VOL_CONST uint kVolFlagShadows = 8u;    ///< shadow ray for the selected light sample
VOL_CONST uint kVolFlagLights = 16u;    ///< in-scattering from the light set (else ambient only)
VOL_CONST uint kVolFlagFog = 32u;       ///< the apply stage composites the medium
VOL_CONST uint kVolFlagParticles = 64u; ///< the apply stage composites the particle systems

// Particle system blend modes (D3D blend states of legacy particles).
VOL_CONST uint kVolBlendAlpha = 0u;          ///< SRCALPHA, INVSRCALPHA
VOL_CONST uint kVolBlendAdditive = 1u;       ///< SRCALPHA, ONE
VOL_CONST uint kVolBlendPremultiplied = 2u;  ///< ONE, INVSRCALPHA (colour premultiplied by alpha)
VOL_CONST uint kVolBlendMultiply = 3u;       ///< DESTCOLOR, ZERO
// Particle system flags.
VOL_CONST uint kVolSystemSoftDisc = 1u;      ///< alpha x max(0, 1 - r^2) over the quad (procedural round sprite)

struct VolParams {
    float3 camPos;
    float nearZ;      ///< first slice boundary scale: boundary k >= 1 at nearZ x (farZ / nearZ)^(k / gz)
    float3 camRight;  ///< unit right x tan(half horizontal fov)
    float farZ;       ///< last slice boundary (the medium's range)
    float3 camUp;     ///< unit up x tan(half vertical fov)
    float g;          ///< Henyey-Greenstein anisotropy
    float3 camFwd;    ///< unit forward
    float alpha;      ///< temporal filter: weight of the current frame
    float3 prevPos;
    float falloff;    ///< height fog falloff
    float3 prevRight;
    float baseHeight;
    float3 prevUp;
    float density;    ///< extinction at / below baseHeight
    float3 prevFwd;
    float rayEps;
    float3 albedo;
    float mCap;       ///< reservoir reuse: history M <= mCap x candidates
    float3 ambient;   ///< isotropic ambient radiance scattered by the medium
    float lightScale; ///< global multiplier of the light set's in-scattering
    uint gx;
    uint gy;
    uint gz;
    uint flags;
    uint frame;
    uint candidates;
    uint width;
    uint height;
    uint systemCount;
    uint lightCount;
    uint pad0;
    uint pad1;
};

struct VolVertex {
    float3 position;
    float4 color; ///< linear rgba from the B8G8R8A8 vertex colour
};

struct VolLightPick {
    uint light;        ///< >= lightCount: none
    float pdf;         ///< light-set density (delta: pmf)
    float3 wi;
    float dist;
    float3 radiance;
    uint flags;        ///< kRlSample*
};

VOL_FN VolParams volParamsUnpack(VOL_PARAM_WORDS(w), VOL_INT_WORDS(iw)) {
    VolParams P;
    P.camPos = float3(w[0].x, w[0].y, w[0].z);
    P.nearZ = w[0].w;
    P.camRight = float3(w[1].x, w[1].y, w[1].z);
    P.farZ = w[1].w;
    P.camUp = float3(w[2].x, w[2].y, w[2].z);
    P.g = w[2].w;
    P.camFwd = float3(w[3].x, w[3].y, w[3].z);
    P.alpha = w[3].w;
    P.prevPos = float3(w[4].x, w[4].y, w[4].z);
    P.falloff = w[4].w;
    P.prevRight = float3(w[5].x, w[5].y, w[5].z);
    P.baseHeight = w[5].w;
    P.prevUp = float3(w[6].x, w[6].y, w[6].z);
    P.density = w[6].w;
    P.prevFwd = float3(w[7].x, w[7].y, w[7].z);
    P.rayEps = w[7].w;
    P.albedo = float3(w[8].x, w[8].y, w[8].z);
    P.mCap = w[8].w;
    P.ambient = float3(w[9].x, w[9].y, w[9].z);
    P.lightScale = w[9].w;
    P.gx = iw[0].x;
    P.gy = iw[0].y;
    P.gz = iw[0].z;
    P.flags = iw[0].w;
    P.frame = iw[1].x;
    P.candidates = iw[1].y;
    P.width = iw[1].z;
    P.height = iw[1].w;
    P.systemCount = iw[2].x;
    P.lightCount = iw[2].y;
    P.pad0 = 0u;
    P.pad1 = 0u;
    return P;
}
