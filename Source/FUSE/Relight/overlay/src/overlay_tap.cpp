// FUSE Relight RL-6.1: the developer overlay inside the tap (see overlay_tap.hpp).
#include <fuse/relight/overlay/overlay_tap.hpp>

#include <fuse/relight/overlay/dev_menu.hpp>
#include <fuse/relight/overlay/overlay_gpu.hpp>
#include <fuse/relight/overlay/overlay_options.hpp>
#include <fuse/relight/overlay/script.hpp>
#include <fuse/relight/overlay/win32_hook.hpp>

#include <fuse/relight/render/frame/frame_options.hpp>
#include <fuse/relight/tap/capture_export_live.hpp>
#include <fuse/relight/tap/capture_tap.hpp>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <unordered_map>

namespace fuse::relight::overlay {

namespace rf = fuse::relight::render::frame;

namespace {

static_assert(static_cast<int>(DebugView::Albedo) == static_cast<int>(rf::DebugBuffer::Albedo) &&
                  static_cast<int>(DebugView::SpecularDemod) == static_cast<int>(rf::DebugBuffer::SpecularDemod),
              "DebugView follows render::frame::DebugBuffer");

/// FrameGpu (RL-4.1) as the overlay's image source; the target image is handed to the host.
class FrameGpuDevice final : public IOverlayDevice {
public:
    rf::FrameGpu* gpu = nullptr;
    tap::IFrameHost* host = nullptr;

    bool createImage(const tap::HostImageInfo& like, std::uint32_t usage, bool hostImport, rf::GpuImage& out) override {
        if (!gpu || !gpu->createImage(like, usage, true, out)) {
            return false;
        }
        if (hostImport) {
            out.host = host ? host->importImage(out.image) : 0;
            if (!out.host) {
                gpu->destroyImage(out);
                return false;
            }
        }
        return true;
    }
    void destroyImage(rf::GpuImage& image) override {
        if (image.host && host) {
            host->releaseImage(image.host);
            host->waitIdle(); // the host's recorded composite may still read it (rare: resize / detach)
        }
        image.host = 0;
        if (gpu) {
            gpu->destroyImage(image);
        }
    }
    void waitSerial(std::uint64_t serial) override {
        if (serial && gpu) {
            gpu->waitRelease(serial);
        }
    }
};

class OverlayTap final : public rf::IFrameOverlay {
public:
    explicit OverlayTap(OverlayConfig config) : m_config(std::move(config)), m_core(m_config) {
        m_textures.reserve(256);
        m_textureIndex.reserve(512);
        if (!m_config.script.empty()) {
            m_scriptLoaded = m_script.load(m_config.script);
            if (!m_scriptLoaded) {
                std::fprintf(stderr, "fuse-relight: overlay script: %s\n", m_script.error().c_str());
            }
        }
        if (!m_config.statsPath.empty()) {
            m_statsFile = std::fopen(m_config.statsPath.c_str(), "w");
        }
        m_debugAvailable.fill(false);
    }
    ~OverlayTap() override {
        detach();
        m_hook.remove();
        if (m_statsFile) {
            std::fclose(m_statsFile);
        }
    }

    void attach(rf::RenderTap& tap, const tap::DeviceEvent& e) override {
        (void)tap;
        m_host = e.host;
        const std::uint64_t window = e.present.deviceWindow ? e.present.deviceWindow : e.focusWindow;
        if (!m_hook.installed() || m_hook.window() != window) {
            m_hook.install(window, &m_core.input());
        }
        m_frameW = e.present.backBufferWidth;
        m_frameH = e.present.backBufferHeight;
        updateCoordMap();
        if (m_statsFile && !m_headerWritten) {
            m_headerWritten = true;
            std::fprintf(m_statsFile,
                         "{\"ev\":\"header\",\"schema\":\"fuse.relight.overlay/1\",\"hook\":%s,\"window\":%" PRIu64
                         ",\"script\":%s,\"script_commands\":%llu,\"script_error\":\"%s\",\"language\":\"%s\"}\n",
                         m_hook.installed() ? "true" : "false", window, m_scriptLoaded ? "true" : "false",
                         static_cast<unsigned long long>(m_script.commands().size()), m_scriptLoaded ? "" : m_script.error().c_str(),
                         OverlayGpu::defaultLanguage() == OverlayGpu::Language::Slang ? "slang" : "glsl");
            std::fflush(m_statsFile);
        }
    }

    void detach() override {
        if (m_gpu.ready() || m_gpuTried) {
            if (m_host) {
                m_host->waitIdle();
            }
            m_gpu.shutdown();
        }
        m_gpuTried = false;
        if (m_export) {
            m_export->finish();
            noteCaptureWritten();
        }
        m_host = nullptr;
        m_device = FrameGpuDevice{};
    }

    void onFrame(rf::RenderTap& tap, std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws) override {
        if (m_capture.requested && !m_export) {
            tap::CaptureExportConfig c;
            c.dir = m_config.captureDir;
            c.firstFrame = frame;
            c.frames = m_config.captureFrames;
            m_export = std::make_unique<tap::LiveCaptureExport>(tap.capture(), std::move(c));
            m_capture = CaptureStatus{};
            m_capture.active = true;
            m_capture.framesTotal = m_config.captureFrames;
        }
        if (m_export) {
            m_export->onFrame(frame, draws, true);
            ++m_capture.framesDone;
            if (m_export->written()) {
                noteCaptureWritten();
            }
        }
        if (!m_core.visible()) {
            return; // hidden: nothing to list (no allocation)
        }
        m_textures.clear();
        m_textureIndex.clear();
        for (const tap::CaptureDrawRecord& d : draws) {
            for (const tap::CaptureDrawRecord::BoundTexture& t : d.textures) {
                if (t.imageHash == 0) {
                    continue;
                }
                auto [it, inserted] = m_textureIndex.try_emplace(t.imageHash, m_textures.size());
                if (inserted) {
                    MenuTexture m;
                    m.hash = t.imageHash;
                    if (const auto tracked = tap.capture().textures().find(t.texture)) {
                        m.width = tracked->desc.width;
                        m.height = tracked->desc.height;
                        m.format = tracked->desc.format;
                    }
                    m_textures.push_back(m);
                }
                ++m_textures[it->second].uses;
            }
        }
    }

    void present(rf::RenderTap& tap, const tap::FrameEvent& f) override {
        const auto now = std::chrono::steady_clock::now();
        const double frameMs =
            m_havePresent ? std::chrono::duration<double, std::milli>(now - m_lastPresent).count() : 0.0;
        m_lastPresent = now;
        m_havePresent = true;
        if (f.width != m_frameW || f.height != m_frameH) {
            m_frameW = f.width;
            m_frameH = f.height;
            updateCoordMap();
        }
        MenuFrame mf;
        mf.stats = &m_stats;
        mf.textures = &m_textures;
        mf.debugAvailable = &m_debugAvailable;
        mf.capture = &m_capture;
        mf.captureFrames = m_config.captureFrames;
        std::size_t scriptCommands = 0;
        if (m_scriptLoaded) {
            std::size_t b = 0, e = 0;
            m_script.range(f.frame, b, e);
            if (b != e) {
                updateCoordMap();
                fillStats(tap, f, frameMs);
                scriptCommands = m_core.runScript(m_script, f.frame, m_hook, m_frameW, m_frameH, mf, &m_scriptError);
            }
        }
        m_core.update(m_frameW, m_frameH, mf, false);
        if (m_core.menu().takeCaptureRequest()) {
            m_capture = CaptureStatus{};
            m_capture.requested = true;
            m_capture.framesTotal = m_config.captureFrames;
        }
        if (!m_core.visible()) {
            writeRecord(f, scriptCommands, false);
            return;
        }
        // Shown: the menu over this frame's state, then drawn over the finished frame.
        updateCoordMap();
        fillStats(tap, f, frameMs);
        m_core.update(m_frameW, m_frameH, mf, true);
        if (m_core.menu().takeCaptureRequest()) {
            m_capture = CaptureStatus{};
            m_capture.requested = true;
            m_capture.framesTotal = m_config.captureFrames;
        }
        m_core.draw();
        draw(tap);
        writeRecord(f, scriptCommands, true);
    }

private:
    void updateCoordMap() {
        CoordMap map;
        map.frameW = static_cast<std::int32_t>(m_frameW);
        map.frameH = static_cast<std::int32_t>(m_frameH);
        m_hook.clientSize(map.clientW, map.clientH);
        m_core.input().setCoordMap(map);
    }

    void noteCaptureWritten() {
        const tap::CaptureExportResult& r = m_export->result();
        m_capture.active = false;
        m_capture.written = true;
        m_capture.ok = r.ok;
        m_capture.dir = r.dir;
        m_capture.meshes = r.meshes;
        m_capture.textures = r.textures;
        m_capture.instances = r.instances;
        m_capture.error = r.errors.empty() ? std::string() : r.errors.front();
        m_export.reset();
    }

    void fillStats(rf::RenderTap& tap, const tap::FrameEvent& f, double frameMs) {
        const rf::FrameRecord& r = tap.lastRecord();
        MenuStats s;
        s.frame = f.frame;
        s.frameMs = m_config.deterministic ? -1.0 : frameMs;
        s.mode = rf::frameModeName(tap.orchestrator().config().mode);
        s.inject = r.inject == "ui" ? "ui" : (r.inject == "present" ? "present" : (r.inject == "failed" ? "failed" : "none"));
        s.injectDraw = r.injectDraw;
        s.pass = r.result.pass;
        s.draws = r.scene.draws + r.scene.lateDraws;
        s.sceneDraws = r.scene.draws;
        s.replaced = r.scene.replaced;
        s.instances = r.scene.instances;
        s.lights = r.scene.lights;
        s.gpuInstances = r.scene.gpuInstances;
        s.bindless = r.bindless.live;
        s.swaps = r.orchestrator.swaps;
        s.renderer = tap.renderer() && tap.renderer()->attached();
        s.error = m_gpuError;
        m_stats = std::move(s);
        rf::IFrameRenderer* renderer = tap.frameRenderer();
        for (std::size_t i = 1; i < kDebugViewCount; ++i) {
            rf::DebugImage img;
            m_debugAvailable[i] = renderer && renderer->debugImage(static_cast<rf::DebugBuffer>(i), img);
        }
    }

    void draw(rf::RenderTap& tap) {
        rf::FrameOrchestrator& orch = tap.orchestrator();
        m_region = Rect{};
        if (!m_gpu.ready()) {
            if (m_gpuTried) {
                return;
            }
            m_gpuTried = true;
            if (!m_host || !tap.renderer() || !tap.renderer()->attached() || !orch.attached()) {
                m_gpuError = "overlay not drawn: the FUSE renderer is not adopted on this device";
                return;
            }
            m_device.gpu = &orch.gpu();
            m_device.host = m_host;
            const tap::VulkanDevice v = m_host->vulkan();
            if (!m_gpu.init(v.device, v.physicalDevice, m_device, OverlayGpu::defaultLanguage())) {
                m_gpuError = "overlay GPU: " + m_gpu.lastError();
                return;
            }
            m_gpuError.clear();
        }
        tap::HostImageInfo bb;
        if (!m_host->backBufferInfo(bb)) {
            m_gpuError = "overlay: no back buffer";
            return;
        }
        rf::DebugImage debug;
        const std::int32_t view = OverlayOptions::debugView();
        const bool haveDebug = view > 0 && view < static_cast<std::int32_t>(kDebugViewCount) && tap.frameRenderer() &&
                               tap.frameRenderer()->debugImage(static_cast<rf::DebugBuffer>(view), debug);
        m_debugShown = haveDebug;
        OverlayFrameDesc d;
        d.frameW = bb.width;
        d.frameH = bb.height;
        d.frameFormat = bb.format;
        d.panel = m_core.menu().panel();
        d.layer = &m_core.layer();
        d.debug = haveDebug ? &debug : nullptr;
        d.serial = orch.stats().releaseValue + 1;
        if (!m_gpu.prepare(d)) {
            m_gpuError = "overlay: " + m_gpu.lastError();
            ++m_roundFailures;
            return;
        }
        const OverlayPush& p = m_gpu.push();
        m_region = Rect{p.regionX, p.regionY, static_cast<std::int32_t>(p.regionW), static_cast<std::int32_t>(p.regionH)};
        const bool dump = !m_config.dumpPath.empty();
        if (dump) {
            orch.gpu().setDumpEnabled(true);
        }
        const rf::InjectResult r = orch.postComposite(m_gpu, m_gpu.target());
        m_gpu.submitted(r.submit.ok ? r.release : 0);
        if (!r.injected) {
            m_gpuError = "overlay round failed: " + r.error;
            ++m_roundFailures;
        } else {
            ++m_rounds;
            if (!m_gpuError.empty() && m_gpuError.rfind("overlay round failed", 0) == 0) {
                m_gpuError.clear();
            }
        }
        if (dump) {
            if (r.injected) {
                std::vector<std::uint8_t> rgba;
                std::uint32_t w = 0, h = 0;
                if (orch.gpu().readDump(r.release, rgba, w, h)) {
                    if (std::FILE* out = std::fopen(m_config.dumpPath.c_str(), "wb")) {
                        const std::uint32_t header[4] = {0x564F4C52u /* "RLOV" */, w, h, 0u};
                        std::fwrite(header, sizeof(header), 1, out);
                        std::fwrite(rgba.data(), 1, rgba.size(), out);
                        std::fclose(out);
                    }
                }
            }
            orch.gpu().setDumpEnabled(!orch.config().dumpPath.empty());
        }
    }

    void writeRecord(const tap::FrameEvent& f, std::size_t scriptCommands, bool shown) {
        if (!m_statsFile) {
            return;
        }
        const InputHookCore& in = m_core.input();
        const Rect& panel = m_core.menu().panel();
        std::fprintf(m_statsFile,
                     "{\"ev\":\"overlay\",\"frame\":%" PRIu64 ",\"visible\":%s,\"toggles\":%u,\"events\":%" PRIu64
                     ",\"forwarded\":%" PRIu64 ",\"consumed\":%" PRIu64 ",\"dropped\":%" PRIu64 ",\"rounds\":%" PRIu64
                     ",\"round_failures\":%" PRIu64 ",\"script_commands\":%llu,\"script_error\":\"%s\"",
                     f.frame, shown ? "true" : "false", m_core.togglesApplied(), m_core.events(), in.forwarded(),
                     in.consumed(), in.dropped(), m_rounds, m_roundFailures, static_cast<unsigned long long>(scriptCommands), m_scriptError.c_str());
        if (shown) {
            const std::int32_t view = OverlayOptions::debugView();
            std::fprintf(m_statsFile,
                         ",\"tab\":\"%s\",\"panel\":[%d,%d,%d,%d],\"region\":[%d,%d,%d,%d],\"scale\":%d,"
                         "\"edits\":%u,\"tags\":%u,\"saves\":%u,\"save_failures\":%u,\"debug\":\"%s\","
                         "\"debug_shown\":%s,\"capture\":{\"requested\":%s,\"active\":%s,\"written\":%s,\"ok\":%s,"
                         "\"dir\":\"%s\",\"frames\":%u},\"error\":\"%s\",\"textures\":[",
                         tabName(m_core.menu().tab()), panel.x, panel.y, panel.w, panel.h, m_region.x, m_region.y,
                         m_region.w, m_region.h, m_core.menu().scale(), m_core.menu().edits(), m_core.menu().tags(),
                         m_core.menu().saves(), m_core.menu().saveFailures(),
                         debugViewName(static_cast<DebugView>(view > 0 && view < static_cast<std::int32_t>(kDebugViewCount) ? view : 0)),
                         m_debugShown ? "true" : "false", m_capture.requested ? "true" : "false",
                         m_capture.active ? "true" : "false", m_capture.written ? "true" : "false",
                         m_capture.ok ? "true" : "false", m_capture.dir.c_str(), m_capture.framesDone,
                         m_gpuError.c_str());
            for (std::size_t i = 0; i < m_textures.size(); ++i) {
                std::fprintf(m_statsFile, "%s{\"hash\":\"%016" PRIX64 "\",\"w\":%u,\"h\":%u,\"uses\":%u}", i ? "," : "",
                             m_textures[i].hash, m_textures[i].width, m_textures[i].height, m_textures[i].uses);
            }
            std::fprintf(m_statsFile, "]");
        }
        std::fprintf(m_statsFile, "}\n");
        std::fflush(m_statsFile);
    }

    OverlayConfig m_config;
    OverlayCore m_core;
    WindowHook m_hook;
    Script m_script;
    bool m_scriptLoaded = false;
    std::string m_scriptError;
    tap::IFrameHost* m_host = nullptr;
    FrameGpuDevice m_device;
    OverlayGpu m_gpu;
    bool m_gpuTried = false;
    std::string m_gpuError;
    std::uint32_t m_frameW = 0, m_frameH = 0;
    MenuStats m_stats;
    std::vector<MenuTexture> m_textures;
    std::unordered_map<std::uint64_t, std::size_t> m_textureIndex;
    std::array<bool, kDebugViewCount> m_debugAvailable{};
    bool m_debugShown = false;
    CaptureStatus m_capture;
    std::unique_ptr<tap::LiveCaptureExport> m_export;
    std::FILE* m_statsFile = nullptr;
    bool m_headerWritten = false;
    std::chrono::steady_clock::time_point m_lastPresent{};
    bool m_havePresent = false;
    std::uint64_t m_rounds = 0, m_roundFailures = 0;
    Rect m_region;
};

} // namespace

std::unique_ptr<rf::IFrameOverlay> createFrameOverlay() {
    registerOverlayOptions();
    OverlayConfig config = OverlayConfig::fromOptions();
    if (!config.enable) {
        return nullptr;
    }
    return std::make_unique<OverlayTap>(std::move(config));
}

} // namespace fuse::relight::overlay
