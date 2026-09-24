// FUSE Relight RL-1.1: recording tap (JSON Lines event stream). See recording_tap.hpp.
#include <fuse/relight/tap/recording_tap.hpp>

#include <fuse/relight/tap/d3d9_names.hpp>

#include "sha256.hpp"

#include <algorithm>
#include <cstring>

namespace fuse::relight::tap {

namespace {

/// Minimal JSON object writer (keys in insertion order; floats as %.9g, which round-trips a float
/// and matches the RL-0.4 sidecar writer).
class Json {
public:
    Json& raw(const std::string& key, const std::string& value) {
        sep();
        m_s += '"';
        m_s += key;
        m_s += "\":";
        m_s += value;
        return *this;
    }
    Json& str(const std::string& key, const std::string& value) { return raw(key, quote(value)); }
    Json& u(const std::string& key, std::uint64_t v) { return raw(key, std::to_string(v)); }
    Json& i(const std::string& key, std::int64_t v) { return raw(key, std::to_string(v)); }
    Json& b(const std::string& key, bool v) { return raw(key, v ? "true" : "false"); }
    Json& f(const std::string& key, float v) { return raw(key, num(v)); }
    Json& null(const std::string& key) { return raw(key, "null"); }
    std::string done() const { return "{" + m_s + "}"; }

    static std::string num(float v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
        return buf;
    }
    static std::string floats(const float* v, std::size_t n) {
        std::string s = "[";
        for (std::size_t k = 0; k < n; ++k) {
            s += (k ? "," : "") + num(v[k]);
        }
        return s + "]";
    }
    static std::string hex32(std::uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "\"0x%08x\"", v);
        return buf;
    }
    static std::string quote(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
        return out + "\"";
    }
    static std::string array(const std::vector<std::string>& items) {
        std::string s = "[";
        for (std::size_t k = 0; k < items.size(); ++k) {
            s += (k ? "," : "") + items[k];
        }
        return s + "]";
    }

private:
    void sep() {
        if (!m_s.empty()) {
            m_s += ',';
        }
    }
    std::string m_s;
};

template <std::size_t N>
std::string enumName(const names::NameEntry (&table)[N], std::uint32_t v, const char* fallbackPrefix) {
    const char* n = names::find(table, v);
    return n ? std::string(n) : std::string(fallbackPrefix) + std::to_string(v);
}

std::string color4(const Color4& c) {
    const float v[4] = {c.r, c.g, c.b, c.a};
    return Json::floats(v, 4);
}
std::string vec3(const Vec3& p) {
    const float v[3] = {p.x, p.y, p.z};
    return Json::floats(v, 3);
}

std::string transformName(std::uint32_t slot) {
    if (slot == kTransformView) {
        return "VIEW";
    }
    if (slot == kTransformProjection) {
        return "PROJECTION";
    }
    if (slot < kTransformWorld0) {
        return "TEXTURE" + std::to_string(slot - kTransformTexture0);
    }
    const std::uint32_t n = slot - kTransformWorld0;
    return n == 0 ? std::string("WORLD") : "WORLDMATRIX_" + std::to_string(n);
}

bool isIdentity(const float (&m)[16]) {
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return std::memcmp(m, kIdentity, sizeof kIdentity) == 0;
}

std::string idOrNull(ResourceId id) { return id == kNoResource ? std::string("null") : std::to_string(id); }

} // namespace

RecordingTap::RecordingTap(std::string path) : m_path(std::move(path)) {
    m_file = std::fopen(m_path.c_str(), "wb");
    if (!m_file) {
        std::fprintf(stderr, "fuse-relight: recording tap cannot open '%s'\n", m_path.c_str());
        return;
    }
    writeLine(Json()
                  .str("ev", "header")
                  .str("schema", "fuse.relight.tap_events/1")
                  .u("interface_version", kTapInterfaceVersion)
                  .done());
}

RecordingTap::~RecordingTap() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file) {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void RecordingTap::writeLine(const std::string& line) {
    if (!m_file) {
        return;
    }
    std::fwrite(line.data(), 1, line.size(), m_file);
    std::fputc('\n', m_file);
}

void RecordingTap::deviceLine(const char* ev, const DeviceEvent& e) {
    const PresentParameters& p = e.present;
    const std::string present = Json()
                                    .u("back_buffer_width", p.backBufferWidth)
                                    .u("back_buffer_height", p.backBufferHeight)
                                    .u("back_buffer_format", p.backBufferFormat)
                                    .u("back_buffer_count", p.backBufferCount)
                                    .u("multisample", p.multiSampleType)
                                    .u("swap_effect", p.swapEffect)
                                    .b("windowed", p.windowed)
                                    .b("auto_depth_stencil", p.enableAutoDepthStencil)
                                    .u("auto_depth_stencil_format", p.autoDepthStencilFormat)
                                    .u("flags", p.flags)
                                    .u("presentation_interval", p.presentationInterval)
                                    .done();
    const std::string vk = Json()
                               .b("imported", e.vulkan.imported)
                               .u("queue_family", e.vulkan.queueFamily)
                               .b("has_device", e.vulkan.device != 0)
                               .done();
    writeLine(Json()
                  .str("ev", ev)
                  .u("frame", m_frame)
                  .u("adapter", e.adapter)
                  .u("device_type", e.deviceType)
                  .u("behavior_flags", e.behaviorFlags)
                  .b("extended", e.extended)
                  .b("d3d8", e.d3d8)
                  .raw("present", present)
                  .raw("back_buffer", idOrNull(e.backBuffer))
                  .raw("auto_depth_stencil", idOrNull(e.autoDepthStencil))
                  .raw("vulkan", vk)
                  .done());
}

void RecordingTap::onDeviceCreate(const DeviceEvent& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    deviceLine("device_create", e);
}

void RecordingTap::onDeviceReset(const DeviceEvent& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    deviceLine("device_reset", e);
}

void RecordingTap::onDeviceDestroy() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_destroyed = true;
    writeLine(Json().str("ev", "device_destroy").u("frame", m_frame).done());
    if (m_file) {
        std::fflush(m_file);
    }
}

void RecordingTap::onTextureCreate(const TextureDesc& d) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json()
                  .str("ev", "texture_create")
                  .u("frame", m_frame)
                  .u("id", d.id)
                  .u("type", d.type)
                  .u("width", d.width)
                  .u("height", d.height)
                  .u("depth", d.depth)
                  .u("levels", d.mipLevels)
                  .u("array_size", d.arraySize)
                  .u("format", d.format)
                  .u("usage", d.usage)
                  .str("pool", enumName(names::kPools, d.pool, "POOL_"))
                  .u("multisample", d.multiSample)
                  .b("back_buffer", d.isBackBuffer)
                  .b("attachment_only", d.isAttachmentOnly)
                  .b("has_image", d.vkImage != 0)
                  .done());
}

void RecordingTap::onTextureUpload(const TextureUpload& u) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string sha = "null";
    if (u.data) {
        // Region bytes: every locked row of every locked slice (rows are rowPitch apart).
        detail::Sha256 h;
        const std::uint32_t depth = std::max<std::uint32_t>(1, u.box.back - u.box.front);
        for (std::uint32_t z = 0; z < depth; ++z) {
            const auto* slice = static_cast<const std::uint8_t*>(u.data) + std::size_t(z) * u.slicePitch;
            if (u.fullUpdate) {
                h.update(slice, std::size_t(u.rowPitch) * u.rows);
            } else {
                for (std::uint32_t r = 0; r < u.rows; ++r) {
                    h.update(slice + std::size_t(r) * u.rowPitch, u.rowPitch);
                }
            }
        }
        sha = Json::quote(h.hexDigest());
    }
    writeLine(Json()
                  .str("ev", "texture_upload")
                  .u("frame", m_frame)
                  .u("texture", u.texture)
                  .u("face", u.face)
                  .u("level", u.level)
                  .u("width", u.width)
                  .u("height", u.height)
                  .u("row_pitch", u.rowPitch)
                  .u("rows", u.rows)
                  .u("lock_flags", u.lockFlags)
                  .b("full", u.fullUpdate)
                  .raw("blob", sha)
                  .done());
}

void RecordingTap::onTextureCopy(const TextureCopy& c) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json()
                  .str("ev", "texture_copy")
                  .u("frame", m_frame)
                  .str("method", c.method == CopyMethod::UpdateTexture ? "UpdateTexture" : "UpdateSurface")
                  .u("source", c.source)
                  .u("destination", c.destination)
                  .u("source_face", c.sourceFace)
                  .u("source_level", c.sourceLevel)
                  .u("dest_face", c.destFace)
                  .u("dest_level", c.destLevel)
                  .b("has_rect", c.hasSourceRect)
                  .u("width", c.width)
                  .u("height", c.height)
                  .u("dest_x", c.destX)
                  .u("dest_y", c.destY)
                  .done());
}

void RecordingTap::onTextureWriteLock(const TextureWriteLock& l) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json()
                  .str("ev", "texture_write_lock")
                  .u("frame", m_frame)
                  .u("texture", l.texture)
                  .u("face", l.face)
                  .u("level", l.level)
                  .u("lock_flags", l.lockFlags)
                  .done());
}

void RecordingTap::onImageDestroy(const ImageDestroy& d) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json().str("ev", "image_destroy").u("frame", m_frame).u("texture", d.texture).done());
}

void RecordingTap::onBufferCreate(const BufferDesc& d) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bufferShadow[d.id].assign(d.size, 0);
    m_bufferShaCache.erase(d.id);
    Json j;
    j.str("ev", "buffer_create")
        .u("frame", m_frame)
        .u("id", d.id)
        .str("kind", d.kind == BufferKind::Index ? "index" : "vertex")
        .u("size", d.size)
        .u("usage", d.usage)
        .str("pool", enumName(names::kPools, d.pool, "POOL_"));
    if (d.kind == BufferKind::Index) {
        j.str("format", enumName(names::kIndexFormats, d.format, "FMT_"));
    } else {
        j.u("fvf", d.fvf);
    }
    writeLine(j.done());
}

std::string RecordingTap::bufferSha(ResourceId id) {
    auto cached = m_bufferShaCache.find(id);
    if (cached != m_bufferShaCache.end()) {
        return cached->second;
    }
    auto it = m_bufferShadow.find(id);
    if (it == m_bufferShadow.end()) {
        return std::string();
    }
    std::string sha = detail::sha256Hex(it->second.data(), it->second.size());
    m_bufferShaCache[id] = sha;
    return sha;
}

void RecordingTap::onBufferWrite(const BufferWrite& w) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::uint8_t>& shadow = m_bufferShadow[w.buffer];
    if (shadow.size() < w.bufferSize) {
        shadow.resize(w.bufferSize, 0);
    }
    constexpr std::uint32_t kLockDiscard = 0x2000; // D3DLOCK_DISCARD
    if (w.lockFlags & kLockDiscard) {
        std::fill(shadow.begin(), shadow.end(), std::uint8_t(0));
    }
    if (w.data && w.offset < shadow.size()) {
        const std::size_t n = std::min<std::size_t>(w.size, shadow.size() - w.offset);
        std::memcpy(shadow.data() + w.offset, w.data, n);
    }
    m_bufferShaCache.erase(w.buffer);
    writeLine(Json()
                  .str("ev", "buffer_write")
                  .u("frame", m_frame)
                  .u("buffer", w.buffer)
                  .u("offset", w.offset)
                  .u("size", w.size)
                  .u("lock_flags", w.lockFlags)
                  .str("blob", bufferSha(w.buffer))
                  .done());
}

void RecordingTap::onBufferDestroy(ResourceId id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bufferShadow.erase(id);
    m_bufferShaCache.erase(id);
    writeLine(Json().str("ev", "buffer_destroy").u("frame", m_frame).u("buffer", id).done());
}

std::string RecordingTap::shaderJson(const ShaderRef& s) {
    if (s.id == kNoResource) {
        return "null";
    }
    auto it = m_shaderSha.find(s.id);
    if (it == m_shaderSha.end()) {
        it = m_shaderSha.emplace(s.id, s.tokens ? detail::sha256Hex(s.tokens, s.byteSize) : std::string()).first;
    }
    char ver[32];
    const bool pixel = (s.version & 0xffff0000u) == 0xffff0000u;
    std::snprintf(ver, sizeof ver, "%s_%u_%u", pixel ? "ps" : "vs", (s.version >> 8) & 0xffu, s.version & 0xffu);
    return Json().u("id", s.id).str("version", ver).u("byte_size", s.byteSize).str("blob", it->second).done();
}

std::size_t RecordingTap::stateBlock(const DrawState& s) {
    Json j;
    // Render states: every named D3DRS the device tracks.
    {
        Json rs;
        if (s.renderStates) {
            for (const names::NameEntry& e : names::kRenderStates) {
                rs.u(e.name, s.renderStates[e.value]);
            }
        }
        j.raw("render_states", rs.done());
    }
    {
        std::vector<std::string> stages;
        for (std::uint32_t st = 0; s.textureStageStates && st < kTextureStageCount; ++st) {
            Json v;
            for (const names::NameEntry& e : names::kTextureStageStates) {
                v.u(e.name, s.textureStageStates[st][e.value - 1]);
            }
            stages.push_back(Json().u("stage", st).raw("states", v.done()).done());
        }
        j.raw("texture_stages", Json::array(stages));
    }
    {
        std::vector<std::string> samplers;
        for (std::uint32_t slot = 0; s.samplerStates && slot < kSamplerSlotCount; ++slot) {
            Json v;
            for (const names::NameEntry& e : names::kSamplerStates) {
                v.u(e.name, s.samplerStates[slot][e.value]);
            }
            samplers.push_back(Json().u("sampler", samplerFromSlot(slot)).raw("states", v.done()).done());
        }
        j.raw("samplers", Json::array(samplers));
    }
    {
        std::vector<std::string> textures;
        for (std::uint32_t slot = 0; slot < kSamplerSlotCount; ++slot) {
            if (s.textures[slot] != kNoResource) {
                textures.push_back(Json().u("stage", samplerFromSlot(slot)).u("texture", s.textures[slot]).done());
            }
        }
        j.raw("textures", Json::array(textures));
    }
    {
        std::vector<std::string> lights;
        for (std::uint32_t k = 0; k < s.lightCount; ++k) {
            const Light& l = s.lights[k];
            const float att[3] = {l.attenuation0, l.attenuation1, l.attenuation2};
            lights.push_back(Json()
                                 .u("index", l.index)
                                 .b("enabled", l.enabled)
                                 .str("type", enumName(names::kLightTypes, l.type, "LIGHT_"))
                                 .raw("diffuse", color4(l.diffuse))
                                 .raw("specular", color4(l.specular))
                                 .raw("ambient", color4(l.ambient))
                                 .raw("position", vec3(l.position))
                                 .raw("direction", vec3(l.direction))
                                 .f("range", l.range)
                                 .f("falloff", l.falloff)
                                 .raw("attenuation", Json::floats(att, 3))
                                 .f("theta", l.theta)
                                 .f("phi", l.phi)
                                 .done());
        }
        j.raw("lights", Json::array(lights));
    }
    j.raw("material", Json()
                          .raw("diffuse", color4(s.material.diffuse))
                          .raw("ambient", color4(s.material.ambient))
                          .raw("specular", color4(s.material.specular))
                          .raw("emissive", color4(s.material.emissive))
                          .f("power", s.material.power)
                          .done());
    j.raw("viewport", Json()
                          .u("x", s.viewport.x)
                          .u("y", s.viewport.y)
                          .u("width", s.viewport.width)
                          .u("height", s.viewport.height)
                          .f("min_z", s.viewport.minZ)
                          .f("max_z", s.viewport.maxZ)
                          .done());
    {
        std::vector<std::string> rts;
        for (ResourceId rt : s.renderTargets) {
            rts.push_back(idOrNull(rt));
        }
        j.raw("render_targets", Json::array(rts));
    }
    j.raw("depth_stencil", idOrNull(s.depthStencil));
    // Shader constants: non-zero registers only (a missing register reads as zero).
    auto constF = [](const float (*v)[4], std::uint32_t n) {
        std::vector<std::string> out;
        static const float kZero[4] = {0, 0, 0, 0};
        for (std::uint32_t r = 0; v && r < n; ++r) {
            if (std::memcmp(v[r], kZero, sizeof kZero) != 0) {
                out.push_back(Json().u("register", r).raw("value", Json::floats(v[r], 4)).done());
            }
        }
        return Json::array(out);
    };
    auto constI = [](const std::int32_t (*v)[4], std::uint32_t n) {
        std::vector<std::string> out;
        for (std::uint32_t r = 0; v && r < n; ++r) {
            if (v[r][0] || v[r][1] || v[r][2] || v[r][3]) {
                out.push_back(Json()
                                  .u("register", r)
                                  .raw("value", "[" + std::to_string(v[r][0]) + "," + std::to_string(v[r][1]) + "," +
                                                    std::to_string(v[r][2]) + "," + std::to_string(v[r][3]) + "]")
                                  .done());
            }
        }
        return Json::array(out);
    };
    auto constB = [](const std::uint32_t* bits, std::uint32_t n) {
        std::vector<std::string> out;
        for (std::uint32_t r = 0; bits && r < n; ++r) {
            if (bits[r / 32] & (1u << (r % 32))) {
                out.push_back(Json().u("register", r).b("value", true).done());
            }
        }
        return Json::array(out);
    };
    j.raw("vs_const_f", constF(s.vsConstF, s.vsConstFCount));
    j.raw("vs_const_i", constI(s.vsConstI, s.vsConstICount));
    j.raw("vs_const_b", constB(s.vsConstB, s.vsConstBCount));
    j.raw("ps_const_f", constF(s.psConstF, s.psConstFCount));
    j.raw("ps_const_i", constI(s.psConstI, s.psConstICount));
    j.raw("ps_const_b", constB(s.psConstB, s.psConstBCount));
    j.b("software_vp", s.softwareVertexProcessing);

    const std::string body = j.done();
    auto it = m_stateBlocks.find(body);
    if (it != m_stateBlocks.end()) {
        return it->second;
    }
    const std::size_t index = m_stateBlocks.size();
    m_stateBlocks.emplace(body, index);
    writeLine(Json().str("ev", "state_block").u("index", index).raw("state", body).done());
    return index;
}

DrawDecision RecordingTap::onDraw(const DrawCall& c, const DrawState& s) {
    static const char* kCalls[] = {"DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP"};
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t block = stateBlock(s);
    Json j;
    j.str("ev", "draw")
        .u("frame", m_frame)
        .str("call", kCalls[static_cast<std::uint32_t>(c.call) & 3u])
        .str("primitive", enumName(names::kPrimitiveTypes, c.primitiveType, "PT_"))
        .u("prim_count", c.primitiveCount)
        .u("start_vertex", c.startVertex)
        .u("vertex_count", c.vertexCount)
        .i("base_vertex", c.baseVertex)
        .u("min_index", c.minIndex)
        .u("num_vertices", c.numVertices)
        .u("start_index", c.startIndex)
        .u("index_count", c.indexCount)
        .u("instance_count", c.instanceCount);
    const bool indexed = c.call == DrawCallType::DrawIndexedPrimitive;
    const bool up = c.call == DrawCallType::DrawPrimitiveUP || c.call == DrawCallType::DrawIndexedPrimitiveUP;
    if (indexed && s.indices.buffer != kNoResource) {
        j.raw("index_buffer", Json()
                                  .u("buffer", s.indices.buffer)
                                  .str("format", enumName(names::kIndexFormats, s.indices.format, "FMT_"))
                                  .str("blob", bufferSha(s.indices.buffer))
                                  .done());
    } else {
        j.null("index_buffer");
    }
    if (up) {
        Json u;
        u.u("vertex_stride", c.upVertexStride)
            .str("vertex_blob", detail::sha256Hex(c.upVertexData, c.upVertexBytes));
        if (c.upIndexData) {
            u.str("index_blob", detail::sha256Hex(c.upIndexData, c.upIndexBytes))
                .str("index_format", enumName(names::kIndexFormats, c.upIndexFormat, "FMT_"));
        } else {
            u.null("index_blob").null("index_format");
        }
        j.raw("up", u.done());
    }
    {
        std::vector<std::string> elems;
        for (std::uint32_t k = 0; k < s.elementCount; ++k) {
            const VertexElement& e = s.elements[k];
            elems.push_back(Json()
                                .u("stream", e.stream)
                                .u("offset", e.offset)
                                .str("type", enumName(names::kDeclTypes, e.type, "DECLTYPE_"))
                                .u("size", e.type < 18 ? names::kDeclTypeSize[e.type] : 0)
                                .u("method", e.method)
                                .str("usage", enumName(names::kDeclUsages, e.usage, "DECLUSAGE_"))
                                .u("usage_index", e.usageIndex)
                                .done());
        }
        j.raw("elements", Json::array(elems));
    }
    j.raw("fvf", s.fvf ? std::to_string(s.fvf) : std::string("null"));
    {
        std::vector<std::string> streams;
        if (!up) {
            for (std::uint32_t k = 0; k < kStreamCount; ++k) {
                const StreamBinding& b = s.streams[k];
                if (b.buffer == kNoResource) {
                    continue;
                }
                streams.push_back(Json()
                                      .u("stream", k)
                                      .u("buffer", b.buffer)
                                      .u("offset", b.offset)
                                      .u("stride", b.stride)
                                      .u("frequency", b.frequency)
                                      .str("blob", bufferSha(b.buffer))
                                      .done());
            }
        }
        j.raw("streams", Json::array(streams));
    }
    {
        Json t;
        for (std::uint32_t slot = 0; s.transforms && slot < kTransformCount; ++slot) {
            if (!isIdentity(s.transforms[slot])) {
                t.raw(transformName(slot), Json::floats(s.transforms[slot], 16));
            }
        }
        j.raw("transforms", t.done());
    }
    j.raw("vertex_shader", shaderJson(s.vertexShader));
    j.raw("pixel_shader", shaderJson(s.pixelShader));
    j.u("state", block);
    j.str("decision", "raster");
    writeLine(j.done());
    return DrawDecision::Raster;
}

void RecordingTap::onQueryBegin(const QueryEvent& q) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json().str("ev", "query_begin").u("frame", m_frame).u("type", q.type).done());
}

void RecordingTap::onQueryEnd(const QueryEvent& q) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json().str("ev", "query_end").u("frame", m_frame).u("type", q.type).done());
}

void RecordingTap::onClear(const ClearEvent& c) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> rts;
    for (ResourceId rt : c.renderTargets) {
        rts.push_back(idOrNull(rt));
    }
    writeLine(Json()
                  .str("ev", "clear")
                  .u("frame", m_frame)
                  .u("rect_count", c.rectCount)
                  .u("flags", c.flags)
                  .raw("color", Json::hex32(c.color))
                  .f("z", c.z)
                  .u("stencil", c.stencil)
                  .raw("render_targets", Json::array(rts))
                  .raw("depth_stencil", idOrNull(c.depthStencil))
                  .done());
}

void RecordingTap::onSetRenderTarget(const SetRenderTargetEvent& e) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json()
                  .str("ev", "set_render_target")
                  .u("frame", m_frame)
                  .u("index", e.index)
                  .raw("texture", idOrNull(e.texture))
                  .done());
}

void RecordingTap::onInjectPoint(const FrameEvent& f) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json().str("ev", "inject_point").u("frame", m_frame).u("back_buffer", f.backBuffer).done());
}

void RecordingTap::onPresent(const FrameEvent& f) {
    std::lock_guard<std::mutex> lock(m_mutex);
    writeLine(Json()
                  .str("ev", "present")
                  .u("frame", m_frame)
                  .u("back_buffer", f.backBuffer)
                  .u("width", f.width)
                  .u("height", f.height)
                  .u("format", f.format)
                  .b("has_image", f.backBufferVkImage != 0)
                  .done());
    m_frame = f.frame + 1;
    if (m_file) {
        std::fflush(m_file);
    }
}

} // namespace fuse::relight::tap
