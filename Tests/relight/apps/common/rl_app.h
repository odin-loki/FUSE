// FUSE Relight test-app kit (RL-0.4, plan §6.2).
//
// Every app is one scene (scenes/<name>.cpp) written once against rl::Gfx, a thin D3D9-shaped
// wrapper, and built twice when it has a D3D8 twin (RL_API=9 / RL_API=8). rl::Gfx forwards each
// call to the real API (rl_gfx_d3d9.cpp / rl_gfx_d3d8.cpp) and records the call's inputs, so the
// app writes the ground truth it fed the runtime: resources and their bytes (vertex / index
// buffers, texture mips in the Remix canonical packed layout, shaders), and per draw call the
// vertex format, stream bindings, transforms, lights, material and render / stage / sampler
// state. The sidecar schema is schema/rl_app_sidecar.schema.json.
//
// Determinism: fixed 128x96 back buffer, fixed frame count, no clocks, no random seeds, no
// pointers or handles in the output; resource ids are allocation order.
//
// Output (directory = --out DIR, default "."): <app>.json, <app>.png, <app>.rgba (raw RGBA8,
// rows top-down), plus <app>.<dump>.{png,rgba} for extra render-target dumps.
#pragma once

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

#include "rl_json.h"
#include "rl_math.h"

#ifndef RL_API
#define RL_API 9
#endif

namespace rl {

static const int kWidth = 128;
static const int kHeight = 96;

enum Api { API_D3D8 = 8, API_D3D9 = 9 };

// Layout-compatible with D3DCOLORVALUE / D3DVECTOR / D3DLIGHT9 (== D3DLIGHT8) / D3DMATERIAL9
// (== D3DMATERIAL8) / D3DVIEWPORT9 (== D3DVIEWPORT8) / D3DVERTEXELEMENT9. The scene sources only
// include d3d9types.h for the enum names; the backends memcpy these into the API structs.
struct Color {
    float r, g, b, a;
};
struct Light {
    uint32_t Type;
    Color Diffuse, Specular, Ambient;
    Vec3 Position, Direction;
    float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
};
struct Material {
    Color Diffuse, Ambient, Specular, Emissive;
    float Power;
};
struct Viewport {
    uint32_t X, Y, Width, Height;
    float MinZ, MaxZ;
};
struct VertexElement {
    uint16_t Stream, Offset;
    uint8_t Type, Method, Usage, UsageIndex;
};

struct Caps {
    uint32_t vsVersion = 0, psVersion = 0; // D3DVS_VERSION / D3DPS_VERSION encodings
    uint32_t maxVertexBlendMatrices = 0, maxVertexBlendMatrixIndex = 0;
    uint32_t maxTextureBlendStages = 0, maxSimultaneousTextures = 0;
    uint32_t maxStreams = 0;
};

// Device-creation flags a scene can ask for.
enum : uint32_t { DEV_SOFTWARE_VP = 1 };

// A resource id is -1 for "none" (NULL); the back buffer / default depth are RT_BACKBUFFER /
// DS_DEFAULT in setRenderTarget / setDepthStencil.
static const int NONE = -1;
static const int RT_BACKBUFFER = -1;
static const int DS_DEFAULT = -1;

// ---- API backend (one per D3D version) --------------------------------------------------------
class Backend {
public:
    virtual ~Backend() {}
    virtual Api api() const = 0;
    virtual long init(void* hwnd, uint32_t devFlags) = 0;
    virtual std::string adapter() = 0;
    virtual Caps caps() = 0;
    virtual bool formatSupported(uint32_t format, uint32_t usage, uint32_t resourceType) = 0;
    virtual long beginScene() = 0;
    virtual long endScene() = 0;
    virtual long present() = 0;
    virtual long clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) = 0;
    virtual long setRenderState(uint32_t s, uint32_t v) = 0;
    virtual long setTextureStageState(uint32_t stage, uint32_t t, uint32_t v) = 0;
    virtual long setSamplerState(uint32_t sampler, uint32_t t, uint32_t v) = 0; // D3D9 D3DSAMP_* ids
    virtual long setTransform(uint32_t t, const Mat4& m) = 0;
    virtual long setLight(uint32_t i, const Light& l) = 0;
    virtual long lightEnable(uint32_t i, bool on) = 0;
    virtual long setMaterial(const Material& m) = 0;
    virtual long setViewport(const Viewport& v) = 0;
    virtual long createTexture(int id, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) = 0;
    // Copies `rows` rows of `rowBytes` (tightly packed source) into the locked level.
    virtual long writeTexture(int id, uint32_t level, const uint8_t* data, uint32_t rowBytes, uint32_t rows) = 0;
    virtual long updateTexture(int src, int dst) = 0;
    virtual long updateSurface(int src, uint32_t srcLevel, int dst, uint32_t dstLevel) = 0;
    virtual long createDepthStencil(int id, uint32_t w, uint32_t h, uint32_t fmt) = 0;
    virtual long setRenderTarget(int texId) = 0;
    virtual long setDepthStencil(int id) = 0;
    virtual long createVertexBuffer(int id, uint32_t size, uint32_t usage, uint32_t fvf, uint32_t pool) = 0;
    virtual long createIndexBuffer(int id, uint32_t size, uint32_t usage, uint32_t fmt, uint32_t pool) = 0;
    virtual long writeBuffer(int id, uint32_t offset, const void* data, uint32_t size, uint32_t lockFlags) = 0;
    virtual long setStreamSource(uint32_t stream, int id, uint32_t offset, uint32_t stride) = 0;
    virtual long setIndices(int id) = 0;
    virtual long setFVF(uint32_t fvf) = 0;
    virtual long createVertexDeclaration(int id, const std::vector<VertexElement>& elems) = 0;
    virtual long setVertexDeclaration(int id) = 0;
    virtual long createVertexShader(int id, const std::vector<uint32_t>& code, int declId) = 0;
    virtual long createPixelShader(int id, const std::vector<uint32_t>& code) = 0;
    virtual long setVertexShader(int id) = 0;
    virtual long setPixelShader(int id) = 0;
    virtual long setVSConstF(uint32_t start, const float* v, uint32_t count) = 0;
    virtual long setVSConstI(uint32_t start, const int* v, uint32_t count) = 0;
    virtual long setVSConstB(uint32_t start, const int* v, uint32_t count) = 0;
    virtual long setPSConstF(uint32_t start, const float* v, uint32_t count) = 0;
    virtual long setTexture(uint32_t stage, int id) = 0;
    virtual long drawPrimitive(uint32_t type, uint32_t startVertex, uint32_t primCount) = 0;
    virtual long drawIndexedPrimitive(uint32_t type, int baseVertex, uint32_t minIndex, uint32_t numVertices,
                                      uint32_t startIndex, uint32_t primCount) = 0;
    virtual long drawPrimitiveUP(uint32_t type, uint32_t primCount, const void* v, uint32_t stride) = 0;
    virtual long drawIndexedPrimitiveUP(uint32_t type, uint32_t minIndex, uint32_t numVertices, uint32_t primCount,
                                        const void* idx, uint32_t idxFmt, const void* v, uint32_t stride) = 0;
    // Reads the current back buffer (before Present) / a render-target texture's level 0 as RGBA8.
    virtual long readBackBuffer(std::vector<uint8_t>& rgba) = 0;
    virtual long readRenderTarget(int texId, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) = 0;
};

Backend* createBackend(); // rl_gfx_d3d9.cpp or rl_gfx_d3d8.cpp, chosen at link time

// ---- recording wrapper ---------------------------------------------------------------------------
class Recorder;

class Gfx {
public:
    Gfx(Backend* be, Recorder* rec);

    Api api() const { return m_api; }
    bool isD3D8() const { return m_api == API_D3D8; }
    const Caps& caps() const { return m_caps; }
    bool formatSupported(uint32_t format, uint32_t usage = 0, uint32_t resourceType = 3 /*D3DRTYPE_TEXTURE*/);

    // Semantic label copied onto following draws ("world", "sky", "hud", "shadow_volume", ...).
    // Pure metadata for the capture expectations of later packages; it has no API effect.
    void tag(const char* t);
    // Top-level scene annotation, e.g. the camera the scene used.
    void annotate(const std::string& key, JV value);
    // Expected pixel colour at (x, y) of the back-buffer dump, +- tol per channel. Checked by
    // tools/rl_app_check.py; only placed where the expected value follows from the inputs alone.
    void probe(int x, int y, uint8_t r, uint8_t g, uint8_t b, int tol, const char* what);
    // Extra render-target dump written after the last frame (<app>.<name>.png/.rgba).
    void dumpRenderTarget(int texId, const char* name);

    void clear(uint32_t flags, uint32_t color, float z = 1.0f, uint32_t stencil = 0);
    void setRenderState(uint32_t s, uint32_t v);
    void setTextureStageState(uint32_t stage, uint32_t t, uint32_t v);
    void setSamplerState(uint32_t sampler, uint32_t t, uint32_t v);
    void setTransform(uint32_t t, const Mat4& m);
    void setLight(uint32_t i, const Light& l);
    void lightEnable(uint32_t i, bool on);
    void setMaterial(const Material& m);
    void setViewport(const Viewport& v);

    int createTexture(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool);
    // `data` is level `level` tightly packed (row = blocksWide * bytesPerBlock bytes).
    void uploadTexture(int id, uint32_t level, const void* data);
    void updateTexture(int src, int dst);
    void updateSurface(int src, uint32_t srcLevel, int dst, uint32_t dstLevel);
    int createDepthStencil(uint32_t w, uint32_t h, uint32_t fmt);
    void setRenderTarget(int texId);
    void setDepthStencil(int id);

    int createVertexBuffer(uint32_t size, uint32_t usage, uint32_t fvf, uint32_t pool, const void* init = nullptr);
    int createIndexBuffer(uint32_t size, uint32_t usage, uint32_t fmt, uint32_t pool, const void* init = nullptr);
    void writeBuffer(int id, uint32_t offset, const void* data, uint32_t size, uint32_t lockFlags);
    void setStreamSource(uint32_t stream, int id, uint32_t offset, uint32_t stride);
    void setIndices(int id);
    void setFVF(uint32_t fvf);
    int createVertexDeclaration(const std::vector<VertexElement>& elems);
    void setVertexDeclaration(int id);
    int createVertexShader(const std::vector<uint32_t>& code, const std::string& listing, int declId);
    int createPixelShader(const std::vector<uint32_t>& code, const std::string& listing);
    void setVertexShader(int id);
    void setPixelShader(int id);
    void setVSConstF(uint32_t start, const float* v, uint32_t count);
    void setVSConstI(uint32_t start, const int* v, uint32_t count);
    void setVSConstB(uint32_t start, const int* v, uint32_t count);
    void setPSConstF(uint32_t start, const float* v, uint32_t count);
    void setTexture(uint32_t stage, int id);

    void drawPrimitive(uint32_t type, uint32_t startVertex, uint32_t primCount);
    void drawIndexedPrimitive(uint32_t type, int baseVertex, uint32_t minIndex, uint32_t numVertices, uint32_t startIndex,
                              uint32_t primCount);
    void drawPrimitiveUP(uint32_t type, uint32_t primCount, const void* v, uint32_t stride);
    void drawIndexedPrimitiveUP(uint32_t type, uint32_t minIndex, uint32_t numVertices, uint32_t primCount, const void* idx,
                                uint32_t idxFmt, const void* v, uint32_t stride);

    // Harness only.
    void beginFrame(int frame, bool recorded);
    void endFrame();
    Backend* backend() { return m_be; }
    Recorder* recorder() { return m_rec; }

private:
    void check(long hr, const char* what);
    Backend* m_be;
    Recorder* m_rec;
    Api m_api;
    Caps m_caps;
};

// ---- scene interface (one per app, scenes/<name>.cpp) --------------------------------------------
struct SceneInfo {
    const char* name;     // base name; the D3D8 build is named "d3d8_<name>"
    int frames;           // frames rendered; the back buffer is dumped on the last one
    bool recordAllFrames; // record every frame's draws (else only the last frame's)
    uint32_t devFlags;    // DEV_*
    const char* covers;   // one line: what the app covers (copied into the sidecar)
};
extern const SceneInfo kScene;
void sceneInit(Gfx& g);
void sceneFrame(Gfx& g, int frame);

// ---- helpers shared by the scenes ------------------------------------------------------------------
std::vector<VertexElement> fvfElements(uint32_t fvf, uint32_t* stride = nullptr);
uint32_t fvfStride(uint32_t fvf);
// Block size of a D3DFORMAT: bytes per block and block width/height (1 for non-block formats).
bool formatBlockInfo(uint32_t fmt, uint32_t& bytesPerBlock, uint32_t& blockW, uint32_t& blockH);
inline uint32_t f2dw(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
inline Color color(float r, float g, float b, float a = 1.0f) { return Color{r, g, b, a}; }
// Deterministic procedural texel generator (checker + gradient), A8R8G8B8 texels.
std::vector<uint32_t> makeCheckerARGB(uint32_t w, uint32_t h, uint32_t c0, uint32_t c1, uint32_t cell);

} // namespace rl
