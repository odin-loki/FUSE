// FUSE Relight RL-1.2: D3DStateModel helpers.
//
// isAlphaTestEnabled follows the vendored DXVK's D3D9DeviceEx::UpdateAlphaToCoverangeAndAlphaTest
// (Engine/lib/dxvk/src/d3d9/d3d9_device.cpp, zlib licence).
// formatCompatibilityCategory is Remix Resources::getFormatCompatibilityCategory
// (dxvk-remix src/dxvk/rtx_render/rtx_resources.cpp@0867d3c, MIT), over the Vulkan enum values.
#include <fuse/relight/scene/classify/d3d_state_model.hpp>

#include <fuse/relight/hash/texture_hash.hpp>

namespace fuse::relight::scene {

D3DStateModel::D3DStateModel() {
    view = identityMatrix();
    projection = identityMatrix();
    world = identityMatrix();
    setD3D9DefaultStates(*this);
}

std::array<float, 16> identityMatrix() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

void setD3D9DefaultStates(D3DStateModel& m) {
    m.renderStates.fill(0);
    m.renderStates[d3d::RS_ZENABLE] = d3d::ZB_TRUE; // with an auto depth-stencil surface
    m.renderStates[d3d::RS_ZWRITEENABLE] = 1;
    m.renderStates[d3d::RS_ALPHATESTENABLE] = 0;
    m.renderStates[d3d::RS_ALPHABLENDENABLE] = 0;
    m.renderStates[d3d::RS_STENCILENABLE] = 0;
    m.renderStates[d3d::RS_STENCILFAIL] = d3d::STENCILOP_KEEP;
    m.renderStates[d3d::RS_STENCILZFAIL] = d3d::STENCILOP_KEEP;
    m.renderStates[d3d::RS_STENCILPASS] = d3d::STENCILOP_KEEP;
    m.renderStates[d3d::RS_STENCILFUNC] = d3d::CMP_ALWAYS;
    m.renderStates[d3d::RS_COLORWRITEENABLE] = 0xf;
    m.renderStates[d3d::RS_VERTEXBLEND] = d3d::VBF_DISABLE;
    m.renderStates[d3d::RS_POINTSIZE] = 0x3f800000u; // 1.0f
    for (std::uint32_t stage = 0; stage < tap::kTextureStageCount; ++stage) {
        auto& s = m.textureStages[stage];
        s.fill(0);
        s[d3d::TSS_COLOROP] = stage == 0 ? d3d::TOP_MODULATE : d3d::TOP_DISABLE;
        s[d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
        s[d3d::TSS_COLORARG2] = d3d::TA_CURRENT;
        s[d3d::TSS_ALPHAOP] = stage == 0 ? d3d::TOP_SELECTARG1 : d3d::TOP_DISABLE;
        s[d3d::TSS_ALPHAARG1] = d3d::TA_TEXTURE;
        s[d3d::TSS_ALPHAARG2] = d3d::TA_CURRENT;
        s[d3d::TSS_TEXCOORDINDEX] = stage;
        s[d3d::TSS_COLORARG0] = d3d::TA_CURRENT;
        s[d3d::TSS_ALPHAARG0] = d3d::TA_CURRENT;
        s[d3d::TSS_RESULTARG] = d3d::TA_CURRENT;
    }
}

std::uint32_t D3DStateModel::activeTextureMask() const {
    std::uint32_t mask = 0;
    for (std::uint32_t slot = 0; slot < textures.size(); ++slot) {
        if (textures[slot].valid()) {
            mask |= 1u << slot;
        }
    }
    return mask;
}

bool D3DStateModel::isAlphaTestEnabled() const {
    const bool alphaTest = rs(d3d::RS_ALPHATESTENABLE) != 0;
    bool alphaToCoverage = false;
    if (!d3d8) {
        const bool amdAtoc = rs(d3d::RS_POINTSIZE) == d3d::FMT_A2M1;
        const bool nvAtoc = rs(d3d::RS_ADAPTIVETESS_Y) == d3d::FMT_ATOC && alphaTest;
        const bool multisampled = renderTarget0.valid() && renderTarget0.multiSample >= d3d::MULTISAMPLE_2_SAMPLES;
        alphaToCoverage = (amdVendor ? amdAtoc : nvAtoc) && multisampled;
    }
    return alphaTest && !alphaToCoverage;
}

bool D3DStateModel::hasSkinning() const {
    if (usesVertexShader) {
        return false;
    }
    const std::uint32_t vertexBlend = rs(d3d::RS_VERTEXBLEND);
    if (vertexBlend == d3d::VBF_DISABLE) {
        return false;
    }
    if (vertexBlend != d3d::VBF_0WEIGHTS) {
        return hasBlendWeight0;
    }
    return hasBlendIndices && rs(d3d::RS_INDEXEDVERTEXBLENDENABLE) != 0;
}

std::uint32_t shaderSamplerMask(const std::uint32_t* tokens, std::uint32_t byteSize) {
    constexpr std::uint32_t kAllSlots = (1u << tap::kSamplerSlotCount) - 1u;
    const std::uint32_t count = byteSize / 4u;
    if (!tokens || count == 0) {
        return kAllSlots;
    }
    const std::uint32_t version = tokens[0];
    const bool pixel = (version >> 16) == 0xffffu;
    const bool vertex = (version >> 16) == 0xfffeu;
    if (!pixel && !vertex) {
        return kAllSlots;
    }
    const std::uint32_t major = (version >> 8) & 0xffu, minor = version & 0xffu;
    if (major < 2) {
        // ps_1_x samples t0..t3 (ps_1_4: t0..t5) by register; vs_1_x samples nothing.
        return pixel ? (minor >= 4 ? 0x3fu : 0xfu) : 0u;
    }
    if (vertex && major < 3) {
        return 0u;
    }
    constexpr std::uint32_t kOpDcl = 31, kOpComment = 0xfffe, kOpEnd = 0xffff, kRegSampler = 10;
    std::uint32_t mask = 0;
    for (std::uint32_t i = 1; i < count;) {
        const std::uint32_t tok = tokens[i];
        const std::uint32_t opcode = tok & 0xffffu;
        if (opcode == kOpEnd) {
            break;
        }
        if (opcode == kOpComment) {
            i += 1 + ((tok >> 16) & 0x7fffu);
            continue;
        }
        const std::uint32_t length = (tok >> 24) & 0xfu;
        if (opcode == kOpDcl && length >= 2 && i + 2 < count) {
            const std::uint32_t dst = tokens[i + 2];
            const std::uint32_t regType = ((dst >> 28) & 0x7u) | ((dst >> 8) & 0x18u);
            const std::uint32_t reg = dst & 0x7ffu;
            if (regType == kRegSampler) {
                if (pixel && reg < 16) {
                    mask |= 1u << reg;
                } else if (vertex && reg < 4) {
                    mask |= 1u << (17 + reg);
                }
            }
        }
        i += 1 + length;
    }
    return mask;
}

FormatCompatibilityCategory formatCompatibilityCategory(std::uint32_t f) {
    using C = FormatCompatibilityCategory;
    // Vulkan core enum values (VK_FORMAT_*), grouped as Remix groups them.
    switch (f) {
    case 1:                                  // R4G4_UNORM_PACK8
    case 9: case 10: case 11: case 12: case 13: case 14: case 15: // R8_*
        return C::Color_Format_8_Bits;
    case 1000470001:                          // A1B5G5R5_UNORM_PACK16
    case 1000156007:                          // R10X6_UNORM_PACK16
    case 1000156017:                          // R12X4_UNORM_PACK16
    case 1000340000: case 1000340001:         // A4R4G4B4 / A4B4G4R4_UNORM_PACK16
    case 2: case 3: case 4: case 5: case 6: case 7: case 8: // R4G4B4A4 .. A1R5G5B5
    case 16: case 17: case 18: case 19: case 20: case 21: case 22: // R8G8_*
    case 70: case 71: case 72: case 73: case 74: case 75: case 76: // R16_*
        return C::Color_Format_16_Bits;
    case 1000156008:                          // R10X6G10X6_UNORM_2PACK16
    case 1000156018:                          // R12X4G12X4_UNORM_2PACK16
    case 1000464000:                          // R16G16_S10_5_NV
    case 37: case 38: case 39: case 40: case 41: case 42: case 43: // R8G8B8A8_*
    case 44: case 45: case 46: case 47: case 48: case 49: case 50: // B8G8R8A8_*
    case 51: case 52: case 53: case 54: case 55: case 56: case 57: // A8B8G8R8_*_PACK32
    case 58: case 59: case 60: case 61: case 62: case 63:          // A2R10G10B10_*
    case 64: case 65: case 66: case 67: case 68: case 69:          // A2B10G10R10_*
    case 77: case 78: case 79: case 80: case 81: case 82: case 83: // R16G16_*
    case 98: case 99: case 100:               // R32_UINT / SINT / SFLOAT
    case 122: case 123:                       // B10G11R11_UFLOAT, E5B9G9R9_UFLOAT
        return C::Color_Format_32_Bits;
    case 91: case 92: case 93: case 94: case 95: case 96: case 97: // R16G16B16A16_*
    case 101: case 102: case 103:             // R32G32_*
    case 110: case 111: case 112:             // R64_*
        return C::Color_Format_64_Bits;
    case 107: case 108: case 109:             // R32G32B32A32_*
    case 113: case 114: case 115:             // R64G64_*
        return C::Color_Format_128_Bits;
    case 119: case 120: case 121:             // R64G64B64A64_*
        return C::Color_Format_256_Bits;
    default:
        return C::InvalidFormatCompatibilityCategory;
    }
}

std::uint32_t renderTargetVkFormat(std::uint32_t d3dFormat) {
    const hash::TextureFormatInfo info = hash::textureFormatInfo(static_cast<hash::D3DFormat>(d3dFormat));
    return info.mapped ? info.vkFormat : 0u;
}

} // namespace fuse::relight::scene
