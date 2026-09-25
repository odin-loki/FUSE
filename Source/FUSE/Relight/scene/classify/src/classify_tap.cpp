// FUSE Relight RL-1.2: tap events -> D3DStateModel -> DrawClassifier. See classify_tap.hpp.
#include <fuse/relight/scene/classify/classify_tap.hpp>

#include <fuse/relight/hash/texture_hash.hpp>

#include <cstdlib>
#include <cstring>

namespace fuse::relight::scene {

namespace {

bool envSet(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}

} // namespace

TextureRecord textureRecordFromDesc(const tap::TextureDesc& d) {
    TextureRecord t;
    t.id = d.id;
    t.type = d.type;
    t.width = d.width;
    t.height = d.height;
    t.depth = d.depth ? d.depth : 1;
    t.mipLevels = d.mipLevels;
    t.arraySize = d.arraySize;
    t.format = d.format;
    t.usage = d.usage;
    t.pool = d.pool;
    t.multiSample = d.multiSample;
    t.isBackBuffer = d.isBackBuffer;
    t.isAttachmentOnly = d.isAttachmentOnly;
    t.hasImage = d.vkImage != 0;
    if (d.usage & d3d::USAGE_RENDERTARGET) {
        // D3D9CommonTexture::CreatePrimaryImage (NV-DXVK): render targets carry the descriptor hash.
        // The tap does not report Discard or MultisampleQuality; both are 0 for textures.
        hash::TextureDescriptor desc;
        desc.width = d.width;
        desc.height = d.height;
        desc.depth = t.depth;
        desc.arraySize = d.arraySize;
        desc.mipLevels = d.mipLevels;
        desc.usage = d.usage;
        desc.format = d.format;
        desc.pool = d.pool;
        desc.multiSample = d.multiSample;
        desc.isBackBuffer = d.isBackBuffer;
        desc.isAttachmentOnly = d.isAttachmentOnly;
        t.descriptorHash = hash::hashTextureDescriptor(desc);
    }
    return t;
}

// ---- D3DStateTracker ---------------------------------------------------------------------------------

D3DStateTracker::D3DStateTracker()
    : resolutionOverride(envSet("DXVK_RESOLUTION_WIDTH") || envSet("DXVK_RESOLUTION_HEIGHT")) {}

void D3DStateTracker::onDevice(const tap::DeviceEvent& e) {
    m_backBufferWidth = e.present.backBufferWidth;
    m_backBufferHeight = e.present.backBufferHeight;
    m_d3d8 = e.d3d8;
}

void D3DStateTracker::onTextureCreate(const tap::TextureDesc& d) {
    TextureRecord record = textureRecordFromDesc(d);
    // Hashes supplied before the descriptor (a replay) survive; otherwise start fresh.
    auto it = m_textures.find(d.id);
    if (it != m_textures.end()) {
        record.imageHash = it->second.imageHash;
        if (it->second.descriptorHash != kEmptyHash) {
            record.descriptorHash = it->second.descriptorHash;
        }
    }
    m_textures[d.id] = record;
}

void D3DStateTracker::onImageDestroy(const tap::ImageDestroy& d) { m_textures.erase(d.texture); }

void D3DStateTracker::onQueryBegin(const tap::QueryEvent& q) {
    if (q.type == d3d::QUERYTYPE_OCCLUSION) {
        ++m_activeOcclusionQueries;
    }
}

void D3DStateTracker::onQueryEnd(const tap::QueryEvent& q) {
    if (q.type == d3d::QUERYTYPE_OCCLUSION && m_activeOcclusionQueries > 0) {
        --m_activeOcclusionQueries;
    }
}

void D3DStateTracker::setTextureHash(tap::ResourceId id, Hash64 hash) {
    TextureRecord& t = m_textures[id];
    t.id = id;
    t.imageHash = hash;
}

void D3DStateTracker::setDescriptorHash(tap::ResourceId id, Hash64 hash) {
    TextureRecord& t = m_textures[id];
    t.id = id;
    t.descriptorHash = hash;
}

const TextureRecord* D3DStateTracker::texture(tap::ResourceId id) const {
    auto it = m_textures.find(id);
    return it == m_textures.end() ? nullptr : &it->second;
}

D3DStateModel D3DStateTracker::buildModel(const tap::DrawCall& call, const tap::DrawState& s) const {
    D3DStateModel m;
    m.primitiveType = call.primitiveType;
    m.primitiveCount = call.primitiveCount;
    m.indexed = call.call == tap::DrawCallType::DrawIndexedPrimitive ||
                call.call == tap::DrawCallType::DrawIndexedPrimitiveUP;
    m.backBufferWidth = m_backBufferWidth;
    m.backBufferHeight = m_backBufferHeight;
    m.d3d8 = m_d3d8;
    m.resolutionOverride = resolutionOverride;
    m.activeOcclusionQueries = m_activeOcclusionQueries;

    if (s.renderStates) {
        std::memcpy(m.renderStates.data(), s.renderStates, sizeof(std::uint32_t) * tap::kRenderStateCount);
    }
    if (s.textureStageStates) {
        for (std::uint32_t stage = 0; stage < tap::kTextureStageCount; ++stage) {
            // DXVK's layout: element i is D3DTEXTURESTAGESTATETYPE (i + 1).
            for (std::uint32_t i = 0; i + 1 < kTextureStageStateSlots; ++i) {
                m.textureStages[stage][i + 1] = s.textureStageStates[stage][i];
            }
        }
    }
    auto lookup = [this](tap::ResourceId id) {
        if (id == tap::kNoResource) {
            return TextureRecord{};
        }
        if (const TextureRecord* t = texture(id)) {
            return *t;
        }
        TextureRecord unknown; // never announced: keep the identity, no properties
        unknown.id = id;
        return unknown;
    };
    for (std::uint32_t slot = 0; slot < tap::kSamplerSlotCount; ++slot) {
        m.textures[slot] = lookup(s.textures[slot]);
    }
    m.renderTarget0 = lookup(s.renderTargets[0]);

    m.usesVertexShader = s.vertexShader.id != tap::kNoResource;
    m.usesPixelShader = s.pixelShader.id != tap::kNoResource;
    m.vsSamplerMask = m.usesVertexShader ? shaderSamplerMask(s.vertexShader.tokens, s.vertexShader.byteSize) : 0u;
    m.psSamplerMask = m.usesPixelShader ? shaderSamplerMask(s.pixelShader.tokens, s.pixelShader.byteSize)
                                        : kFixedFunctionPsSamplerMask;

    for (std::uint32_t i = 0; i < s.elementCount && i < tap::kMaxVertexElements; ++i) {
        const tap::VertexElement& e = s.elements[i];
        if (e.usage == d3d::DECLUSAGE_POSITIONT) {
            m.hasPositionT = true;
        } else if (e.usage == d3d::DECLUSAGE_BLENDWEIGHT && e.usageIndex == 0) {
            m.hasBlendWeight0 = true;
        } else if (e.usage == d3d::DECLUSAGE_BLENDINDICES) {
            m.hasBlendIndices = true;
        }
    }
    if (s.transforms) {
        std::memcpy(m.view.data(), s.transforms[tap::kTransformView], sizeof(float) * 16);
        std::memcpy(m.projection.data(), s.transforms[tap::kTransformProjection], sizeof(float) * 16);
        std::memcpy(m.world.data(), s.transforms[tap::kTransformWorld0], sizeof(float) * 16);
    }
    m.viewport = s.viewport;
    return m;
}

// ---- ClassifyTap ----------------------------------------------------------------------------------------

ClassifyTap::ClassifyTap(tap::IRelightTap* forward, Sink sink, bool applyDecisions)
    : m_forward(forward), m_sink(std::move(sink)), m_applyDecisions(applyDecisions) {}

void ClassifyTap::onDeviceCreate(const tap::DeviceEvent& e) {
    if (m_forward) {
        m_forward->onDeviceCreate(e);
    }
    m_tracker.onDevice(e);
}

void ClassifyTap::onDeviceReset(const tap::DeviceEvent& e) {
    if (m_forward) {
        m_forward->onDeviceReset(e);
    }
    m_tracker.onDevice(e);
}

void ClassifyTap::onDeviceDestroy() {
    if (m_forward) {
        m_forward->onDeviceDestroy();
    }
}

void ClassifyTap::onTextureCreate(const tap::TextureDesc& d) {
    if (m_forward) {
        m_forward->onTextureCreate(d);
    }
    m_tracker.onTextureCreate(d);
}

void ClassifyTap::onTextureUpload(const tap::TextureUpload& u) {
    if (m_forward) {
        m_forward->onTextureUpload(u);
    }
}

void ClassifyTap::onTextureCopy(const tap::TextureCopy& c) {
    if (m_forward) {
        m_forward->onTextureCopy(c);
    }
}

void ClassifyTap::onTextureWriteLock(const tap::TextureWriteLock& l) {
    if (m_forward) {
        m_forward->onTextureWriteLock(l);
    }
}

void ClassifyTap::onImageDestroy(const tap::ImageDestroy& d) {
    if (m_forward) {
        m_forward->onImageDestroy(d);
    }
    m_tracker.onImageDestroy(d);
}

void ClassifyTap::onBufferCreate(const tap::BufferDesc& d) {
    if (m_forward) {
        m_forward->onBufferCreate(d);
    }
}

void ClassifyTap::onBufferWrite(const tap::BufferWrite& w) {
    if (m_forward) {
        m_forward->onBufferWrite(w);
    }
}

void ClassifyTap::onBufferDestroy(tap::ResourceId id) {
    if (m_forward) {
        m_forward->onBufferDestroy(id);
    }
}

tap::DrawDecision ClassifyTap::onDraw(const tap::DrawCall& call, const tap::DrawState& state) {
    const tap::DrawDecision forwarded = m_forward ? m_forward->onDraw(call, state) : tap::DrawDecision::Raster;
    ClassifiedDraw draw;
    draw.frame = m_frame;
    draw.indexInFrame = m_drawInFrame++;
    draw.result = m_classifier.classify(m_tracker.buildModel(call, state));
    if (m_sink) {
        m_sink(draw);
    }
    return m_applyDecisions ? toTapDecision(draw.result.prepareFlags) : forwarded;
}

bool ClassifyTap::substituteVertexShader(const tap::ShaderModule& m, std::vector<std::uint32_t>& replacement) {
    return m_forward ? m_forward->substituteVertexShader(m, replacement) : false;
}

void ClassifyTap::onQueryBegin(const tap::QueryEvent& q) {
    if (m_forward) {
        m_forward->onQueryBegin(q);
    }
    m_tracker.onQueryBegin(q);
}

void ClassifyTap::onQueryEnd(const tap::QueryEvent& q) {
    if (m_forward) {
        m_forward->onQueryEnd(q);
    }
    m_tracker.onQueryEnd(q);
}

void ClassifyTap::onClear(const tap::ClearEvent& c) {
    if (m_forward) {
        m_forward->onClear(c);
    }
}

void ClassifyTap::onSetRenderTarget(const tap::SetRenderTargetEvent& e) {
    if (m_forward) {
        m_forward->onSetRenderTarget(e);
    }
}

void ClassifyTap::onInjectPoint(const tap::FrameEvent& f) {
    if (m_forward) {
        m_forward->onInjectPoint(f);
    }
}

void ClassifyTap::onPresent(const tap::FrameEvent& f) {
    if (m_forward) {
        m_forward->onPresent(f);
    }
    m_classifier.endFrame();
    m_frame = f.frame + 1;
    m_drawInFrame = 0;
}

} // namespace fuse::relight::scene
