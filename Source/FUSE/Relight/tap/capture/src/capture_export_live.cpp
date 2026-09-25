// FUSE Relight RL-1.8 live: the capture written from inside d3d9.dll. See capture_export_live.hpp.
//
// The per-frame feeding mirrors rl_capture_export_replay's frame sink step for step (committed draws to
// SceneModel every frame; captured geometry to GameCapturer inside the window; the main camera and the
// frame's lights from TranslateTap; the colour texture and its sampler / the cull mode from the draw), so
// the live and the replayed capture of one run are equal (rl_capture_live_<app>, capture_diff).
#include <fuse/relight/tap/capture_export_live.hpp>

#include <fuse/relight/capture/export/capture_builder.hpp>
#include <fuse/relight/capture/export/capture_writer.hpp>
#include <fuse/relight/options/options.hpp>
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/scene_input.hpp>
#include <fuse/relight/scene/instances/scene_model.hpp>
#include <fuse/relight/tap/capture_tap.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fuse::relight::tap {

namespace ex = capture::exporter;
namespace inst = scene::instances;

namespace {

struct CaptureExportOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.tap", bool, captureExport, false, "FUSE_RELIGHT_TAP_CAPTURE_EXPORT",
                            "Capture mode (relight.tap.mode = capture) also writes the RL-1.8 capture: the "
                            "Remix-compatible USDA, the FUSE POCO + hash_key store and the DDS textures of the "
                            "frames relight.tap.captureExportFirstFrame .. + captureExportFrames, to "
                            "relight.tap.captureExportDir.");
    FUSE_RELIGHT_OPTION_ENV("relight.tap", std::string, captureExportDir, "relight_capture",
                            "FUSE_RELIGHT_TAP_CAPTURE_EXPORT_DIR",
                            "Output directory of the capture export (device n > 0: <dir>.<n>). A directory that "
                            "exists and is not empty is kept: the capture goes to <dir>_1, <dir>_2, ...");
    FUSE_RELIGHT_OPTION_ENV("relight.tap", std::uint32_t, captureExportFirstFrame, 0,
                            "FUSE_RELIGHT_TAP_CAPTURE_EXPORT_FIRST_FRAME",
                            "First frame of the capture export (the number of Presents before it).");
    FUSE_RELIGHT_OPTION_ENV("relight.tap", std::uint32_t, captureExportFrames, 0,
                            "FUSE_RELIGHT_TAP_CAPTURE_EXPORT_FRAMES",
                            "Number of frames of the capture export; 0: every frame until the device is "
                            "destroyed. The capture is written when the last one is presented.");
    FUSE_RELIGHT_OPTION_ENV("relight.tap", std::string, captureExportGameId, "",
                            "FUSE_RELIGHT_TAP_CAPTURE_EXPORT_GAME_ID",
                            "Remaster game id of the capture export (store namespace); empty: the executable's "
                            "name without extension.");
};

std::filesystem::path exePath() {
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) {
        return {};
    }
    return std::filesystem::path(std::wstring(buf, n));
#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p;
#endif
}

/// `dir` when it is missing or empty, else the first <dir>_<n> that is.
std::filesystem::path freshDirectory(const std::filesystem::path& dir) {
    auto usable = [](const std::filesystem::path& p) {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            return !ec;
        }
        return std::filesystem::is_directory(p, ec) && std::filesystem::is_empty(p, ec) && !ec;
    };
    if (usable(dir)) {
        return dir;
    }
    for (unsigned n = 1; n < 10000; ++n) {
        std::filesystem::path p = dir;
        p += "_" + std::to_string(n);
        if (usable(p)) {
            return p;
        }
    }
    return {};
}

} // namespace

std::string processExeName() {
    std::string name;
    try {
        name = exePath().filename().string();
    } catch (const std::exception&) { // not representable in the narrow encoding
        name.clear();
    }
    return name.empty() ? std::string("game") : name;
}

std::string processExeStem() {
    std::string stem;
    try {
        stem = exePath().stem().string();
    } catch (const std::exception&) {
        stem.clear();
    }
    return stem.empty() ? std::string("game") : stem;
}

CaptureExportConfig CaptureExportConfig::fromOptions() {
    CaptureExportConfig c;
    if (CaptureExportOptions::captureExport()) {
        c.dir = CaptureExportOptions::captureExportDir();
        if (c.dir.empty()) {
            c.dir = "relight_capture";
        }
    }
    c.firstFrame = CaptureExportOptions::captureExportFirstFrame();
    c.frames = CaptureExportOptions::captureExportFrames();
    c.gameId = CaptureExportOptions::captureExportGameId();
    return c;
}

// ---- LiveCaptureExport ----------------------------------------------------------------------------------

struct LiveCaptureExport::State {
    explicit State(ex::CaptureOptions options, ex::GameCapturer::TextureSource source)
        : capturer(std::move(options), std::move(source)) {}
    ex::GameCapturer capturer;
    inst::SceneModel model;
    hash::HashRule assetRule{};
};

LiveCaptureExport::LiveCaptureExport(CaptureTap& tap, CaptureExportConfig config)
    : m_tap(tap), m_config(std::move(config)) {
    inst::registerInstanceOptions();
    const std::string stem = processExeStem();
    const hash::HashRule assetRule = m_tap.geometry().config().assetRule;
    ex::CaptureOptions opt;
    opt.meta.gameId = m_config.gameId.empty() ? stem : m_config.gameId;
    opt.meta.windowTitle = m_config.windowTitle.empty() ? stem : m_config.windowTitle;
    opt.meta.exeName = m_config.exeName.empty() ? processExeName() : m_config.exeName;
    opt.meta.stageName = m_config.stageName.empty() ? "capture_" + stem : m_config.stageName;
    opt.assetRule = assetRule;
    opt.assetRuleString = hash::formatHashRule(assetRule);
    opt.generationRule = m_tap.geometry().config().generationRule;
    // The bytes of a hashed texture (called under the tap's lock, from captureFrame).
    CaptureTap* t = &m_tap;
    auto source = [t](hash::Hash64 h, ResourceId texture) -> std::optional<ex::CaptureTexture> {
        auto describe = [&](const capture::texture::TrackedTexture& tr) {
            ex::CaptureTexture out;
            out.hash = h;
            out.obsoleteHash = tr.obsoleteHash;
            out.descriptorHash = tr.descriptorHash;
            out.d3dFormat = tr.desc.format;
            out.width = tr.desc.width;
            out.height = tr.desc.height;
            out.mip0 = t->textures().mip0Bytes(tr.desc.id); // empty for a render target
            return out;
        };
        if (auto tr = t->textures().find(texture); tr && tr->imageHash == h) {
            return describe(*tr);
        }
        for (const capture::texture::TrackedTexture& tr : t->textures().textures()) {
            if (tr.imageHash == h) {
                return describe(tr);
            }
        }
        return std::nullopt;
    };
    m_state = std::make_unique<State>(std::move(opt), std::move(source));
    m_state->assetRule = assetRule;
}

LiveCaptureExport::~LiveCaptureExport() = default;

void LiveCaptureExport::onFrame(std::uint64_t frame, const std::vector<CaptureDrawRecord>& draws, bool presented) {
    if (!presented || m_written) {
        return;
    }
    const scene::TranslatedFrame& tf = m_tap.lastTranslatedFrame();
    std::optional<scene::CameraState> mainCamera;
    for (const scene::CameraState& c : tf.cameras) {
        if (c.type == scene::CameraType::Main) {
            mainCamera = c;
        }
    }
    ex::CaptureFrame cf;
    cf.lights = tf.lights;
    cf.mainCamera = mainCamera;
    const hash::HashRule assetRule = m_state->assetRule;
    for (const CaptureDrawRecord& r : draws) {
        if (!r.translated || !r.classification.committed()) {
            continue;
        }
        inst::SceneDrawInput input = r.geometry && r.geometry->captured()
                                         ? inst::sceneDrawInput(r.translation, *r.geometry, assetRule)
                                         : inst::sceneDrawInput(r.translation);
        input.categories = r.classification.categories; // with the geometry categories applied
        const inst::SceneDrawResult res = m_state->model.submitDraw(input);
        if (!r.geometry || !r.geometry->captured()) {
            continue;
        }
        ex::CaptureDraw d;
        d.geometry = r.geometry.get();
        d.translation = &r.translation;
        d.categories = r.classification.categories;
        d.instance = res;
        d.cullMode = r.cullMode;
        d.colorSampler.addressU = r.colorSampler.addressU;
        d.colorSampler.addressV = r.colorSampler.addressV;
        d.colorSampler.magFilter = r.colorSampler.magFilter;
        const std::int32_t slot = r.translation.material.colorTextureSlots[0];
        for (const CaptureDrawRecord::BoundTexture& bt : r.textures) {
            if (std::int32_t(bt.slot) == slot) {
                d.colorTexture = bt.texture;
            }
        }
        cf.draws.push_back(d);
    }
    const bool inWindow = frame >= m_config.firstFrame && (m_config.frames == 0 || frame < m_config.firstFrame + m_config.frames);
    if (inWindow) {
        m_state->capturer.captureFrame(cf);
    }
    m_state->model.endFrame(mainCamera ? &*mainCamera : nullptr);
    if (m_config.frames != 0 && frame + 1 >= m_config.firstFrame + m_config.frames) {
        finish();
    }
}

void LiveCaptureExport::finish() {
    if (m_written) {
        return;
    }
    m_written = true;
    const ex::CaptureData data = m_state->capturer.finish();
    CaptureExportResult& res = m_result;
    res.framesCaptured = m_state->capturer.framesCaptured();
    res.meshes = data.meshes.size();
    res.materials = data.materials.size();
    res.textures = data.textures.size();
    res.instances = data.instances.size();
    res.lights = data.sphereLights.size() + data.distantLights.size();
    const std::filesystem::path dir = freshDirectory(std::filesystem::path(m_config.dir));
    if (dir.empty()) {
        res.errors.push_back("no usable output directory for '" + m_config.dir + "'");
    } else {
        res.dir = dir.string();
        ex::CaptureWriteReport report;
        const bool ok = ex::writeCapture(dir, data, m_state->assetRule, &report);
        res.errors = report.errors;
        if (!ok) {
            res.errors.push_back("I/O error");
        }
        res.keys = report.keys.size();
    }
    res.ok = res.errors.empty();
    std::fprintf(stderr,
                 "fuse-relight: capture export: %s: %zu frame(s), %zu mesh(es), %zu material(s), %zu texture(s), "
                 "%zu instance(s), %zu light(s), %zu key(s)%s\n",
                 res.dir.empty() ? m_config.dir.c_str() : res.dir.c_str(), res.framesCaptured, res.meshes,
                 res.materials, res.textures, res.instances, res.lights, res.keys, res.ok ? "" : "; problems:");
    for (const std::string& e : res.errors) {
        std::fprintf(stderr, "fuse-relight: capture export:   %s\n", e.c_str());
    }
    // The capture data and the scene are no longer needed.
    m_state.reset();
}

} // namespace fuse::relight::tap
