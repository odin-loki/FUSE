// FUSE Relight RL-6.2 tests: the C APIs (fuse_relight_api.h, remixapi_compat.h) on the CPU.
//
//   layout      every remixapi_* struct's size / alignment / field offsets and the enum values match the Remix API
//               0.6.5 reference (remixapi_layout_ref.inc, generated from upstream remix_c.h with MinGW x64 / i686),
//               checked at compile time (static_assert, this TU) and at run time from C (test_api_layout_c.c); the
//               FUSE-native structs are pinned (x64 and x86 sizes)
//   version     fuse_relight_Initialize and remixapi_InitializeLibrary negotiation; struct-size / sType checks; an
//               older 0.6 patch's shorter remixapi_Interface is not overrun
//   native      materials (RL-3.2 parameters, sanitizing, RL-4.3 emission), meshes (RL-1.3 hash components, asset
//               hash, bounds), lights (RL-4.4 lightFromUsd), instances through the RL-1.7 scene model (stable ids,
//               transform history, digests), emissive triangles and DrawLight in the light set, options (the
//               "Relight API" layer), record output size handling, stale handles
//   remix       the same through remixapi_Interface: MaterialInfo + EXT chains, HardcodedVertex meshes (same asset
//               hash as the native front end), sphere / rect / disk / cylinder / distant / USD / dome lights, 3x4
//               transforms, category bits, SetConfigVariable, Present
//   replace     an RL-3.4 mod (mesh_<asset hash> without preserve, mat_<material hash>) written to a temporary
//               directory: API draws are hidden / material-replaced as the engine decides
#include <fuse/relight/api/api_runtime.hpp>
#include <fuse/relight/api/fuse_relight_api.h>
#include <fuse/relight/api/remixapi_compat.h>
#include <fuse/relight/hash/hash_string.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" int rl_api_layout_check_c(void);

namespace {

int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

using namespace fuse::relight;
using api::ApiRuntime;
namespace fs = std::filesystem;

// ---- layout ---------------------------------------------------------------------------------------------------------

constexpr bool kIs64 = sizeof(void*) == 8;
#define RL_STRUCT(T, s64, a64, s32, a32)                                                                               \
    static_assert(sizeof(T) == (kIs64 ? (s64) : (s32)), "sizeof(" #T ") differs from Remix API 0.6.5");                \
    static_assert(alignof(T) == (kIs64 ? (a64) : (a32)), "alignof(" #T ") differs from Remix API 0.6.5");
#define RL_FIELD(T, f, o64, o32)                                                                                       \
    static_assert(offsetof(T, f) == (kIs64 ? (o64) : (o32)), "offsetof(" #T ", " #f ") differs from Remix API 0.6.5");
#define RL_ENUM(name, value) static_assert(std::uint32_t(name) == std::uint32_t(value), #name " differs from Remix API 0.6.5");
#include "remixapi_layout_ref.inc"
#undef RL_STRUCT
#undef RL_FIELD
#undef RL_ENUM

// The FUSE-native ABI (1.0), pinned: a change here is a new major version.
#define RL_NATIVE(T, s64, s32) static_assert(sizeof(T) == (kIs64 ? (s64) : (s32)), "fuse_relight ABI change: " #T);
RL_NATIVE(fuse_relight_InitInfo, 32, 28)
RL_NATIVE(fuse_relight_MaterialParam, 32, 20)
RL_NATIVE(fuse_relight_MaterialDesc, 32, 32)
RL_NATIVE(fuse_relight_SurfaceDesc, 64, 48)
RL_NATIVE(fuse_relight_MeshDesc, 24, 24)
RL_NATIVE(fuse_relight_LightParam, 24, 16)
RL_NATIVE(fuse_relight_LightDesc, 96, 88)
RL_NATIVE(fuse_relight_InstanceDesc, 88, 88)
RL_NATIVE(fuse_relight_CameraDesc, 136, 136)
RL_NATIVE(fuse_relight_FrameRecord, 104, 104)
#undef RL_NATIVE

void testLayout() {
    CHECK(rl_api_layout_check_c() == 0);
    // Handles are pointers in the Remix ABI: the runtime's handles must round-trip through them.
    CHECK(sizeof(remixapi_MeshHandle) == sizeof(void*));
}

// ---- helpers --------------------------------------------------------------------------------------------------------

constexpr float kQuad[] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
constexpr float kQuadUv[] = {0, 1, 1, 1, 1, 0, 0, 0};
constexpr std::uint32_t kQuadIdx[] = {0, 1, 2, 0, 2, 3};

std::array<float, 16> translate(float x, float y, float z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }

fuse_relight_Result init(std::uint32_t mode, const char* mod = nullptr, std::uint32_t minor = 0) {
    fuse_relight_InitInfo info{};
    info.structSize = sizeof(info);
    info.versionMajor = FUSE_RELIGHT_API_VERSION_MAJOR;
    info.versionMinor = minor;
    info.replacementMode = mode;
    info.modDirectory = mod;
    return fuse_relight_Initialize(&info);
}

fuse_relight_Handle nativeMaterial(std::uint64_t hash, const std::vector<fuse_relight_MaterialParam>& params,
                                   fuse_relight_Result* result = nullptr) {
    fuse_relight_MaterialDesc d{};
    d.structSize = sizeof(d);
    d.surfaceType = FUSE_RELIGHT_SURFACE_OPAQUE;
    d.hash = hash;
    d.params = params.data();
    d.paramCount = std::uint32_t(params.size());
    fuse_relight_Handle h = 0;
    const fuse_relight_Result r = fuse_relight_CreateMaterial(&d, &h);
    if (result != nullptr) {
        *result = r;
    }
    return h;
}

fuse_relight_Handle nativeQuad(fuse_relight_Handle material, std::uint64_t hash = 0x1234) {
    fuse_relight_SurfaceDesc s{};
    s.positions = kQuad;
    s.vertexCount = 4;
    s.texcoords = kQuadUv;
    s.indices = kQuadIdx;
    s.indexCount = 6;
    s.material = material;
    fuse_relight_MeshDesc m{};
    m.structSize = sizeof(m);
    m.surfaceCount = 1;
    m.hash = hash;
    m.surfaces = &s;
    fuse_relight_Handle h = 0;
    CHECK(fuse_relight_CreateMesh(&m, &h) == FUSE_RELIGHT_SUCCESS);
    return h;
}

fuse_relight_Result drawNative(fuse_relight_Handle mesh, const std::array<float, 16>& m, std::uint32_t categories = 0) {
    fuse_relight_InstanceDesc d{};
    d.structSize = sizeof(d);
    d.mesh = mesh;
    d.categoryFlags = categories;
    std::memcpy(d.transform, m.data(), sizeof(d.transform));
    return fuse_relight_DrawInstance(&d);
}

fuse_relight_FrameRecord endFrame() {
    fuse_relight_FrameRecord rec{};
    rec.structSize = sizeof(rec);
    CHECK(fuse_relight_EndFrame(&rec) == FUSE_RELIGHT_SUCCESS);
    return rec;
}

std::string option(const char* key) {
    char buf[128] = {};
    CHECK(fuse_relight_GetOption(key, buf, sizeof(buf)) == FUSE_RELIGHT_SUCCESS);
    return buf;
}

// ---- version --------------------------------------------------------------------------------------------------------

void testVersion() {
    std::uint32_t major = 0, minor = 9, patch = 9;
    fuse_relight_GetVersion(&major, &minor, &patch);
    CHECK(major == FUSE_RELIGHT_API_VERSION_MAJOR && minor == FUSE_RELIGHT_API_VERSION_MINOR &&
          patch == FUSE_RELIGHT_API_VERSION_PATCH);
    CHECK(fuse_relight_Initialize(nullptr) == FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    fuse_relight_InitInfo info{};
    info.structSize = sizeof(info) - 4;
    info.versionMajor = FUSE_RELIGHT_API_VERSION_MAJOR;
    CHECK(fuse_relight_Initialize(&info) == FUSE_RELIGHT_ERROR_STRUCT_SIZE);
    info.structSize = sizeof(info);
    info.versionMajor = FUSE_RELIGHT_API_VERSION_MAJOR + 1;
    CHECK(fuse_relight_Initialize(&info) == FUSE_RELIGHT_ERROR_INCOMPATIBLE_VERSION);
    CHECK(init(0, nullptr, FUSE_RELIGHT_API_VERSION_MINOR + 1) == FUSE_RELIGHT_ERROR_INCOMPATIBLE_VERSION);
    CHECK(init(7) == FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    // Nothing runs before Initialize.
    fuse_relight_Handle h = 0;
    CHECK(fuse_relight_Shutdown() == FUSE_RELIGHT_ERROR_NOT_INITIALIZED);
    CHECK(nativeMaterial(1, {}, nullptr) == 0);
    CHECK(fuse_relight_DrawLight(1) == FUSE_RELIGHT_ERROR_NOT_INITIALIZED);
    CHECK(fuse_relight_EndFrame(nullptr) == FUSE_RELIGHT_ERROR_NOT_INITIALIZED);
    fuse_relight_FrameRecord rec{};
    rec.structSize = sizeof(rec);
    CHECK(fuse_relight_GetFrameRecord(&rec) == FUSE_RELIGHT_ERROR_NOT_INITIALIZED);
    (void)h;

    // Remix negotiation.
    remixapi_InitializeLibraryInfo ri{};
    ri.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    struct {
        remixapi_Interface itf;
        void* sentinel;
    } buf{};
    ri.version = REMIXAPI_VERSION_MAKE(0, 7, 0);
    CHECK(remixapi_InitializeLibrary(&ri, &buf.itf) == REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION);
    ri.version = REMIXAPI_VERSION_MAKE(1, 6, 5);
    CHECK(remixapi_InitializeLibrary(&ri, &buf.itf) == REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION);
    ri.version = REMIXAPI_VERSION_MAKE(0, 5, 0);
    CHECK(remixapi_InitializeLibrary(&ri, &buf.itf) == REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION);
    ri.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    ri.version = REMIXAPI_VERSION_MAKE(0, 6, 5);
    CHECK(remixapi_InitializeLibrary(&ri, &buf.itf) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);
    ri.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    CHECK(remixapi_InitializeLibrary(&ri, nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);
    CHECK(remixapi_InitializeLibrary(nullptr, &buf.itf) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);
    // An older patch: every entry but the newest; the slot after it (the caller's next variable) is untouched.
    void* const kSentinel = &buf;
    buf.sentinel = kSentinel;
    std::memcpy(&buf.itf.SetCameraMediumMaterial, &kSentinel, sizeof(void*));
    ri.version = REMIXAPI_VERSION_MAKE(0, 6, 4);
    CHECK(remixapi_InitializeLibrary(&ri, &buf.itf) == REMIXAPI_ERROR_CODE_SUCCESS);
    void* medium = nullptr;
    std::memcpy(&medium, &buf.itf.SetCameraMediumMaterial, sizeof(void*));
    CHECK(medium == kSentinel);
    CHECK(buf.sentinel == kSentinel);
    CHECK(buf.itf.Present != nullptr && buf.itf.Startup != nullptr && buf.itf.CreateMesh != nullptr);
    ri.version = REMIXAPI_VERSION_MAKE(0, 6, 5);
    CHECK(remixapi_InitializeLibrary(&ri, &buf.itf) == REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(buf.itf.SetCameraMediumMaterial != nullptr && buf.sentinel == kSentinel);
    CHECK(ApiRuntime::global().initialized());
    CHECK(buf.itf.Shutdown() == REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(buf.itf.Shutdown() == REMIXAPI_ERROR_CODE_NOT_INITIALIZED);
}

// ---- native ---------------------------------------------------------------------------------------------------------

void testNative() {
    CHECK(init(0) == FUSE_RELIGHT_SUCCESS);
    ApiRuntime& rt = ApiRuntime::global();
    CHECK(rt.replacement() == nullptr);

    // Materials: RL-3.2 parameters (clamped as upstream), RL-4.3 emission = colour^2.2 x intensity.
    const std::vector<fuse_relight_MaterialParam> params = {
        {"diffuse_color_constant", {0.5f, 0.25f, 1.0f}, nullptr},
        {"reflection_roughness_constant", {2.0f, 0.f, 0.f}, nullptr},
        {"enable_emission", {1.f, 0.f, 0.f}, nullptr},
        {"emissive_intensity", {2.f, 0.f, 0.f}, nullptr},
        {"emissive_color_constant", {1.f, 1.f, 1.f}, nullptr},
        {"diffuse_texture", {0.f, 0.f, 0.f}, "textures/wall.dds"},
    };
    const fuse_relight_Handle emissive = nativeMaterial(0xE1, params);
    CHECK(emissive != 0);
    const api::ApiMaterial* m = rt.material(emissive);
    CHECK(m != nullptr);
    if (m != nullptr) {
        CHECK(m->hash == 0xE1);
        CHECK(m->params.values.at("reflection_roughness_constant").value[0] == 1.0f);
        CHECK(m->params.values.at("diffuse_texture").asset == "textures/wall.dds");
        CHECK(m->params.authored.count("diffuse_color_constant") == 1);
        CHECK(std::fabs(m->emission.x - 2.f) < 1e-5f && std::fabs(m->emission.y - 2.f) < 1e-5f);
    }
    const fuse_relight_Handle plain = nativeMaterial(0xA2, {{"diffuse_color_constant", {0.8f, 0.8f, 0.8f}, nullptr}});
    fuse_relight_Result r = FUSE_RELIGHT_SUCCESS;
    CHECK(nativeMaterial(3, {{"no_such_param", {0, 0, 0}, nullptr}}, &r) == 0 && r == FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    CHECK(nativeMaterial(3, {{"ior_constant", {1.5f, 0, 0}, nullptr}}, &r) == 0 && r == FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    {
        fuse_relight_MaterialDesc d{};
        d.structSize = sizeof(d) - 8; // an older / truncated struct
        fuse_relight_Handle h = 0;
        CHECK(fuse_relight_CreateMaterial(&d, &h) == FUSE_RELIGHT_ERROR_STRUCT_SIZE && h == 0);
        d.structSize = sizeof(d) + 16; // a newer 1.x caller: extra fields ignored
        CHECK(fuse_relight_CreateMaterial(&d, &h) == FUSE_RELIGHT_SUCCESS && h != 0);
        CHECK(fuse_relight_DestroyMaterial(h) == FUSE_RELIGHT_SUCCESS);
        CHECK(fuse_relight_DestroyMaterial(h) == FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE);
    }

    // Meshes: RL-1.3 components and the asset hash.
    const fuse_relight_Handle quadE = nativeQuad(emissive, 0x51);
    const fuse_relight_Handle quadP = nativeQuad(plain, 0x52);
    const api::ApiMesh* mesh = rt.mesh(quadE);
    CHECK(mesh != nullptr && mesh->surfaces.size() == 1);
    if (mesh != nullptr) {
        const api::ApiSurface& s = mesh->surfaces[0];
        using HC = hash::HashComponent;
        CHECK(s.hashes[HC::Positions] != 0 && s.hashes[HC::Indices] != 0 && s.hashes[HC::GeometryDescriptor] != 0 &&
              s.hashes[HC::Texcoords] != 0 && s.hashes[HC::VertexShader] == 0);
        CHECK(s.assetHash == s.hashes.hashForRule(hash::parseHashRule(hash::rules::kDefaultAssetRuleString)));
        CHECK(s.bounds.minPos.x == -1.f && s.bounds.maxPos.y == 1.f && s.bounds.maxPos.z == 0.f);
        CHECK(rt.mesh(quadP)->surfaces[0].assetHash == s.assetHash); // same geometry, same asset hash
    }
    {
        // Non-indexed: 0..n-1; an out-of-range index is refused.
        fuse_relight_SurfaceDesc s{};
        s.positions = kQuad;
        s.vertexCount = 3;
        fuse_relight_MeshDesc md{};
        md.structSize = sizeof(md);
        md.surfaceCount = 1;
        md.surfaces = &s;
        fuse_relight_Handle h = 0;
        CHECK(fuse_relight_CreateMesh(&md, &h) == FUSE_RELIGHT_SUCCESS);
        CHECK(rt.mesh(h) != nullptr && rt.mesh(h)->surfaces[0].indices.size() == 3);
        const std::uint32_t bad[] = {0, 1, 7};
        s.indices = bad;
        s.indexCount = 3;
        fuse_relight_Handle h2 = 0;
        CHECK(fuse_relight_CreateMesh(&md, &h2) == FUSE_RELIGHT_ERROR_INVALID_ARGUMENT && h2 == 0);
        s.indices = nullptr;
        s.indexCount = 0;
        s.material = 999999;
        CHECK(fuse_relight_CreateMesh(&md, &h2) == FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE);
        CHECK(fuse_relight_DestroyMesh(h) == FUSE_RELIGHT_SUCCESS);
    }

    // Lights: UsdLux parameters through lightFromUsd.
    const fuse_relight_LightParam lp[] = {{"intensity", {10.f, 0, 0}}, {"radius", {0.5f, 0, 0}}, {"color", {1.f, 0.5f, 0.25f}}};
    fuse_relight_LightDesc ld{};
    ld.structSize = sizeof(ld);
    ld.hash = 0x11;
    ld.usdType = "SphereLight";
    ld.params = lp;
    ld.paramCount = 3;
    const auto t = translate(0, 3, 0);
    std::memcpy(ld.transform, t.data(), sizeof(ld.transform));
    fuse_relight_Handle sphere = 0;
    CHECK(fuse_relight_CreateLight(&ld, &sphere) == FUSE_RELIGHT_SUCCESS);
    if (const api::ApiLight* l = rt.light(sphere)) {
        CHECK(l->light.kind == lightk::kRlKindSphere);
        CHECK(std::fabs(l->light.radius - 0.5f) < 1e-6f && std::fabs(l->light.position.y - 3.f) < 1e-6f);
        CHECK(std::fabs(l->light.radiance.x - 10.f) < 1e-5f && std::fabs(l->light.radiance.z - 2.5f) < 1e-5f);
    } else {
        CHECK(false);
    }
    ld.usdType = "DomeLight";
    fuse_relight_Handle bad = 0;
    CHECK(fuse_relight_CreateLight(&ld, &bad) == FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    ld.usdType = "SphereLight";
    const fuse_relight_LightParam zero[] = {{"radius", {0.f, 0, 0}}};
    ld.params = zero;
    ld.paramCount = 1;
    CHECK(fuse_relight_CreateLight(&ld, &bad) == FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);

    // Options: the API layer, applied at the frame end.
    CHECK(option("rtx.numFramesToKeepInstances") == "1");
    CHECK(fuse_relight_SetOption("rtx.numFramesToKeepInstances", "4") == FUSE_RELIGHT_SUCCESS);
    CHECK(fuse_relight_SetOption("rtx.noSuchOptionAnywhere", "1") == FUSE_RELIGHT_ERROR_UNKNOWN_OPTION);

    // Frame 0: two instances (one emissive quad = 2 triangles each), one sphere light.
    CHECK(drawNative(quadE, translate(0, 0, 0)) == FUSE_RELIGHT_SUCCESS);
    CHECK(drawNative(quadE, translate(10, 0, 0), REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) == FUSE_RELIGHT_SUCCESS);
    CHECK(drawNative(12345, translate(0, 0, 0)) == FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE);
    CHECK(fuse_relight_DrawLight(sphere) == FUSE_RELIGHT_SUCCESS);
    fuse_relight_FrameRecord f0 = endFrame();
    CHECK(f0.structSize == sizeof(f0));
    CHECK(f0.frame == 0);
    CHECK(f0.instancesDrawn == 2 && f0.surfacesDrawn == 2);
    CHECK(f0.sceneInstances == 2 && f0.createdInstances == 2);
    CHECK(f0.authoredLights == 1 && f0.emissiveTriangles == 4 && f0.lights == 5);
    CHECK(f0.replacementActive == 0 && f0.meshReplaced == 0);
    CHECK(f0.liveMeshes == 2 && f0.liveMaterials == 2 && f0.liveLights == 1);
    CHECK(f0.optionWrites == 1);
    CHECK(f0.rejectedCalls >= 7); // the refused calls above
    CHECK(f0.sceneDigest != 0 && f0.lightDigest != 0);
    CHECK(option("rtx.numFramesToKeepInstances") == "4");
    const std::vector<api::ApiDraw> draws0 = rt.lastFrame().draws;
    CHECK(draws0.size() == 2);
    if (draws0.size() == 2) {
        CHECK(draws0[0].instanceId != 0 && draws0[1].instanceId != 0 && draws0[0].instanceId != draws0[1].instanceId);
        CHECK(draws0[1].categories.test(scene::InstanceCategories::Sky) && !draws0[0].categories.any());
        CHECK(draws0[1].objectToWorld[12] == 10.f);
    }

    // Frame 1: the same draws -> same instances, same digests, nothing created.
    CHECK(drawNative(quadE, translate(0, 0, 0)) == FUSE_RELIGHT_SUCCESS);
    CHECK(drawNative(quadE, translate(10, 0, 0), REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) == FUSE_RELIGHT_SUCCESS);
    CHECK(fuse_relight_DrawLight(sphere) == FUSE_RELIGHT_SUCCESS);
    const fuse_relight_FrameRecord f1 = endFrame();
    CHECK(f1.frame == 1 && f1.createdInstances == 0 && f1.sceneInstances == 2);
    CHECK(f1.sceneDigest == f0.sceneDigest && f1.lightDigest == f0.lightDigest);
    CHECK(f1.rejectedCalls == 0 && f1.optionWrites == 0);
    CHECK(rt.lastFrame().draws.size() == 2 && rt.lastFrame().draws[0].instanceId == draws0[0].instanceId);

    // Frame 2: the first instance moves a little (within rtx.uniqueObjectDistance): same instance, new digest; the
    // plain quad is a new instance with no emissive triangles.
    CHECK(drawNative(quadE, translate(0.5f, 0, 0)) == FUSE_RELIGHT_SUCCESS);
    CHECK(drawNative(quadP, translate(-5, 0, 0)) == FUSE_RELIGHT_SUCCESS);
    const fuse_relight_FrameRecord f2 = endFrame();
    CHECK(f2.surfacesDrawn == 2 && f2.createdInstances == 1 && f2.emissiveTriangles == 2 && f2.lights == 2);
    CHECK(f2.sceneDigest != f1.sceneDigest);
    CHECK(rt.lastFrame().draws.size() == 2 && rt.lastFrame().draws[0].instanceId == draws0[0].instanceId);

    // A mesh destroyed after DrawInstance is dropped from the frame; output records honour the caller's size.
    CHECK(drawNative(quadP, translate(-5, 0, 0)) == FUSE_RELIGHT_SUCCESS);
    CHECK(fuse_relight_DestroyMesh(quadP) == FUSE_RELIGHT_SUCCESS);
    struct {
        fuse_relight_FrameRecord rec;
        std::uint64_t guard;
    } small{};
    std::memset(&small, 0xAB, sizeof(small));
    small.rec.structSize = 24; // an older caller's record: structSize, reserved, frame, instancesDrawn, surfacesDrawn
    CHECK(fuse_relight_EndFrame(&small.rec) == FUSE_RELIGHT_SUCCESS);
    CHECK(small.rec.structSize == 24 && small.rec.frame == 3 && small.rec.instancesDrawn == 0);
    CHECK(small.rec.sceneInstances == 0xABABABABu && small.guard == 0xABABABABABABABABull);
    fuse_relight_FrameRecord again{};
    again.structSize = sizeof(again);
    CHECK(fuse_relight_GetFrameRecord(&again) == FUSE_RELIGHT_SUCCESS && again.frame == 3 && again.liveMeshes == 1);
    again.structSize = 4;
    CHECK(fuse_relight_GetFrameRecord(&again) == FUSE_RELIGHT_ERROR_STRUCT_SIZE);

    CHECK(fuse_relight_Shutdown() == FUSE_RELIGHT_SUCCESS);
    CHECK(option("rtx.numFramesToKeepInstances") == "1"); // the API layer left with the runtime
    CHECK(rt.mesh(quadE) == nullptr);
}

// ---- remix ----------------------------------------------------------------------------------------------------------

remixapi_Interface remixInit() {
    remixapi_InitializeLibraryInfo ri{};
    ri.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    ri.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    remixapi_Interface itf{};
    CHECK(remixapi_InitializeLibrary(&ri, &itf) == REMIXAPI_ERROR_CODE_SUCCESS);
    remixapi_StartupInfo si{};
    si.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    CHECK(itf.Startup(&si) == REMIXAPI_ERROR_CODE_SUCCESS);
    return itf;
}

std::vector<remixapi_HardcodedVertex> remixQuad() {
    std::vector<remixapi_HardcodedVertex> v(4);
    for (int i = 0; i < 4; ++i) {
        v[std::size_t(i)] = remixapi_HardcodedVertex{};
        std::memcpy(v[std::size_t(i)].position, &kQuad[i * 3], sizeof(float) * 3);
        v[std::size_t(i)].normal[2] = -1.f;
        std::memcpy(v[std::size_t(i)].texcoord, &kQuadUv[i * 2], sizeof(float) * 2);
        v[std::size_t(i)].color = 0xFFFFFFFFu;
    }
    return v;
}

void testRemix() {
    const remixapi_Interface itf = remixInit();
    ApiRuntime& rt = ApiRuntime::global();

    // Material with an opaque + subsurface chain.
    remixapi_MaterialInfoOpaqueSubsurfaceEXT sss{};
    sss.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT;
    sss.subsurfaceMeasurementDistance = 0.25f;
    sss.subsurfaceTransmittanceColor = {0.5f, 0.5f, 0.5f};
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &sss;
    opaque.albedoConstant = {0.25f, 0.5f, 0.75f};
    opaque.opacityConstant = 1.f;
    opaque.roughnessConstant = 0.3f;
    opaque.metallicConstant = 0.1f;
    opaque.alphaTestType = 7;
    opaque.thinFilmThickness_hasvalue = 1;
    opaque.thinFilmThickness_value = 5000.f; // clamped to 1500 nm
    remixapi_MaterialInfo mi{};
    mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    mi.pNext = &opaque;
    mi.hash = 0xBEEF;
    mi.albedoTexture = L"tex/albedoé.dds";
    mi.emissiveIntensity = 0.f;
    mi.filterMode = 0;
    mi.wrapModeU = 1;
    remixapi_MaterialHandle mat = nullptr;
    CHECK(itf.CreateMaterial(&mi, &mat) == REMIXAPI_ERROR_CODE_SUCCESS && mat != nullptr);
    if (const api::ApiMaterial* m = rt.material(api::Handle(reinterpret_cast<std::uintptr_t>(mat)))) {
        CHECK(m->hash == 0xBEEF);
        CHECK(m->params.surface == mods::import::SurfaceType::Opaque);
        CHECK(m->params.values.at("diffuse_color_constant").value[1] == 0.5f);
        CHECK(m->params.values.at("reflection_roughness_constant").value[0] == 0.3f);
        CHECK(m->params.values.at("thin_film_thickness_constant").value[0] == 1500.f);
        CHECK(m->params.values.at("enable_thin_film").value[0] == 1.f);
        CHECK(m->params.values.at("subsurface_measurement_distance").value[0] == 0.25f);
        CHECK(m->params.values.at("diffuse_texture").asset == "tex/albedo\xc3\xa9.dds");
        CHECK(m->params.values.at("wrap_mode_u").value[0] == 1.f);
        CHECK(m->emission.x == 0.f);
    } else {
        CHECK(false);
    }
    remixapi_MaterialInfoTranslucentEXT tr{};
    tr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    tr.refractiveIndex = 1.5f;
    tr.transmittanceColor = {0.9f, 0.9f, 0.9f};
    tr.transmittanceMeasurementDistance = 2.f;
    mi.pNext = &tr;
    mi.hash = 0x7A;
    remixapi_MaterialHandle glass = nullptr;
    CHECK(itf.CreateMaterial(&mi, &glass) == REMIXAPI_ERROR_CODE_SUCCESS);
    if (const api::ApiMaterial* m = rt.material(api::Handle(reinterpret_cast<std::uintptr_t>(glass)))) {
        CHECK(m->params.surface == mods::import::SurfaceType::Translucent);
        CHECK(m->params.values.at("ior_constant").value[0] == 1.5f);
    }
    mi.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    remixapi_MaterialHandle wrong = nullptr;
    CHECK(itf.CreateMaterial(&mi, &wrong) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS && wrong == nullptr);

    // Mesh from HardcodedVertex: the same geometry hashes as the native quad.
    const std::vector<remixapi_HardcodedVertex> verts = remixQuad();
    remixapi_MeshInfoSurfaceTriangles surf{};
    surf.vertices_values = verts.data();
    surf.vertices_count = verts.size();
    surf.indices_values = kQuadIdx;
    surf.indices_count = 6;
    surf.material = mat;
    remixapi_MeshInfo meshInfo{};
    meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    meshInfo.hash = 0x99;
    meshInfo.surfaces_values = &surf;
    meshInfo.surfaces_count = 1;
    remixapi_MeshHandle mesh = nullptr;
    CHECK(itf.CreateMesh(&meshInfo, &mesh) == REMIXAPI_ERROR_CODE_SUCCESS && mesh != nullptr);
    const fuse_relight_Handle nativeTwin = nativeQuad(0);
    const api::ApiMesh* rm = rt.mesh(api::Handle(reinterpret_cast<std::uintptr_t>(mesh)));
    CHECK(rm != nullptr && rt.mesh(nativeTwin) != nullptr);
    if (rm != nullptr && rt.mesh(nativeTwin) != nullptr) {
        CHECK(rm->surfaces[0].hashes.fields == rt.mesh(nativeTwin)->surfaces[0].hashes.fields);
        CHECK(rm->surfaces[0].normals[2] == -1.f);
    }
    surf.indices_count = 5; // not a triangle list
    remixapi_MeshHandle badMesh = nullptr;
    CHECK(itf.CreateMesh(&meshInfo, &badMesh) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);

    // Lights.
    auto light = [&](const void* ext, remixapi_Float3D radiance, remixapi_ErrorCode expect) {
        remixapi_LightInfo li{};
        li.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
        li.pNext = const_cast<void*>(ext);
        li.hash = 0x42;
        li.radiance = radiance;
        remixapi_LightHandle h = nullptr;
        CHECK(itf.CreateLight(&li, &h) == expect);
        return rt.light(api::Handle(reinterpret_cast<std::uintptr_t>(h)));
    };
    remixapi_LightInfoSphereEXT sp{};
    sp.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sp.position = {1, 2, 3};
    sp.radius = 0.25f;
    sp.shaping_hasvalue = 1;
    sp.shaping_value = {{0, -1, 0}, 30.f, 0.f, 0.f};
    const api::ApiLight* lsp = light(&sp, {5, 5, 5}, REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(lsp != nullptr && lsp->light.kind == lightk::kRlKindSphere && lsp->light.position.z == 3.f &&
          std::fabs(lsp->light.cosCone - std::cos(30.f * 3.14159265f / 180.f)) < 1e-5f && lsp->light.radiance.x == 5.f);
    remixapi_LightInfoRectEXT rc{};
    rc.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT;
    rc.xAxis = {1, 0, 0};
    rc.yAxis = {0, 0, 1};
    rc.xSize = 2.f;
    rc.ySize = 1.f;
    rc.direction = {0, -1, 0};
    const api::ApiLight* lrc = light(&rc, {1, 1, 1}, REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(lrc != nullptr && lrc->light.kind == lightk::kRlKindRect && std::fabs(lrc->light.area - 2.f) < 1e-5f &&
          lrc->light.axis.y < -0.99f);
    remixapi_LightInfoDiskEXT dk{};
    dk.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT;
    dk.xAxis = {1, 0, 0};
    dk.yAxis = {0, 1, 0};
    dk.xRadius = dk.yRadius = 1.f;
    dk.direction = {0, 0, 1};
    const api::ApiLight* ldk = light(&dk, {1, 1, 1}, REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(ldk != nullptr && ldk->light.kind == lightk::kRlKindDisk && ldk->light.axis.z > 0.99f);
    remixapi_LightInfoCylinderEXT cy{};
    cy.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT;
    cy.axis = {1, 0, 0};
    cy.axisLength = 2.f;
    cy.radius = 0.1f;
    const api::ApiLight* lcy = light(&cy, {1, 1, 1}, REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(lcy != nullptr && lcy->light.kind == lightk::kRlKindCylinder);
    remixapi_LightInfoDistantEXT ds{};
    ds.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    ds.direction = {0, -1, 0};
    ds.angularDiameterDegrees = 0.5f;
    const api::ApiLight* lds = light(&ds, {3, 3, 3}, REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(lds != nullptr && lds->light.kind == lightk::kRlKindDistant);
    remixapi_LightInfoUSDEXT usd{};
    usd.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
    usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    usd.transform = remixapi_Transform{{{1, 0, 0, 4}, {0, 1, 0, 5}, {0, 0, 1, 6}}};
    const float radius = 0.5f, intensity = 8.f;
    usd.pRadius = &radius;
    usd.pIntensity = &intensity;
    const api::ApiLight* lusd = light(&usd, {0, 0, 0}, REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(lusd != nullptr && lusd->light.kind == lightk::kRlKindSphere && lusd->light.position.x == 4.f &&
          lusd->light.position.z == 6.f && lusd->light.radiance.y == 8.f);
    remixapi_LightInfoDomeEXT dome{};
    dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    const api::ApiLight* ldome = light(&dome, {1, 1, 1}, REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(ldome != nullptr && !ldome->supported);
    CHECK(light(nullptr, {1, 1, 1}, REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS) == nullptr);

    // Frame: two instances (3x4 transform, category bits), five light instances (the dome never enters the set).
    remixapi_CameraInfoParameterizedEXT cp{};
    cp.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    cp.position = {0, 0, -5};
    cp.forward = {0, 0, 1};
    cp.up = {0, 1, 0};
    cp.right = {1, 0, 0};
    cp.fovYInDegrees = 60.f;
    cp.aspect = 4.f / 3.f;
    cp.nearPlane = 0.1f;
    cp.farPlane = 100.f;
    remixapi_CameraInfo ci{};
    ci.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    ci.pNext = &cp;
    ci.type = REMIXAPI_CAMERA_TYPE_WORLD;
    CHECK(itf.SetupCamera(&ci) == REMIXAPI_ERROR_CODE_SUCCESS);
    remixapi_InstanceInfo ii{};
    ii.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    ii.mesh = mesh;
    ii.transform = remixapi_Transform{{{1, 0, 0, 7}, {0, 1, 0, 8}, {0, 0, 1, 9}}};
    ii.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL |
                       REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_TRANSPARENCY_LAYER | REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
    CHECK(itf.DrawInstance(&ii) == REMIXAPI_ERROR_CODE_SUCCESS);
    ii.transform = remixapi_Transform{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    ii.categoryFlags = 0;
    ii.doubleSided = 1;
    CHECK(itf.DrawInstance(&ii) == REMIXAPI_ERROR_CODE_SUCCESS);
    // Light handles are the runtime's: draw every light created above (the seven live ones).
    std::vector<api::Handle> lightHandles;
    for (api::Handle h = 1; h < 1000; ++h) {
        if (rt.light(h) != nullptr) {
            lightHandles.push_back(h);
        }
    }
    CHECK(lightHandles.size() == 7);
    for (api::Handle h : lightHandles) {
        CHECK(itf.DrawLightInstance(reinterpret_cast<remixapi_LightHandle>(static_cast<std::uintptr_t>(h))) ==
              REMIXAPI_ERROR_CODE_SUCCESS);
    }
    CHECK(itf.SetConfigVariable("rtx.enablePreservePath", "False") == REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(itf.SetConfigVariable("rtx.definitelyNotAnOption", "1") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);
    remixapi_PresentInfo pi{};
    pi.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    CHECK(itf.Present(&pi) == REMIXAPI_ERROR_CODE_SUCCESS);
    const fuse_relight_FrameRecord rec = rt.lastFrame().record;
    CHECK(rec.instancesDrawn == 2 && rec.surfacesDrawn == 2 && rec.sceneInstances == 2);
    CHECK(rec.authoredLights == 6 && rec.lights == 6 && rec.emissiveTriangles == 0);
    CHECK(rec.cameraValid == 1 && rec.optionWrites == 1 && rec.rejectedCalls >= 4);
    const std::vector<api::ApiDraw>& d = rt.lastFrame().draws;
    CHECK(d.size() == 2);
    if (d.size() == 2) {
        CHECK(d[0].objectToWorld[12] == 7.f && d[0].objectToWorld[13] == 8.f && d[0].objectToWorld[14] == 9.f);
        CHECK(d[0].categories.test(scene::InstanceCategories::WorldUI) &&
              d[0].categories.test(scene::InstanceCategories::IgnoreAlphaChannel) && d[0].viewModel);
        CHECK(!d[1].categories.any() && !d[1].viewModel && d[1].doubleSided);
    }
    CHECK(option("rtx.enablePreservePath") == "False");
    CHECK(itf.Present(nullptr) == REMIXAPI_ERROR_CODE_SUCCESS); // an empty frame
    CHECK(rt.lastFrame().record.instancesDrawn == 0 && rt.lastFrame().record.frame == 1);

    // Entries without a FUSE system behind them report failure; the static library has no D3D9.
    IDirect3D9Ex* d3d = nullptr;
    CHECK(itf.dxvk_CreateD3D9(0, &d3d) == REMIXAPI_ERROR_CODE_GENERAL_FAILURE && d3d == nullptr);
    CHECK(itf.SetCameraMediumMaterial(nullptr) == REMIXAPI_ERROR_CODE_GENERAL_FAILURE);
    CHECK(itf.pick_HighlightObjects(nullptr, 0, 0, 0, 0) == REMIXAPI_ERROR_CODE_GENERAL_FAILURE);
    CHECK(itf.DestroyMesh(mesh) == REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(itf.DestroyMesh(mesh) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS);
    CHECK(itf.DestroyMaterial(mat) == REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(itf.Shutdown() == REMIXAPI_ERROR_CODE_SUCCESS);
    CHECK(option("rtx.enablePreservePath") == "True");
}

// ---- replace --------------------------------------------------------------------------------------------------------

void testReplace() {
    // Hashes of the API quad (computed by the runtime, as a mod author would read them from a capture).
    CHECK(init(0) == FUSE_RELIGHT_SUCCESS);
    const fuse_relight_Handle probe = nativeQuad(0);
    const hash::Hash64 asset = ApiRuntime::global().mesh(probe)->surfaces[0].assetHash;
    CHECK(fuse_relight_Shutdown() == FUSE_RELIGHT_SUCCESS);

    const fs::path dir = fs::temp_directory_path() / "fuse_relight_api_test_mod";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "mod.usda", std::ios::binary);
        f << "#usda 1.0\n(\n    defaultPrim = \"RootNode\"\n    metersPerUnit = 1\n    upAxis = \"Y\"\n)\n\n"
             "def Xform \"RootNode\"\n{\n"
             "    def Scope \"meshes\"\n    {\n"
             "        def Xform \"mesh_" << hash::hashToString(asset) << "\"\n        {\n"
             "            int preserveOriginalDrawCall = 0\n"
             "            def Mesh \"part\"\n            {\n"
             "                int[] faceVertexCounts = [4]\n"
             "                int[] faceVertexIndices = [0, 1, 2, 3]\n"
             "                point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]\n"
             "                uniform token subdivisionScheme = \"none\"\n"
             "            }\n        }\n    }\n"
             "    def Scope \"Looks\"\n    {\n"
             "        def Material \"mat_" << hash::hashToString(0xC0FFEE) << "\"\n        {\n"
             "            token outputs:mdl:surface.connect = </RootNode/Looks/mat_" << hash::hashToString(0xC0FFEE)
          << "/Shader.outputs:out>\n"
             "            def Shader \"Shader\"\n            {\n"
             "                uniform token info:implementationSource = \"sourceAsset\"\n"
             "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
             "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n"
             "                float inputs:reflection_roughness_constant = 0.9\n"
             "                token outputs:out\n            }\n        }\n    }\n}\n";
    }
    const std::string modDir = dir.generic_string();
    CHECK(init(0, modDir.c_str()) == FUSE_RELIGHT_SUCCESS);
    ApiRuntime& rt = ApiRuntime::global();
    CHECK(rt.replacement() != nullptr);
    const fuse_relight_Handle emissive = nativeMaterial(0xE1, {{"enable_emission", {1.f, 0, 0}, nullptr}});
    const fuse_relight_Handle replaced = nativeMaterial(0xC0FFEE, {});
    const fuse_relight_Handle quad = nativeQuad(emissive);
    // A different mesh (a triangle) with the replaced material.
    fuse_relight_SurfaceDesc s{};
    s.positions = kQuad;
    s.vertexCount = 3;
    s.material = replaced;
    fuse_relight_MeshDesc md{};
    md.structSize = sizeof(md);
    md.surfaceCount = 1;
    md.surfaces = &s;
    fuse_relight_Handle tri = 0;
    CHECK(fuse_relight_CreateMesh(&md, &tri) == FUSE_RELIGHT_SUCCESS);
    for (int frame = 0; frame < 2; ++frame) {
        CHECK(drawNative(quad, translate(0, 0, 0)) == FUSE_RELIGHT_SUCCESS);
        CHECK(drawNative(tri, translate(3, 0, 0)) == FUSE_RELIGHT_SUCCESS);
        const fuse_relight_FrameRecord rec = endFrame();
        CHECK(rec.replacementActive == 1);
        CHECK(rec.meshReplaced == 1 && rec.hiddenDraws == 1 && rec.replacementParts == 1);
        CHECK(rec.materialReplaced == 1);
        CHECK(rec.emissiveTriangles == 0); // the emissive quad is hidden by its replacement
        const std::vector<api::ApiDraw>& d = rt.lastFrame().draws;
        CHECK(d.size() == 2 && d[0].hidden && d[0].meshReplaced && !d[1].hidden && d[1].materialReplaced);
        if (d.size() == 2) {
            CHECK(d[1].replacementMaterial.find("mat_" + hash::hashToString(0xC0FFEE)) != std::string::npos);
        }
    }
    CHECK(fuse_relight_Shutdown() == FUSE_RELIGHT_SUCCESS);
    fs::remove_all(dir, ec);
}

} // namespace

int main() {
    testLayout();
    testVersion();
    testNative();
    testRemix();
    testReplace();
    if (g_failures != 0) {
        std::printf("rl_api_unit: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rl_api_unit: all %d checks passed\n", g_checks);
    return 0;
}
