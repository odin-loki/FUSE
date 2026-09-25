// FUSE Relight RL-5.1: relight.frame.mode = pathtrace (see pt_frame_renderer.hpp).
#include <fuse/relight/render/pathtrace/pt_frame_renderer.hpp>

#include <fuse/relight/render/frame/bindless_images.hpp>
#include <fuse/relight/render/frame/renderer_context.hpp>
#include <fuse/relight/render/frame/scene_adapter.hpp>
#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>
#include <fuse/relight/render/pathtrace/restir_di_gpu.hpp>

#include "pt_reference_kernels.hpp"

#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/relight/render/frame/vk_dispatch.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace fuse::relight::render::pathtrace {

namespace rr = fuse::renderer;
namespace rg = fuse::renderer::rg;
namespace rf = fuse::relight::render::frame;
namespace rs = fuse::relight::render::raster;
namespace rl = fuse::relight::render::lights;

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &PtFrameOptions::samplesPerFrame, &PtFrameOptions::maxBounces, &PtFrameOptions::exposure,
    &PtFrameOptions::accumulate,      &PtFrameOptions::referenceSpp,
};

float toLinear(float display) { return std::pow(std::clamp(display, 0.f, 1.f), 2.2f); }

u64 fnv(u64 h, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

std::uint8_t srgbByte(double linear) {
    const double c = std::clamp(linear, 0.0, 1.0);
    const double s = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(std::lround(s * 255.0));
}

// VkFormat values of the 8-bit colour layouts the present copy writes.
constexpr std::uint32_t kR8G8B8A8Unorm = 37u, kR8G8B8A8Srgb = 43u, kB8G8R8A8Unorm = 44u, kB8G8R8A8Srgb = 50u;

} // namespace

PtFrameConfig PtFrameConfig::fromOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
    PtFrameConfig c;
    c.samplesPerFrame = static_cast<u32>(std::clamp(PtFrameOptions::samplesPerFrame(), 1, 4096));
    c.maxBounces = static_cast<u32>(std::clamp(PtFrameOptions::maxBounces(), 1, 64));
    c.exposure = PtFrameOptions::exposure();
    c.accumulate = PtFrameOptions::accumulate();
    c.referenceSpp = static_cast<u32>(std::clamp(PtFrameOptions::referenceSpp(), 0, 1 << 16));
    c.raster = rs::RasterConfig::fromOptions();
    return c;
}

bool ptSceneFromRaster(const rs::RasterFrame& frame, const std::vector<scene::LightRecord>* gameLights,
                       const std::vector<rf::AdapterLight>* sceneLights, const PtFrameConfig& config, PtScene& out,
                       PtFromRasterStats* stats) {
    PtFromRasterStats st;
    out = PtScene{};
    if (!frame.camera.valid || !frame.camera.perspective) {
        if (stats != nullptr) {
            *stats = st;
        }
        return false;
    }
    for (const rs::RasterDraw& d : frame.draws) {
        ++st.draws;
        const bool objectSpace = d.positions == rs::PositionSource::FixedFunction ||
                                 d.positions == rs::PositionSource::VertexCapture;
        if (d.bucket == rs::Bucket::Skipped || !objectSpace || d.vertexCount < 3u || d.gpu >= frame.gpuDraws.size() ||
            std::size_t(d.firstVertex) + d.vertexCount > frame.vertices.size()) {
            ++st.skipped;
            continue;
        }
        const rs::RasterDrawGpu& g = frame.gpuDraws[d.gpu];
        const rs::RasterMaterial& m = g.material;
        PtMaterial pm;
        const bool vertexColor = (m.flags & rs::kMatVertexColor) != 0u;
        pm.bsdf.albedo = vertexColor ? bsdfk::float3(1.f, 1.f, 1.f)
                                     : bsdfk::float3(toLinear(m.diffuse[0]), toLinear(m.diffuse[1]),
                                                     toLinear(m.diffuse[2]));
        pm.bsdf.roughness = std::clamp(m.roughness, 0.02f, 1.f);
        pm.bsdf.metallic = std::clamp(m.metallic, 0.f, 1.f);
        pm.bsdf.emission = bsdfk::float3(toLinear(m.emissive[0]), toLinear(m.emissive[1]), toLinear(m.emissive[2]));
        pm.flags = (vertexColor ? kPtMatVertexColor : 0u) | ((m.flags & rs::kMatUnlit) != 0u ? kPtMatUnlit : 0u);
        if ((m.flags & rs::kMatAlphaTest) != 0u) {
            pm.flags |= kPtMatAlphaTest;
            pm.alphaCompare = m.alphaTest & 0xffu;
            pm.alphaReference = static_cast<float>((m.alphaTest >> 8) & 0xffu) / 255.f;
        }
        if (d.bucket == rs::Bucket::Blend) {
            pm.flags |= kPtMatAlphaBlend;
            pm.bsdf.opacity = vertexColor ? 1.f : std::clamp(m.diffuse[3], 0.f, 1.f);
        }
        PtMesh mesh;
        mesh.material = static_cast<u32>(out.materials.size());
        const u32 n = d.vertexCount - d.vertexCount % 3u;
        const bool normals = (m.flags & rs::kMatHasNormals) != 0u;
        mesh.positions.reserve(std::size_t(n) * 3u);
        for (u32 v = 0; v < n; ++v) {
            const rs::RasterVertex& rv = frame.vertices[d.firstVertex + v];
            mesh.positions.insert(mesh.positions.end(), rv.pos, rv.pos + 3);
            if (normals) {
                mesh.normals.insert(mesh.normals.end(), rv.normal, rv.normal + 3);
            }
            if (vertexColor) {
                const float c[4] = {toLinear(float(rv.color & 0xffu) / 255.f),
                                    toLinear(float((rv.color >> 8) & 0xffu) / 255.f),
                                    toLinear(float((rv.color >> 16) & 0xffu) / 255.f),
                                    float((rv.color >> 24) & 0xffu) / 255.f};
                mesh.colors.insert(mesh.colors.end(), c, c + 4);
            }
            mesh.indices.push_back(v);
        }
        PtInstance inst;
        inst.mesh = static_cast<u32>(out.meshes.size());
        inst.objectToWorld = fromD3dMatrix(g.objectToWorld);
        if (d.bucket == rs::Bucket::Blend || (m.flags & rs::kMatUnlit) != 0u) {
            inst.flags = kPtInstanceVisible; // blended / unlit (sky) layers cast no shadow, as in the raster remaster
        }
        out.materials.push_back(pm);
        out.meshes.push_back(std::move(mesh));
        out.instances.push_back(inst);
        ++st.meshes;
        st.triangles += n / 3u;
    }
    constexpr float kDistantHalfAngle = 0.0349f / 2.f;
    if (gameLights != nullptr) {
        for (const scene::LightRecord& l : *gameLights) {
            if (l.isOff()) {
                continue;
            }
            const lk::float3 radiance(l.radiance[0], l.radiance[1], l.radiance[2]);
            if (l.type == hash::LightType::Distant) {
                out.lights.push_back(rl::makeDistantLight(lk::float3(l.direction[0], l.direction[1], l.direction[2]),
                                                          l.halfAngle, radiance));
                continue;
            }
            lk::RlLight L = rl::makeSphereLight(lk::float3(l.position[0], l.position[1], l.position[2]),
                                                std::max(l.radius, 1e-3f), radiance);
            if (l.shaping.enabled) {
                rl::setShaping(L,
                               lk::float3(l.shaping.direction[0], l.shaping.direction[1], l.shaping.direction[2]),
                               std::acos(std::clamp(l.shaping.cosConeAngle, -1.f, 1.f)), l.shaping.coneSoftness,
                               l.shaping.focusExponent);
            }
            out.lights.push_back(L);
        }
    } else if (sceneLights != nullptr) {
        for (const rf::AdapterLight& a : *sceneLights) {
            const renderer::gpu_scene::GpuLight& g = a.light;
            const lk::float3 flux(g.color[0] * g.intensity, g.color[1] * g.intensity, g.color[2] * g.intensity);
            const lk::float3 dir(g.direction[0], g.direction[1], g.direction[2]);
            if (g.type == static_cast<u32>(renderer::gpu_scene::GpuLightType::Directional)) {
                out.lights.push_back(rl::makeDistantLight(dir, kDistantHalfAngle, flux));
                continue;
            }
            // A point light of intensity I as a small sphere of radius r: L = I / (pi r^2).
            constexpr float r = 0.1f;
            const float k = 1.f / (3.14159265f * r * r);
            lk::RlLight L = rl::makeSphereLight(lk::float3(g.position[0], g.position[1], g.position[2]), r,
                                                lk::float3(flux.x * k, flux.y * k, flux.z * k));
            if (g.type == static_cast<u32>(renderer::gpu_scene::GpuLightType::Spot)) {
                rl::setShaping(L, dir, std::acos(std::clamp(g.cosOuter, -1.f, 1.f)), g.cosInner - g.cosOuter, 0.f);
            }
            out.lights.push_back(L);
        }
    }
    const u32 mode = config.raster.fallbackLightMode;
    if (mode == 2u || (mode == 1u && out.lights.empty())) {
        const float* dir = config.raster.fallbackDirection;
        const float* rad = config.raster.fallbackRadiance;
        out.lights.push_back(rl::makeDistantLight(lk::float3(dir[0], dir[1], dir[2]), kDistantHalfAngle,
                                                  lk::float3(rad[0], rad[1], rad[2])));
        st.fallbackLight = true;
    }
    st.lights = static_cast<u32>(out.lights.size());
    const rs::RasterCamera& c = frame.camera;
    std::copy(c.eye, c.eye + 3, out.camera.origin);
    std::copy(c.forward, c.forward + 3, out.camera.forward);
    std::copy(c.up, c.up + 3, out.camera.up);
    out.camera.fovY = c.fovY;
    out.camera.aspect = c.aspect;
    out.camera.leftHanded = true;
    if (stats != nullptr) {
        *stats = st;
    }
    return st.meshes > 0u;
}

namespace {

class PathTraceFrameRenderer final : public rf::IFrameRenderer {
public:
    explicit PathTraceFrameRenderer(const rf::FrameConfig&) : m_config(PtFrameConfig::fromOptions()) {}
    ~PathTraceFrameRenderer() override { detach(); }

    bool attach(rf::RendererContext& context, tap::IFrameHost& host, rf::BindlessImageRegistry& registry) override;
    void detach() override;
    bool prepare(const rf::FrameInputs& in, std::uint64_t retireSerial) override;
    void collect(std::uint64_t completedSerial) override;
    std::string recordJson() const override;
    std::string headerJson() const override;
    const std::string& lastError() const override { return m_error; }

    bool declare(rg::Graph& graph, rg::TextureRef output, const rf::GpuImage& outputImage) override;
    Image image(std::uint32_t) const override { return Image{}; }
    std::uint64_t buffer(std::uint32_t) const override { return 0; }
    void record(std::uint32_t pass, std::uint64_t commandBuffer) override;

private:
    static constexpr u32 kRing = 3u;
    static constexpr std::size_t kStagingBytes = 16u * 1024u * 1024u;
    struct Slot {
        rr::Buffer buffer{};
        std::uint64_t serial = 0;
    };

    bool trace(u32 width, u32 height);
    void compareReference(u32 width, u32 height);
    bool fail(const char* why) {
        m_error = why;
        return false;
    }

    PtFrameConfig m_config;
    rf::RendererContext* m_context = nullptr;
    std::string m_error;
    rr::Buffer m_staging{};
    rr::UploadQueue m_upload;
    std::unique_ptr<rg::Executor> m_executor;
    PathTracerGpu m_gpu;
    RestirDiGpu m_restir;       ///< RL-5.2: rtx.useRTXDI (direct light of the first sample per frame)
    RestirDiSettings m_rdi{};
    rg::Graph m_graph;
    PtScene m_scene;
    PtCompiledScene m_compiled;
    rs::RasterFrame m_raster;
    PtFromRasterStats m_stats{};
    u64 m_geometryKey = 0; ///< vertices + draw structure: a change recompiles
    u64 m_sceneKey = 0;    ///< everything the image depends on: a change restarts the accumulation
    u64 m_serial = 0;      ///< the path tracer's own frame serial (its graph is waited for every frame)
    u32 m_sampleBase = 0;
    u32 m_width = 0, m_height = 0;
    bool m_accumulated = false;
    Slot m_slots[kRing];
    u32 m_slot = 0;
    std::uint64_t m_completed = 0;
    bool m_prepared = false;
    std::uint64_t m_outputImage = 0;
    u32 m_presentPass = ~0u;
    u32 m_frames = 0;
    u32 m_recompiles = 0;
    // reference (relight.pathtrace.referenceSpp)
    PtReferenceImage m_reference;
    u64 m_referenceKey = 0;
    bool m_referenceValid = false;
    double m_refRmse = -1.0, m_refWorstZ = 0.0;
    u32 m_refBlocks = 0, m_refFailing = 0;
};

bool PathTraceFrameRenderer::attach(rf::RendererContext& context, tap::IFrameHost&, rf::BindlessImageRegistry&) {
    detach();
    m_context = &context;
    rr::BufferDesc d{};
    d.size = kStagingBytes;
    d.usage = rr::BufferUsage::TransferSrc;
    d.memoryUsage = rr::MemoryUsage::CpuToGpu;
    d.name = "relight.pt.frame.staging";
    if (!context.allocator().createBuffer(d, m_staging) || m_staging.mapped == nullptr ||
        !m_upload.init(&context.device(), m_staging.handle, m_staging.mapped, kStagingBytes)) {
        detach();
        return fail("path tracer staging / upload queue unavailable");
    }
    m_executor = rg::Executor::create(context.device(), &context.allocator());
    if (m_executor == nullptr || !m_executor->isValid()) {
        detach();
        return fail("path tracer render-graph executor unavailable");
    }
    PathTracerGpuDesc gd{};
    gd.device = &context.device();
    gd.allocator = &context.allocator();
    gd.upload = &m_upload;
    gd.bindless = &context.bindless();
    gd.framesInFlight = 1;
    if (!m_gpu.init(gd)) {
        const std::string keep = std::string("PathTracerGpu: ") + m_gpu.reason();
        detach();
        m_error = keep;
        return false;
    }
    m_rdi = RestirDiSettings::fromOptions();
    if (m_rdi.enabled) {
        RestirDiGpuDesc rd{};
        rd.device = &context.device();
        rd.allocator = &context.allocator();
        rd.bindless = &context.bindless();
        rd.framesInFlight = 1;
        m_rdi.enabled = m_restir.init(rd); // unavailable: plain NEE
    }
    m_error.clear();
    return true;
}

void PathTraceFrameRenderer::detach() {
    if (m_context == nullptr) {
        return;
    }
    if (m_executor != nullptr) {
        m_executor->waitIdle();
    }
    m_upload.waitAll();
    m_restir.destroy();
    m_gpu.destroy();
    m_executor.reset();
    m_upload.destroy();
    for (Slot& s : m_slots) {
        if (s.buffer.handle != nullptr) {
            m_context->allocator().destroyBuffer(s.buffer);
        }
        s = Slot{};
    }
    if (m_staging.handle != nullptr) {
        m_context->allocator().destroyBuffer(m_staging);
    }
    m_staging = rr::Buffer{};
    m_geometryKey = m_sceneKey = 0;
    m_referenceValid = false;
    m_prepared = false;
    m_context = nullptr;
}

void PathTraceFrameRenderer::collect(std::uint64_t completedSerial) {
    m_completed = std::max(m_completed, completedSerial);
}

bool PathTraceFrameRenderer::trace(u32 width, u32 height) {
    ++m_serial;
    PtFrameDesc fd;
    fd.width = width;
    fd.height = height;
    fd.frameSeed = 1u;
    fd.sampleBase = m_sampleBase;
    fd.accumulate = m_accumulated;
    fd.settings.maxBounces = m_config.maxBounces;
    fd.settings.samplesPerPixel = m_config.samplesPerFrame;
    if (m_rdi.enabled) {
        fd.settings = withRestirDi(fd.settings);
    }
    if (!m_gpu.setScene(m_compiled) || !m_gpu.beginFrame(m_serial, m_compiled, fd) ||
        (m_rdi.enabled && !m_restir.beginFrame(m_serial, m_compiled, fd, m_rdi))) {
        return fail(m_rdi.enabled && m_restir.valid() ? m_restir.reason() : m_gpu.reason());
    }
    m_graph.reset();
    const PtGraphRefs refs = m_gpu.importInto(m_graph);
    if (!refs.valid || (m_rdi.enabled && !m_restir.addPasses(m_graph, m_gpu, refs)) ||
        !m_gpu.addTracePass(m_graph, refs)) {
        return fail("path tracer graph");
    }
    m_graph.addPass("relight.pt.readback", nullptr, nullptr).use(refs.outputs, rg::Access::HostRead);
    m_context->lockQueue();
    m_upload.flush();
    const rg::ExecuteResult result = m_executor->execute(m_graph);
    m_context->unlockQueue();
    const bool waited = m_executor->waitIdle() && m_upload.waitAll();
    m_gpu.collectRetired(m_serial);
    m_restir.collectRetired(m_serial);
    if (!result.ok || !waited) {
        return fail("path tracer submission failed");
    }
    return true;
}

void PathTraceFrameRenderer::compareReference(u32 width, u32 height) {
    m_refRmse = -1.0;
    m_refBlocks = m_refFailing = 0;
    m_refWorstZ = 0.0;
    if (m_config.referenceSpp == 0u) {
        return;
    }
    if (!m_referenceValid || m_referenceKey != m_sceneKey) {
        PtSettings st;
        st.maxBounces = m_config.maxBounces;
        m_reference.resize(width, height);
        m_referenceValid = renderReference(m_compiled, st, width, height, 0x5EEDu, 0u, m_config.referenceSpp,
                                           m_reference);
        m_referenceKey = m_sceneKey;
        if (!m_referenceValid) {
            return;
        }
    }
    const float* acc = reinterpret_cast<const float*>(static_cast<const u8*>(m_gpu.mappedOutputs()) +
                                                      u64(kPtOutAccum) * m_gpu.outputStride());
    const float* sq = reinterpret_cast<const float*>(static_cast<const u8*>(m_gpu.mappedOutputs()) +
                                                     u64(kPtOutAccumSq) * m_gpu.outputStride());
    double se2 = 0.0;
    u32 count = 0;
    constexpr u32 kBlock = 8u;
    for (u32 by = 0; by < height; by += kBlock) {
        for (u32 bx = 0; bx < width; bx += kBlock) {
            for (u32 c = 0; c < 3u; ++c) {
                double mg = 0.0, mr = 0.0, vg = 0.0, vr = 0.0;
                u32 n = 0;
                for (u32 y = by; y < std::min(height, by + kBlock); ++y) {
                    for (u32 x = bx; x < std::min(width, bx + kBlock); ++x) {
                        const std::size_t i = std::size_t(y) * width + x;
                        const double ns = std::max(double(acc[i * 4u + 3u]), 1.0);
                        const double s = acc[i * 4u + c], s2 = sq[i * 4u + c];
                        const double g = s / ns;
                        const double r = m_reference.mean(x, y, c);
                        se2 += (g - r) * (g - r);
                        ++count;
                        mg += g;
                        mr += r;
                        vg += ns > 1.0 ? std::max(0.0, (s2 - s * s / ns) / (ns - 1.0)) / ns : 0.0;
                        vr += m_reference.variance(x, y, c) / double(m_config.referenceSpp);
                        ++n;
                    }
                }
                const double se = std::sqrt(vg + vr) / n;
                const double d = std::fabs(mg - mr) / n;
                m_refWorstZ = std::max(m_refWorstZ, d / std::max(se, 1e-12));
                ++m_refBlocks;
                m_refFailing += d <= 4.0 * se + 1e-4 ? 0u : 1u;
            }
        }
    }
    m_refRmse = count > 0u ? std::sqrt(se2 / count) : 0.0;
}

bool PathTraceFrameRenderer::prepare(const rf::FrameInputs& in, std::uint64_t retireSerial) {
    m_prepared = false;
    m_error.clear();
    if (m_context == nullptr || in.backBuffer == nullptr || in.draws == nullptr) {
        return fail("not attached");
    }
    const tap::HostImageInfo& bb = *in.backBuffer;
    const std::uint32_t format = bb.format;
    if (format != kR8G8B8A8Unorm && format != kR8G8B8A8Srgb && format != kB8G8R8A8Unorm && format != kB8G8R8A8Srgb) {
        return fail("back buffer format without an 8-bit RGBA / BGRA layout");
    }
    rs::BuildInputs bi;
    bi.frame = in.frame;
    bi.draws = in.draws;
    bi.count = in.count;
    bi.classifications = in.classifications;
    bi.sceneDraw = in.sceneDraw;
    bi.haveClear = in.haveClear;
    bi.clearColor = in.clearColor;
    bi.width = bb.width;
    bi.height = bb.height;
    bi.textureHandle = [](tap::ResourceId) { return 0u; };
    bi.samplerHandle = [](const tap::CaptureDrawRecord::SamplerFacts&) { return 0u; };
    bi.replacementMaterial = in.replacementMaterial;
    rs::BuildOptions opt;
    opt.features = rs::kFeatureGBuffer | rs::kFeatureClustered | rs::kFeatureForward;
    opt.fog = false;
    opt.fallbackLightMode = 0;
    if (!rs::buildRasterFrame(bi, opt, m_raster)) {
        return fail("no scene draw to render");
    }
    const std::vector<scene::LightRecord>* game = in.sceneLights == nullptr ? in.lights : nullptr;
    if (!ptSceneFromRaster(m_raster, game, in.sceneLights, m_config, m_scene, &m_stats)) {
        return fail("no path-traceable draw (or no perspective camera)");
    }
    // Keys: geometry + materials (recompile + GPU rebuild) and the whole image (accumulation).
    u64 g = 0xcbf29ce484222325ull;
    for (const PtMesh& m : m_scene.meshes) {
        g = fnv(g, m.positions.data(), m.positions.size() * sizeof(float));
        g = fnv(g, m.normals.data(), m.normals.size() * sizeof(float));
        g = fnv(g, m.colors.data(), m.colors.size() * sizeof(float));
        const u32 n = static_cast<u32>(m.positions.size());
        g = fnv(g, &n, sizeof(n));
    }
    for (const PtMaterial& m : m_scene.materials) {
        g = fnv(g, &m.bsdf, sizeof(m.bsdf));
        g = fnv(g, &m.flags, sizeof(m.flags));
        g = fnv(g, &m.alphaReference, sizeof(m.alphaReference));
        g = fnv(g, &m.alphaCompare, sizeof(m.alphaCompare));
    }
    u64 k = g;
    for (const PtInstance& i : m_scene.instances) {
        k = fnv(k, i.objectToWorld.data(), sizeof(float) * 12u);
        k = fnv(k, &i.flags, sizeof(i.flags));
    }
    k = fnv(k, m_scene.lights.data(), m_scene.lights.size() * sizeof(lk::RlLight));
    k = fnv(k, &m_scene.camera, offsetof(PtCamera, leftHanded)); // the floats (no padding)
    k = fnv(k, &bb.width, sizeof(bb.width));
    k = fnv(k, &bb.height, sizeof(bb.height));
    if (!m_compiled.valid() || g != m_geometryKey) {
        std::string error;
        if (!m_compiled.compile(m_scene, {}, &error)) {
            m_geometryKey = 0;
            m_error = "path-tracing scene: " + error;
            return false;
        }
        m_gpu.invalidateScene();
        m_restir.resetHistory();
        m_geometryKey = g;
        ++m_recompiles;
    } else if (!m_compiled.update(m_scene)) {
        return fail("path-tracing scene update");
    }
    const bool same = m_config.accumulate && k == m_sceneKey && bb.width == m_width && bb.height == m_height;
    m_accumulated = same;
    m_sampleBase = same ? m_sampleBase + m_config.samplesPerFrame : 0u;
    m_sceneKey = k;
    m_width = bb.width;
    m_height = bb.height;
    if (!trace(bb.width, bb.height)) {
        return false;
    }
    compareReference(bb.width, bb.height);
    // The present slot: the accumulated mean, sRGB-encoded in the output's byte order.
    u32 slot = kRing;
    for (u32 i = 0; i < kRing; ++i) {
        const u32 s = (m_slot + i) % kRing;
        if (m_slots[s].serial <= m_completed) {
            slot = s;
            break;
        }
    }
    if (slot == kRing) {
        return fail("every path-tracer present slot is in flight");
    }
    m_slot = (slot + 1u) % kRing;
    Slot& sl = m_slots[slot];
    const std::size_t bytes = std::size_t(bb.width) * bb.height * 4u;
    if (sl.buffer.handle == nullptr || sl.buffer.desc.size < bytes) {
        if (sl.buffer.handle != nullptr) {
            m_context->allocator().destroyBuffer(sl.buffer); // its serial completed
        }
        rr::BufferDesc d{};
        d.size = bytes;
        d.usage = rr::BufferUsage::TransferSrc;
        d.memoryUsage = rr::MemoryUsage::CpuToGpu;
        d.name = "relight.pt.present";
        if (!m_context->allocator().createBuffer(d, sl.buffer) || sl.buffer.mapped == nullptr) {
            sl.buffer = rr::Buffer{};
            return fail("path-tracer present buffer");
        }
    }
    sl.serial = retireSerial;
    const float* acc = reinterpret_cast<const float*>(static_cast<const u8*>(m_gpu.mappedOutputs()) +
                                                      u64(kPtOutAccum) * m_gpu.outputStride());
    const bool bgra = format == kB8G8R8A8Unorm || format == kB8G8R8A8Srgb;
    auto* px = static_cast<std::uint8_t*>(sl.buffer.mapped);
    const double e = m_config.exposure;
    for (std::size_t i = 0; i < std::size_t(bb.width) * bb.height; ++i) {
        const double n = std::max(double(acc[i * 4u + 3u]), 1.0);
        const std::uint8_t r = srgbByte(acc[i * 4u + 0u] / n * e), gch = srgbByte(acc[i * 4u + 1u] / n * e),
                           b = srgbByte(acc[i * 4u + 2u] / n * e);
        px[i * 4u + 0u] = bgra ? b : r;
        px[i * 4u + 1u] = gch;
        px[i * 4u + 2u] = bgra ? r : b;
        px[i * 4u + 3u] = 255u;
    }
    ++m_frames;
    m_prepared = true;
    return true;
}

bool PathTraceFrameRenderer::declare(rg::Graph& graph, rg::TextureRef output, const rf::GpuImage& outputImage) {
    if (!m_prepared || outputImage.image.info.width != m_width || outputImage.image.info.height != m_height) {
        m_error = m_prepared ? "output image size differs from the traced frame" : m_error;
        return false;
    }
    m_outputImage = outputImage.image.vkImage;
    m_presentPass =
        graph.addPass("relight.pt.present", nullptr, nullptr).use(output, rg::Access::TransferDst).neverCull().index();
    return true;
}

void PathTraceFrameRenderer::record(std::uint32_t pass, std::uint64_t commandBuffer) {
#if defined(FUSE_VULKAN_BACKEND)
    if (pass != m_presentPass || !m_prepared) {
        return;
    }
    const Slot& sl = m_slots[(m_slot + kRing - 1u) % kRing];
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_width, m_height, 1};
    vkCmdCopyBufferToImage(rf::vkHandle<VkCommandBuffer>(commandBuffer), rf::vkHandle<VkBuffer>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(sl.buffer.handle))),
                           rf::vkHandle<VkImage>(m_outputImage), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
#else
    (void)pass;
    (void)commandBuffer;
#endif
}

std::string PathTraceFrameRenderer::recordJson() const {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "\"renderer\":\"pathtrace\",\"rendered\":%s,\"error\":\"%s\",\"draws\":%u,\"meshes\":%u,"
                  "\"triangles\":%u,\"skipped\":%u,\"lights\":%u,\"fallback_light\":%s,\"spp\":%u,\"sample_base\":%u,"
                  "\"accumulated\":%u,\"reset\":%s,\"recompiles\":%u,\"scene_builds\":%u,\"ref_spp\":%u,"
                  "\"ref_rmse\":%.6g,\"ref_blocks\":%u,\"ref_failing\":%u,\"ref_worst_z\":%.3f,\"restir_di\":%s",
                  m_prepared ? "true" : "false", m_error.c_str(), m_stats.draws, m_stats.meshes, m_stats.triangles,
                  m_stats.skipped, m_stats.lights, m_stats.fallbackLight ? "true" : "false", m_config.samplesPerFrame,
                  m_sampleBase, m_sampleBase + m_config.samplesPerFrame, m_accumulated ? "false" : "true",
                  m_recompiles, m_gpu.stats().sceneBuilds, m_config.referenceSpp, m_refRmse, m_refBlocks,
                  m_refFailing, m_refWorstZ, m_rdi.enabled ? "true" : "false");
    return buf;
}

std::string PathTraceFrameRenderer::headerJson() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "\"renderer\":\"pathtrace\",\"kernel\":\"%s\",\"spp\":%u,\"max_bounces\":%u,\"accumulate\":%s,"
                  "\"ref_spp\":%u,\"error\":\"%s\"",
                  m_gpu.kernelLanguage(), m_config.samplesPerFrame, m_config.maxBounces,
                  m_config.accumulate ? "true" : "false", m_config.referenceSpp, m_error.c_str());
    return buf;
}

} // namespace

std::unique_ptr<rf::IFrameRenderer> createPathTraceRenderer(const rf::FrameConfig& config) {
    return std::make_unique<PathTraceFrameRenderer>(config);
}

} // namespace fuse::relight::render::pathtrace
