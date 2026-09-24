// FUSE Relight RL-1.2: D3DStateModel, the classifier's view of one draw call
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.5, §2.4).
//
// Remix's D3D9Rtx reads DXVK's Direct3DState9 and D3D9CommonTexture objects directly. Relight's
// classifier reads this plain value type instead, so every rule is unit-testable on a synthetic
// state without DXVK or Wine. It is built from tap events (D3DStateTracker, classify_tap.hpp):
// onDraw's DrawCall + DrawState, plus what earlier events told the tracker (present parameters,
// texture descriptors, texture hashes, open occlusion queries).
//
// Values are raw D3D9 values (as in the tap). Matrices are row-major D3DMATRIX (m[row * 4 + col]),
// so Remix's `matrix[3][3]` is m[15] and `worldToView[3].xyz()` is m[12..14].
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>

#include <array>
#include <cstdint>

namespace fuse::relight::scene {

using Hash64 = std::uint64_t;
/// Remix kEmptyHash: "no hash" for textures and images.
inline constexpr Hash64 kEmptyHash = 0;

// ---- D3D9 values the rules read (d3d9types.h) ------------------------------------------------------
namespace d3d {
inline constexpr std::uint32_t RS_ZENABLE = 7, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_ALPHABLENDENABLE = 27,
                               RS_STENCILENABLE = 52, RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54, RS_STENCILPASS = 55,
                               RS_STENCILFUNC = 56, RS_POINTSIZE = 154, RS_VERTEXBLEND = 151,
                               RS_INDEXEDVERTEXBLENDENABLE = 167, RS_COLORWRITEENABLE = 168, RS_ADAPTIVETESS_Y = 181;
inline constexpr std::uint32_t ZB_TRUE = 1;
inline constexpr std::uint32_t CMP_NOTEQUAL = 6, CMP_ALWAYS = 8;
inline constexpr std::uint32_t STENCILOP_KEEP = 1, STENCILOP_INCRSAT = 4, STENCILOP_DECRSAT = 5, STENCILOP_INCR = 7,
                               STENCILOP_DECR = 8;
inline constexpr std::uint32_t COLORWRITE_RGB = 0x7; ///< RED | GREEN | BLUE
inline constexpr std::uint32_t VBF_DISABLE = 0, VBF_0WEIGHTS = 256;

inline constexpr std::uint32_t TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5,
                               TSS_ALPHAARG2 = 6, TSS_TEXCOORDINDEX = 11, TSS_COLORARG0 = 26, TSS_ALPHAARG0 = 27,
                               TSS_RESULTARG = 28;
inline constexpr std::uint32_t TOP_DISABLE = 1, TOP_SELECTARG1 = 2, TOP_SELECTARG2 = 3, TOP_MODULATE = 4,
                               TOP_BLENDTEXTUREALPHA = 13, TOP_BLENDTEXTUREALPHAPM = 15, TOP_BLENDCURRENTALPHA = 16,
                               TOP_PREMODULATE = 17, TOP_BUMPENVMAP = 22, TOP_BUMPENVMAPLUMINANCE = 23,
                               TOP_MULTIPLYADD = 25, TOP_LERP = 26;
inline constexpr std::uint32_t TA_SELECTMASK = 0xf, TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3,
                               TA_TEMP = 5;

inline constexpr std::uint32_t PT_POINTLIST = 1, PT_LINELIST = 2, PT_LINESTRIP = 3, PT_TRIANGLELIST = 4,
                               PT_TRIANGLESTRIP = 5, PT_TRIANGLEFAN = 6;
inline constexpr std::uint32_t RTYPE_SURFACE = 1, RTYPE_TEXTURE = 3, RTYPE_VOLUMETEXTURE = 4, RTYPE_CUBETEXTURE = 5;
inline constexpr std::uint32_t USAGE_RENDERTARGET = 0x1, USAGE_DEPTHSTENCIL = 0x2;
inline constexpr std::uint32_t QUERYTYPE_OCCLUSION = 9;
inline constexpr std::uint32_t DECLUSAGE_POSITION = 0, DECLUSAGE_BLENDWEIGHT = 1, DECLUSAGE_BLENDINDICES = 2,
                               DECLUSAGE_POSITIONT = 9;
inline constexpr std::uint32_t MULTISAMPLE_NONMASKABLE = 1, MULTISAMPLE_2_SAMPLES = 2;
constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8) |
           (std::uint32_t(std::uint8_t(c)) << 16) | (std::uint32_t(std::uint8_t(d)) << 24);
}
inline constexpr std::uint32_t FMT_ATOC = fourcc('A', 'T', 'O', 'C'), FMT_A2M1 = fourcc('A', '2', 'M', '1');
} // namespace d3d

/// Number of D3DTEXTURESTAGESTATETYPE slots (indexed by the D3D value, 1..32; slot 0 unused).
inline constexpr std::uint32_t kTextureStageStateSlots = 33;
/// Remix LegacyMaterialData::kMaxSupportedTextures.
inline constexpr std::uint32_t kMaxSupportedTextures = 2;
/// D3DDP_MAXTEXCOORD.
inline constexpr std::uint32_t kMaxTexcoords = 8;
/// dxvk-remix FixedFunctionMask (src/d3d9/d3d9_util.h@0867d3c): the sampler mask Remix's DXVK fork
/// reports for the fixed-function pixel shader, whatever the stage states: stages 0..6 (7 bits, as
/// upstream). isRenderingUI's UI-texture test uses it.
inline constexpr std::uint32_t kFixedFunctionPsSamplerMask = 0b1111111u;

/// What the classifier knows about one texture: the tap's TextureDesc plus the hashes Remix keeps on
/// the DxvkImage (produced by RL-1.4 in the live pipeline, or supplied by a test / replay).
struct TextureRecord {
    tap::ResourceId id = tap::kNoResource; ///< kNoResource: slot empty
    std::uint32_t type = 0;                ///< D3DRESOURCETYPE
    std::uint32_t width = 0, height = 0, depth = 1;
    std::uint32_t mipLevels = 1, arraySize = 1;
    std::uint32_t format = 0; ///< D3DFORMAT
    std::uint32_t usage = 0, pool = 0, multiSample = 0;
    bool isBackBuffer = false;
    bool isAttachmentOnly = false;
    bool hasImage = true; ///< D3D9CommonTexture::GetImage() != nullptr
    /// DxvkImage::getHash(): the colour-texture hash (texture lists, material hash).
    Hash64 imageHash = kEmptyHash;
    /// DxvkImage::getDescriptorHash(): set for render-target textures only
    /// (rtx.raytracedRenderTargetTextures).
    Hash64 descriptorHash = kEmptyHash;

    bool valid() const { return id != tap::kNoResource; }
};

/// The state of one draw call as Remix's D3D9Rtx sees it.
struct D3DStateModel {
    // ---- the draw call (D3D9Rtx::DrawContext) --------------------------------------------------------
    std::uint32_t primitiveType = d3d::PT_TRIANGLELIST;
    std::uint32_t primitiveCount = 0;
    bool indexed = false;

    // ---- device ----------------------------------------------------------------------------------------
    std::uint32_t backBufferWidth = 0, backBufferHeight = 0; ///< m_activePresentParams
    bool d3d8 = false;       ///< DXVK: alpha to coverage is never on for D3D8
    bool amdVendor = false;  ///< DXVK: the AMD alpha-to-coverage hack instead of NVIDIA's
    /// DXVK_RESOLUTION_WIDTH / _HEIGHT set: Remix skips the primary render-target check.
    bool resolutionOverride = false;
    /// D3D9Rtx::m_activeOcclusionQueries (occlusion queries begun and not yet ended).
    std::int32_t activeOcclusionQueries = 0;

    // ---- Direct3DState9 --------------------------------------------------------------------------------
    std::array<std::uint32_t, tap::kRenderStateCount> renderStates{};
    /// [stage][D3DTEXTURESTAGESTATETYPE]; slot 0 unused.
    std::array<std::array<std::uint32_t, kTextureStageStateSlots>, tap::kTextureStageCount> textureStages{};
    /// Bound textures by tap sampler slot (tap::samplerSlot: 0..15 PS, 16 DMAP, 17..20 VS).
    std::array<TextureRecord, tap::kSamplerSlotCount> textures{};
    TextureRecord renderTarget0; ///< invalid(): no colour render target bound
    /// D3D9ShaderMasks::samplerMask of the bound pixel shader (fixed function:
    /// kFixedFunctionPsSamplerMask) and vertex shader (fixed function: 0), by tap sampler slot.
    std::uint32_t psSamplerMask = kFixedFunctionPsSamplerMask, vsSamplerMask = 0;
    bool usesVertexShader = false; ///< UseProgrammableVS()
    bool usesPixelShader = false;  ///< UseProgrammablePS()
    bool hasPositionT = false;     ///< D3D9VertexDeclFlag::HasPositionT
    bool hasBlendWeight0 = false;  ///< BLENDWEIGHT[0] in the declaration (skinning input)
    bool hasBlendIndices = false;  ///< D3D9VertexDeclFlag::HasBlendIndices
    std::array<float, 16> view{}, projection{}, world{};
    tap::Viewport viewport;

    D3DStateModel();

    std::uint32_t rs(std::uint32_t state) const { return state < renderStates.size() ? renderStates[state] : 0; }
    std::uint32_t tss(std::uint32_t stage, std::uint32_t type) const {
        return stage < textureStages.size() && type < kTextureStageStateSlots ? textureStages[stage][type] : 0;
    }
    /// DXVK m_activeTextures: bit per sampler slot with a texture bound.
    std::uint32_t activeTextureMask() const;
    /// DXVK D3D9DeviceEx::IsAlphaTestEnabled(): ALPHATESTENABLE without alpha to coverage.
    bool isAlphaTestEnabled() const;
    /// Remix D3D9Rtx::processSkinning precondition (futureSkinningData.valid()).
    bool hasSkinning() const;
};

/// D3D9's default render states (DXVK D3D9DeviceEx::SetDefaultRenderStates, the values the rules
/// read) and texture stage states (stage 0 MODULATE, others DISABLE, TEXCOORDINDEX = stage).
void setD3D9DefaultStates(D3DStateModel& model);

/// Identity row-major matrix.
std::array<float, 16> identityMatrix();

// ---- shader texture usage ----------------------------------------------------------------

/// Sampler slots a D3D9 shader samples (DXVK shader analysis GetSamplerMask), from its bytecode:
/// SM2+ `dcl` of sampler registers (vertex samplers map to slots 17..20); ps_1_x: t0..t3
/// (t0..t5 for ps_1_4); vs_1/vs_2: none. `tokens` null: every slot (unknown bytecode).
std::uint32_t shaderSamplerMask(const std::uint32_t* tokens, std::uint32_t byteSize);

// ---- formats ---------------------------------------------------------------------------------------------

/// Remix RtxTextureFormatCompatibilityCategory (rtx_resources.h).
enum class FormatCompatibilityCategory : std::uint32_t {
    Color_Format_8_Bits,
    Color_Format_16_Bits,
    Color_Format_32_Bits,
    Color_Format_64_Bits,
    Color_Format_128_Bits,
    Color_Format_256_Bits,
    InvalidFormatCompatibilityCategory,
};

/// Remix Resources::getFormatCompatibilityCategory(VkFormat).
FormatCompatibilityCategory formatCompatibilityCategory(std::uint32_t vkFormat);

/// The VkFormat DXVK's render-target image view of a D3DFORMAT has (D3D9 format table, as
/// Relight's hash module maps it); 0 (VK_FORMAT_UNDEFINED) when unmapped.
std::uint32_t renderTargetVkFormat(std::uint32_t d3dFormat);

} // namespace fuse::relight::scene
