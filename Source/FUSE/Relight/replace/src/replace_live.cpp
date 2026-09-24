// FUSE Relight RL-3.4: the replacement engine inside d3d9.dll (see replace_live.hpp).
#include <fuse/relight/replace/replace_live.hpp>

#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/replace/replace_json.hpp>
#include <fuse/relight/replace/replace_options.hpp>
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/scene_input.hpp>
#include <fuse/relight/scene/instances/scene_model.hpp>
#include <fuse/relight/tap/capture_export_live.hpp>

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fuse::relight::replace {

namespace inst = scene::instances;
using capture::exporter::json::Value;

struct CaptureReplaceProcessor::Scene {
    inst::SceneModel model;
};

CaptureReplaceProcessor::CaptureReplaceProcessor(EngineConfig config, hash::HashRule assetRule)
    : m_engine(std::move(config)), m_assetRule(assetRule), m_scene(std::make_unique<Scene>()) {
    inst::registerInstanceOptions();
}

CaptureReplaceProcessor::~CaptureReplaceProcessor() = default;

tap::IFrameProcessor::Output CaptureReplaceProcessor::processFrame(std::uint64_t frame,
                                                                    const std::vector<tap::CaptureDrawRecord>& draws,
                                                                    const scene::TranslatedFrame* translatedFrame,
                                                                    bool presented) {
    Output out;
    if (!presented) {
        return out; // the draws after the last Present at device destruction: not a frame
    }
    m_engine.beginFrame(frame);
    m_lastDraws.assign(draws.size(), std::nullopt);
    out.draws.resize(draws.size());
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const tap::CaptureDrawRecord& r = draws[i];
        if (!r.translated || !r.classification.committed()) {
            continue;
        }
        const bool captured = r.geometry && r.geometry->captured();
        inst::SceneDrawInput input = captured ? inst::sceneDrawInput(r.translation, *r.geometry, m_assetRule)
                                              : inst::sceneDrawInput(r.translation);
        input.categories = r.classification.categories;
        const inst::SceneDrawResult res = m_scene->model.submitDraw(input);

        DrawInput d;
        d.index = r.drawInFrame;
        d.geometryValid = captured;
        if (captured) {
            d.hashes = r.geometry->hashes.get();
            const capture::geometry::CapturedDrawPtr g = r.geometry;
            d.legacyHashes = [g] { return g->geometryHashes(); };
        }
        d.materialHash = r.translation.material.hash();
        d.categories = r.classification.categories;
        d.objectToWorld = toMat4d(r.translation.transforms.objectToWorld);
        d.instanceId = res.instanceId;
        ReplacedDraw rd = m_engine.replaceDraw(d);
        out.draws[i] = capture::exporter::json::write(replacedDrawJson(rd));
        m_lastDraws[i] = std::move(rd);
    }
    std::optional<scene::CameraState> mainCamera;
    static const std::vector<scene::LightRecord> kNoLights;
    if (translatedFrame) {
        for (const scene::CameraState& c : translatedFrame->cameras) {
            if (c.type == scene::CameraType::Main) {
                mainCamera = c;
            }
        }
    }
    m_scene->model.endFrame(mainCamera ? &*mainCamera : nullptr);
    m_lastFrame = m_engine.endFrame(translatedFrame ? translatedFrame->lights : kNoLights);
    const std::string frameJson =
        capture::exporter::json::write(replacedFrameJson(m_lastFrame, m_engine.mods(), m_engine.watchBackend()));
    out.frame = frameJson.size() > 2 ? frameJson.substr(1, frameJson.size() - 2) : std::string();
    if (m_lastFrame.reload) {
        const ReloadEvent& e = *m_lastFrame.reload;
        std::fprintf(stderr,
                     "fuse-relight: replace: generation %llu at frame %llu (%zu mod(s); %zu changed, %zu added, %zu removed, "
                     "%zu failed; invalidated %zu mesh / %zu material / %zu light key(s))\n",
                     static_cast<unsigned long long>(e.generation), static_cast<unsigned long long>(e.appliedFrame),
                     m_engine.mods().size(), e.changed.size(), e.added.size(), e.removed.size(), e.failed.size(),
                     e.invalidated.meshes.size(), e.invalidated.materials.size(), e.invalidated.lights.size());
        for (const std::string& d : m_engine.diagnostics()) {
            std::fprintf(stderr, "fuse-relight: replace:   %s\n", d.c_str());
        }
    }
    return out;
}

std::unique_ptr<tap::IFrameProcessor> createCaptureReplaceProcessor(unsigned deviceOrdinal) {
    (void)deviceOrdinal; // every device of the process sees the same mods
    registerReplaceOptions();
    if (!ReplaceOptions::enable()) {
        return nullptr;
    }
    EngineConfig config = EngineConfig::fromOptions(tap::processExeStem());
    bool anything = false;
    for (const std::vector<ModRoot>* list : {&config.roots, &config.mods}) {
        for (const ModRoot& r : *list) {
            std::error_code ec;
            anything = anything || std::filesystem::is_directory(std::filesystem::path(r.dir), ec);
        }
    }
    if (!anything) {
        return nullptr;
    }
    const hash::HashRule assetRule = capture::geometry::GeometryCaptureConfig::fromOptions().assetRule;
    return std::make_unique<CaptureReplaceProcessor>(std::move(config), assetRule);
}

} // namespace fuse::relight::replace
