// FUSE Relight RL-1.1: the tap seam (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.4).
//
// IRelightTap is what the vendored, patched DXVK d3d9 front end (Engine/lib/dxvk, patches
// RL-1.1-NN in PATCHES.md) reports to FUSE Relight. The d3d9 device holds a per-device dispatcher
// (Source/FUSE/Relight/tap/dxvk/fuse_tap_dxvk.cpp) that owns one IRelightTap. With the tap off
// (relight.tap.mode = off, or FUSE_RELIGHT=0) the dispatcher does not exist and every patched hook
// is a single null-pointer check.
//
// Contract:
// - Every event is delivered on the thread that made the D3D9 call, with the device lock held
//   (except onQueryBegin/onQueryEnd, which D3D9 issues without the device lock). Implementations
//   copy what they need and return; pointers in the event structs are valid for the call only.
// - Values are raw D3D9 values (D3DFORMAT, D3DPOOL, D3DUSAGE_*, D3DRS_*, ...) as uint32_t, so this
//   header needs no Windows or D3D headers and consumers build and unit-test on any platform.
// - Resource ids are assigned by the dispatcher per kind (textures, buffers, shaders) in creation
//   order, starting at 1, and never reused within one device. 0 means "none".
// - D3D8 applications reach the tap through DXVK's d3d8 -> d3d9 translation: events describe the
//   translated D3D9 calls (DeviceEvent::d3d8 is set).
//
// Plain C++17, no dependencies: it is compiled into the DXVK d3d9.dll (C++17) and into Relight's
// CPU-side modules and tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fuse::relight::tap {

/// Bumped when an event struct or IRelightTap changes incompatibly.
/// 2: TextureCopy carries the UpdateSurface extent and destination point.
/// 3: DrawState carries the light / clip-plane change counters and the render-target alpha-swizzle mask.
inline constexpr std::uint32_t kTapInterfaceVersion = 3;

using ResourceId = std::uint32_t;
inline constexpr ResourceId kNoResource = 0;

// ---- limits (D3D9 caps as DXVK implements them) ------------------------------------------------
inline constexpr std::uint32_t kRenderStateCount = 256;       ///< indexed by D3DRENDERSTATETYPE
inline constexpr std::uint32_t kTextureStageCount = 8;
inline constexpr std::uint32_t kTextureStageStateCount = 33;  ///< indexed by D3DTEXTURESTAGESTATETYPE
inline constexpr std::uint32_t kSamplerSlotCount = 21;        ///< see samplerSlot()
inline constexpr std::uint32_t kSamplerStateCount = 14;       ///< indexed by D3DSAMPLERSTATETYPE
inline constexpr std::uint32_t kStreamCount = 16;
inline constexpr std::uint32_t kMaxVertexElements = 64;
inline constexpr std::uint32_t kRenderTargetCount = 4;
inline constexpr std::uint32_t kClipPlaneCount = 6;
/// Transform slots in DrawState::transforms: DXVK's layout, 0 = VIEW, 1 = PROJECTION,
/// 2..9 = TEXTURE0..7, 10 + n = WORLDMATRIX(n) (n = 0 is D3DTS_WORLD), n < 256.
inline constexpr std::uint32_t kTransformView = 0;
inline constexpr std::uint32_t kTransformProjection = 1;
inline constexpr std::uint32_t kTransformTexture0 = 2;
inline constexpr std::uint32_t kTransformWorld0 = 10;
inline constexpr std::uint32_t kTransformCount = 10 + 256;

/// Sampler slot of a D3D9 sampler index: 0..15 pixel samplers, 16 = D3DDMAPSAMPLER (256),
/// 17..20 = D3DVERTEXTEXTURESAMPLER0..3 (257..260). Returns kSamplerSlotCount when invalid.
constexpr std::uint32_t samplerSlot(std::uint32_t d3dSampler) {
    return d3dSampler < 16                              ? d3dSampler
           : (d3dSampler >= 256 && d3dSampler <= 260) ? 16 + (d3dSampler - 256)
                                                        : kSamplerSlotCount;
}
/// Inverse of samplerSlot().
constexpr std::uint32_t samplerFromSlot(std::uint32_t slot) { return slot < 16 ? slot : 256 + (slot - 16); }

// ---- device --------------------------------------------------------------------------------------
struct PresentParameters {
    std::uint32_t backBufferWidth = 0;
    std::uint32_t backBufferHeight = 0;
    std::uint32_t backBufferFormat = 0; ///< D3DFORMAT
    std::uint32_t backBufferCount = 0;
    std::uint32_t multiSampleType = 0;
    std::uint32_t multiSampleQuality = 0;
    std::uint32_t swapEffect = 0;
    std::uint64_t deviceWindow = 0; ///< HWND value
    bool windowed = false;
    bool enableAutoDepthStencil = false;
    std::uint32_t autoDepthStencilFormat = 0;
    std::uint32_t flags = 0;
    std::uint32_t fullScreenRefreshRate = 0;
    std::uint32_t presentationInterval = 0;
};

/// Vulkan objects DXVK runs on. Handles are the raw VkInstance / VkPhysicalDevice / VkDevice /
/// VkQueue values. `imported` is true when FUSE created the instance and device and DXVK imported
/// them (plan AD-2; relight.device.import), false when DXVK created its own.
struct VulkanDevice {
    std::uint64_t instance = 0;
    std::uint64_t physicalDevice = 0;
    std::uint64_t device = 0;
    std::uint64_t queue = 0;
    std::uint32_t queueFamily = 0;
    bool imported = false;
};

struct DeviceEvent {
    std::uint32_t adapter = 0;
    std::uint32_t deviceType = 0;    ///< D3DDEVTYPE
    std::uint64_t focusWindow = 0;   ///< HWND value
    std::uint32_t behaviorFlags = 0; ///< D3DCREATE_*
    bool extended = false;           ///< IDirect3DDevice9Ex
    bool d3d8 = false;               ///< created through d3d8.dll
    PresentParameters present;
    ResourceId backBuffer = kNoResource;       ///< implicit swap chain back buffer 0
    ResourceId autoDepthStencil = kNoResource; ///< kNoResource without EnableAutoDepthStencil
    VulkanDevice vulkan;
};

// ---- textures ------------------------------------------------------------------------------------
struct TextureDesc {
    ResourceId id = kNoResource;
    std::uint32_t type = 0; ///< D3DRESOURCETYPE: 1 surface, 3 texture, 4 volume texture, 5 cube texture
    std::uint32_t width = 0, height = 0, depth = 0;
    std::uint32_t mipLevels = 0; ///< after DXVK's normalisation (0 in CreateTexture = full chain)
    std::uint32_t arraySize = 0; ///< 6 for cube textures
    std::uint32_t format = 0;    ///< D3DFORMAT
    std::uint32_t usage = 0;     ///< D3DUSAGE_*
    std::uint32_t pool = 0;      ///< D3DPOOL
    std::uint32_t multiSample = 0;
    bool isBackBuffer = false;
    bool isAttachmentOnly = false; ///< render target / depth surface without a texture
    std::uint64_t vkImage = 0;     ///< 0 for textures without a GPU image (e.g. D3DPOOL_SYSTEMMEM)
};

struct Box {
    std::uint32_t left = 0, top = 0, right = 0, bottom = 0, front = 0, back = 0;
};

/// Data written through LockRect/LockBox, delivered when the subresource is unlocked. `data`
/// points at the start of the locked region; rows are `rowPitch` apart, slices `slicePitch`. For a
/// full lock the layout is Remix's canonical packed layout for block formats (plan §4.1.3): `rows`
/// block rows of `rowPitch` = align(bytesPerBlock * blocksWide, 4) bytes.
struct TextureUpload {
    ResourceId texture = kNoResource;
    std::uint32_t face = 0, level = 0;
    std::uint32_t width = 0, height = 0, depth = 0; ///< extent of the level
    const void* data = nullptr;
    std::uint32_t rowPitch = 0, slicePitch = 0, rows = 0;
    std::uint32_t lockFlags = 0; ///< D3DLOCK_* as the application passed them
    bool fullUpdate = false;     ///< no rect/box: the whole subresource was locked
    Box box;                     ///< locked region (the whole level when fullUpdate)
};

enum class CopyMethod : std::uint32_t { UpdateTexture = 0, UpdateSurface = 1 };

/// UpdateTexture (all levels and faces the call copies) or UpdateSurface (one subresource) from a
/// D3DPOOL_SYSTEMMEM source into a D3DPOOL_DEFAULT destination.
struct TextureCopy {
    CopyMethod method = CopyMethod::UpdateTexture;
    ResourceId source = kNoResource, destination = kNoResource;
    std::uint32_t sourceFace = 0, sourceLevel = 0; ///< UpdateSurface only
    std::uint32_t destFace = 0, destLevel = 0;     ///< UpdateSurface only
    bool hasSourceRect = false;                    ///< UpdateSurface with a source rect / dest point
    /// UpdateSurface only: the copied extent in texels (the source rect, or the whole source level
    /// without one) and where it lands in the destination level (the dest point, or 0,0). 0 x 0
    /// for UpdateTexture, and when a producer does not know the extent.
    std::uint32_t width = 0, height = 0;
    std::uint32_t destX = 0, destY = 0;
};

struct TextureWriteLock {
    ResourceId texture = kNoResource;
    std::uint32_t face = 0, level = 0;
    std::uint32_t lockFlags = 0;
};

struct ImageDestroy {
    ResourceId texture = kNoResource;
    std::uint64_t vkImage = 0;
};

// ---- buffers -------------------------------------------------------------------------------------
enum class BufferKind : std::uint32_t { Vertex = 0, Index = 1 };

struct BufferDesc {
    ResourceId id = kNoResource;
    BufferKind kind = BufferKind::Vertex;
    std::uint32_t size = 0;
    std::uint32_t usage = 0; ///< D3DUSAGE_*
    std::uint32_t pool = 0;  ///< D3DPOOL
    std::uint32_t fvf = 0;   ///< vertex buffers
    std::uint32_t format = 0; ///< index buffers: D3DFMT_INDEX16 (101) / D3DFMT_INDEX32 (102)
};

/// One Lock/Unlock write, delivered when the buffer's last lock is released. `data` points at
/// byte `offset` of the buffer's CPU mapping (`base` is byte 0, `bufferSize` bytes long).
struct BufferWrite {
    ResourceId buffer = kNoResource;
    std::uint32_t offset = 0, size = 0; ///< resolved range (SizeToLock 0 = to the end)
    std::uint32_t lockFlags = 0;        ///< D3DLOCK_* as the application passed them
    const void* data = nullptr;
    const void* base = nullptr;
    std::uint32_t bufferSize = 0;
};

// ---- shaders and vertex formats -----------------------------------------------------------------
struct ShaderRef {
    ResourceId id = kNoResource;           ///< kNoResource: fixed function
    const std::uint32_t* tokens = nullptr; ///< D3D9 bytecode (as GetFunction returns it)
    std::uint32_t byteSize = 0;
    std::uint32_t version = 0;             ///< first token (D3DVS_VERSION / D3DPS_VERSION)
};

/// D3DVERTEXELEMENT9.
struct VertexElement {
    std::uint16_t stream = 0, offset = 0;
    std::uint8_t type = 0, method = 0, usage = 0, usageIndex = 0;
};

struct StreamBinding {
    ResourceId buffer = kNoResource;
    std::uint32_t offset = 0, stride = 0;
    std::uint32_t frequency = 0; ///< SetStreamSourceFreq value
    /// The buffer's CPU mapping (byte 0) and size, when it has one; nullptr otherwise.
    const void* base = nullptr;
    std::uint32_t bufferSize = 0;
};

struct IndexBinding {
    ResourceId buffer = kNoResource;
    std::uint32_t format = 0; ///< D3DFMT_INDEX16 / D3DFMT_INDEX32
    const void* base = nullptr;
    std::uint32_t bufferSize = 0;
};

// ---- fixed-function state ------------------------------------------------------------------------
struct Color4 {
    float r = 0, g = 0, b = 0, a = 0;
};
struct Vec3 {
    float x = 0, y = 0, z = 0;
};

/// D3DLIGHT9 plus its slot and enable bit.
struct Light {
    std::uint32_t index = 0;
    bool enabled = false;
    std::uint32_t type = 0; ///< D3DLIGHTTYPE
    Color4 diffuse, specular, ambient;
    Vec3 position, direction;
    float range = 0, falloff = 0, attenuation0 = 0, attenuation1 = 0, attenuation2 = 0, theta = 0, phi = 0;
};

/// D3DMATERIAL9.
struct Material {
    Color4 diffuse, ambient, specular, emissive;
    float power = 0;
};

/// D3DVIEWPORT9.
struct Viewport {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    float minZ = 0, maxZ = 1;
};

struct Rect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};

// ---- draws ---------------------------------------------------------------------------------------
enum class DrawCallType : std::uint32_t {
    DrawPrimitive = 0,
    DrawIndexedPrimitive = 1,
    DrawPrimitiveUP = 2,
    DrawIndexedPrimitiveUP = 3,
};

/// The draw call's own arguments. Counts follow the D3D9 call; vertexCount / indexCount are
/// derived from the primitive type and count.
struct DrawCall {
    DrawCallType call = DrawCallType::DrawPrimitive;
    std::uint32_t primitiveType = 0; ///< D3DPRIMITIVETYPE
    std::uint32_t primitiveCount = 0;
    std::uint32_t startVertex = 0;   ///< DrawPrimitive
    std::uint32_t vertexCount = 0;   ///< non-indexed draws
    std::int32_t baseVertex = 0;     ///< DrawIndexedPrimitive
    std::uint32_t minIndex = 0, numVertices = 0;
    std::uint32_t startIndex = 0, indexCount = 0;
    std::uint32_t instanceCount = 1;
    /// DrawPrimitiveUP / DrawIndexedPrimitiveUP: the application's pointers. The vertex range is
    /// vertexCount (non-indexed) or minIndex + numVertices (indexed) vertices of vertexStride bytes.
    const void* upVertexData = nullptr;
    std::uint32_t upVertexStride = 0, upVertexBytes = 0;
    const void* upIndexData = nullptr;
    std::uint32_t upIndexFormat = 0, upIndexBytes = 0;
};

/// Snapshot of the device state at a draw: DXVK's Direct3DState9 viewed in place. The arrays are
/// DXVK's own storage (valid during onDraw only); layouts are documented per field.
struct DrawState {
    const std::uint32_t* renderStates = nullptr;                   ///< [kRenderStateCount]
    /// [kTextureStageCount][32]; element i is D3DTEXTURESTAGESTATETYPE (i + 1) (DXVK's layout).
    const std::uint32_t (*textureStageStates)[32] = nullptr;
    const std::uint32_t (*samplerStates)[kSamplerStateCount] = nullptr; ///< [kSamplerSlotCount]
    ResourceId textures[kSamplerSlotCount] = {};
    StreamBinding streams[kStreamCount];
    IndexBinding indices;
    VertexElement elements[kMaxVertexElements];
    std::uint32_t elementCount = 0;
    std::uint32_t fvf = 0; ///< 0 when a vertex declaration (not an FVF) is bound
    ShaderRef vertexShader, pixelShader;
    const float (*transforms)[16] = nullptr; ///< [kTransformCount], row-major D3DMATRIX
    const Light* lights = nullptr;           ///< every light slot ever set
    std::uint32_t lightCount = 0;
    Material material;
    Viewport viewport;
    Rect scissor;
    const float (*clipPlanes)[4] = nullptr; ///< [kClipPlaneCount]
    ResourceId renderTargets[kRenderTargetCount] = {};
    ResourceId depthStencil = kNoResource;
    const float (*vsConstF)[4] = nullptr;
    std::uint32_t vsConstFCount = 0;
    const std::int32_t (*vsConstI)[4] = nullptr;
    std::uint32_t vsConstICount = 0;
    const std::uint32_t* vsConstB = nullptr; ///< bit i of word i / 32
    std::uint32_t vsConstBCount = 0;
    const float (*psConstF)[4] = nullptr;
    std::uint32_t psConstFCount = 0;
    const std::int32_t (*psConstI)[4] = nullptr;
    std::uint32_t psConstICount = 0;
    const std::uint32_t* psConstB = nullptr;
    std::uint32_t psConstBCount = 0;
    bool softwareVertexProcessing = false;

    /// Change counters (interface 3): Remix's D3D9RtxFlag::DirtyLights / DirtyClipPlanes as counters. A
    /// counter advances on every event that sets the flag upstream; a consumer that keeps the value it last
    /// acted on sees "changed since" as a different value (the flag set), and acts exactly where upstream
    /// clears the flag. 0 = the producer does not track changes (consumers compare state instead).
    /// lightsVersion: SetLight on an enabled light, LightEnable that flips a light's enable bit, device
    ///   creation and reset (ResetState).
    /// clipPlanesVersion: SetClipPlane changing an enabled plane, SetRenderState(D3DRS_CLIPPLANEENABLE),
    ///   device creation and reset.
    std::uint32_t lightsVersion = 0;
    std::uint32_t clipPlanesVersion = 0;
    /// Bit i: render target i's image view reads alpha as ONE (DXVK's hasAlphaSwizzle, Remix's
    /// m_alphaSwizzleRTs). Meaningful when hasAlphaSwizzleMask; otherwise consumers derive it from the format.
    std::uint32_t alphaSwizzleRenderTargets = 0;
    bool hasAlphaSwizzleMask = false;
};

/// What DXVK does with the draw (plan §2.3). RayTracedPreserveRaster keeps the raster draw while
/// Relight also consumes it; Ignore skips DXVK's raster draw (the call still returns D3D_OK).
enum class DrawDecision : std::uint32_t { Raster = 0, Ignore = 1, RayTracedPreserveRaster = 2 };

// ---- other events --------------------------------------------------------------------------------
struct ClearEvent {
    std::uint32_t rectCount = 0;
    const Rect* rects = nullptr; ///< D3DRECT x1 y1 x2 y2, or nullptr
    std::uint32_t flags = 0;     ///< D3DCLEAR_*
    std::uint32_t color = 0;     ///< D3DCOLOR (ARGB)
    float z = 0;
    std::uint32_t stencil = 0;
    ResourceId renderTargets[kRenderTargetCount] = {};
    ResourceId depthStencil = kNoResource;
};

struct SetRenderTargetEvent {
    std::uint32_t index = 0;
    ResourceId texture = kNoResource;
};

struct QueryEvent {
    std::uint32_t type = 0;  ///< D3DQUERYTYPE
    std::uint64_t query = 0; ///< identity of the query object (stable while it lives)
};

struct FrameEvent {
    std::uint64_t frame = 0; ///< presents so far (0 before the first Present)
    ResourceId backBuffer = kNoResource;
    std::uint64_t backBufferVkImage = 0;
    std::uint32_t width = 0, height = 0, format = 0;
};

/// A vertex shader's SPIR-V, offered for substitution (vertex capture, plan §2.5).
struct ShaderModule {
    ResourceId shader = kNoResource;
    const std::uint32_t* spirv = nullptr;
    std::size_t wordCount = 0;
};

// ---- the interface -------------------------------------------------------------------------------
/// Every method has a no-op default, so a tap overrides only what it consumes.
class IRelightTap {
public:
    virtual ~IRelightTap() = default;

    virtual void onDeviceCreate(const DeviceEvent&) {}
    virtual void onDeviceReset(const DeviceEvent&) {}
    virtual void onDeviceDestroy() {}

    virtual void onTextureCreate(const TextureDesc&) {}
    /// Subresource data written by the application (every level; hashing uses level 0).
    virtual void onTextureUpload(const TextureUpload&) {}
    virtual void onTextureCopy(const TextureCopy&) {}
    /// A write lock was taken (rtx.recomputeTextureHashOnWrite).
    virtual void onTextureWriteLock(const TextureWriteLock&) {}
    virtual void onImageDestroy(const ImageDestroy&) {}

    virtual void onBufferCreate(const BufferDesc&) {}
    virtual void onBufferWrite(const BufferWrite&) {}
    virtual void onBufferDestroy(ResourceId) {}

    virtual DrawDecision onDraw(const DrawCall&, const DrawState&) { return DrawDecision::Raster; }
    /// Offer a vertex shader's SPIR-V for replacement; return true with `replacement` filled to
    /// substitute it. Reserved for RL-1.6 (vertex capture): the RL-1.1 patch set does not call it.
    virtual bool substituteVertexShader(const ShaderModule&, std::vector<std::uint32_t>& /*replacement*/) {
        return false;
    }

    virtual void onQueryBegin(const QueryEvent&) {}
    virtual void onQueryEnd(const QueryEvent&) {}
    virtual void onClear(const ClearEvent&) {}
    virtual void onSetRenderTarget(const SetRenderTargetEvent&) {}
    /// Where Relight renders and composites (plan §2.3). Until the classifier (RL-1.2) finds the
    /// first UI draw, the dispatcher raises it once per frame just before onPresent.
    virtual void onInjectPoint(const FrameEvent&) {}
    virtual void onPresent(const FrameEvent&) {}
};

} // namespace fuse::relight::tap
