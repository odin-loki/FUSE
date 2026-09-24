// FUSE Relight RL-3.5: Logic graphs inside d3d9.dll (see logic_live.hpp).
#include <fuse/relight/logic/logic_live.hpp>
#include <fuse/relight/logic/logic_log.hpp>
#include <fuse/relight/logic/logic_options.hpp>

#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>
#include <fuse/relight/replace/replace_live.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fuse::relight::logic {

namespace {

std::string genericPath(const std::filesystem::path& p) { return p.generic_string(); }

/// The root layer of a Remix mod directory (mod.usda / mod.usdc / mod.usd), "" when none.
std::string modRootLayer(const std::string& dir) {
    for (const char* name : {"mod.usda", "mod.usdc", "mod.usd"}) {
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::path(dir) / name;
        if (std::filesystem::is_regular_file(p, ec)) {
            return genericPath(p);
        }
    }
    return {};
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// The winning mesh replacement definition of a replaced draw.
const replace::MeshReplacementDef* meshDef(const replace::ReplacementEngine& engine, const replace::ReplacedDraw& rd) {
    for (const replace::ModContent* mod : engine.index().stack()) {
        if (mod == nullptr || mod->location.name != rd.meshMod) {
            continue;
        }
        for (const auto& [key, def] : mod->meshes) {
            if (def.key == rd.meshKey && def.ruleString == rd.meshRule) {
                return &def;
            }
        }
    }
    return nullptr;
}

AxisAlignedBoundingBox boundsOf(const std::array<double, 6>& b) {
    AxisAlignedBoundingBox out;
    out.minPos = Vector3(static_cast<float>(b[0]), static_cast<float>(b[1]), static_cast<float>(b[2]));
    out.maxPos = Vector3(static_cast<float>(b[3]), static_cast<float>(b[4]), static_cast<float>(b[5]));
    return out;
}

/// The owner's prim table snapshot (see logic_live.hpp step 3).
std::vector<PrimSnapshot> snapshotPrims(const ReplacementGraphs& graphs, const replace::ReplacedDraw& rd,
                                        const replace::MeshReplacementDef* def, const tap::CaptureDrawRecord& record,
                                        const replace::Mat4d& drawToWorld) {
    std::vector<PrimSnapshot> out(graphs.prims.size());
    const std::string rootPrefix = graphs.rootPath + "/";
    for (std::size_t i = 0; i < graphs.prims.size(); ++i) {
        const ReplacementGraphs::PrimEntry& entry = graphs.prims[i];
        PrimSnapshot& s = out[i];
        switch (entry.kind) {
        case ReplacementGraphs::PrimKind::OriginalMesh: {
            s.kind = PrimSnapshot::Kind::Mesh;
            s.objectToWorld = Matrix4::fromRowMajor(drawToWorld.data());
            if (record.geometry && record.geometry->captured()) {
                if (record.geometry->boundingBox.valid()) {
                    const capture::geometry::BoundingBox& b = record.geometry->boundingBox.get();
                    s.bounds.minPos = Vector3(b.minPos[0], b.minPos[1], b.minPos[2]);
                    s.bounds.maxPos = Vector3(b.maxPos[0], b.maxPos[1], b.maxPos[2]);
                }
                if (record.geometry->skinning.valid()) {
                    for (const capture::geometry::Matrix4& m : record.geometry->skinning.get().boneMatrices) {
                        s.boneMatrices.push_back(Matrix4::fromRowMajor(m.data()));
                    }
                }
            }
            break;
        }
        case ReplacementGraphs::PrimKind::Mesh:
            if (def != nullptr) {
                for (std::size_t k = 0; k < def->parts.size(); ++k) {
                    if (def->parts[k].prim != entry.path) {
                        continue;
                    }
                    s.kind = PrimSnapshot::Kind::Mesh;
                    const replace::Mat4d world = k < rd.parts.size() && rd.parts[k].meshId == def->parts[k].meshId
                                                     ? rd.parts[k].objectToWorld
                                                     : replace::multiply(def->parts[k].transform, drawToWorld);
                    s.objectToWorld = Matrix4::fromRowMajor(world.data());
                    s.bounds = boundsOf(def->parts[k].bounds);
                    break;
                }
            }
            break;
        case ReplacementGraphs::PrimKind::Light:
            if (def != nullptr && entry.path.compare(0, rootPrefix.size(), rootPrefix) == 0) {
                const std::string relative = entry.path.substr(graphs.rootPath.rfind('/') + 1);
                for (const replace::LightDef& l : def->lights) {
                    if (endsWith(l.recordId, "/" + relative)) {
                        const replace::Vec3d p =
                            replace::transformPoint(replace::multiply(l.transform, drawToWorld), replace::Vec3d{0.0, 0.0, 0.0});
                        s.kind = PrimSnapshot::Kind::Light;
                        s.lightPosition = Vector3(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
                        break;
                    }
                }
            }
            break;
        case ReplacementGraphs::PrimKind::Graph:
            s.kind = PrimSnapshot::Kind::Graph;
            break;
        }
    }
    return out;
}

} // namespace

LogicFrameProcessor::LogicFrameProcessor(std::unique_ptr<tap::IFrameProcessor> inner)
    : m_inner(std::move(inner)), m_replace(dynamic_cast<replace::CaptureReplaceProcessor*>(m_inner.get())) {
    registerLogicOptions();
    std::error_code ec;
    m_pathBase = genericPath(std::filesystem::current_path(ec));
}

LogicFrameProcessor::~LogicFrameProcessor() {
    // Graph instances hold option layer references; release them before the inner processor (and its engine) go.
    m_runtime.clear();
}

void LogicFrameProcessor::reloadGraphs() {
    std::vector<ModGraphs> mods;
    for (const replace::ModInfo& info : m_replace->engine().mods()) {
        if (!info.ok || info.location.format != replace::ModFormat::Usd) {
            continue;
        }
        const std::string root = modRootLayer(info.location.dir);
        if (root.empty()) {
            continue;
        }
        const mods::usd::ComposedStage stage = mods::usd::readStage(root);
        ModGraphs g = loadModGraphs(stage, info.location.name);
        for (const GraphDiagnostic& d : g.diagnostics) {
            std::fprintf(stderr, "fuse-relight: logic: %s: %s: %s: %s\n", info.location.name.c_str(), logSeverityName(d.severity),
                         d.path.c_str(), d.message.c_str());
        }
        if (g.graphCount() > 0) {
            std::fprintf(stderr, "fuse-relight: logic: mod %s: %zu graph(s) on %zu mesh replacement(s)\n", info.location.name.c_str(),
                         g.graphCount(), g.meshes.size());
        }
        mods.push_back(std::move(g));
    }
    m_runtime.setModGraphs(std::move(mods));
}

FrameInputs LogicFrameProcessor::gatherInputs(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws,
                                              const scene::TranslatedFrame* translatedFrame) {
    FrameInputs in;
    in.frame = frame;
    const auto now = std::chrono::steady_clock::now();
    const float fixed = LogicOptions::fixedDeltaTime();
    if (fixed > 0.0f) {
        in.deltaTime = fixed;
    } else if (m_haveLastTime) {
        in.deltaTime = std::chrono::duration<float>(now - m_lastTime).count();
    }
    m_lastTime = now;
    m_haveLastTime = true;

    const hash::HashRule assetRule = capture::geometry::GeometryCaptureConfig::fromOptions().assetRule;
    for (const tap::CaptureDrawRecord& r : draws) {
        if (!r.translated || !r.classification.committed()) {
            continue;
        }
        if (r.geometry && r.geometry->captured()) {
            if (const hash::Hash64 h = r.geometry->assetHash(assetRule)) {
                in.meshHashUsage[h]++;
            }
        }
        if (const hash::Hash64 t = r.translation.material.hash()) {
            in.textureHashUsage[t]++;
        }
    }
    if (translatedFrame != nullptr) {
        for (const scene::LightRecord& l : translatedFrame->lights) {
            in.lightHashes.insert(l.hash);
        }
        if (translatedFrame->fog.mode != 0) {
            in.fogHash = translatedFrame->fog.hash();
        }
        for (const scene::CameraState& c : translatedFrame->cameras) {
            if (c.type != scene::CameraType::Main) {
                continue;
            }
            const auto& m = c.viewToWorld;
            const auto p = c.position();
            const auto d = c.direction();
            in.camera.valid = true;
            in.camera.position = Vector3(p[0], p[1], p[2]);
            in.camera.forward = Vector3(d[0], d[1], d[2]);
            in.camera.right = Vector3(static_cast<float>(m[0]), static_cast<float>(m[1]), static_cast<float>(m[2]));
            in.camera.up = Vector3(static_cast<float>(m[4]), static_cast<float>(m[5]), static_cast<float>(m[6]));
            in.camera.fovRadians = std::fabs(c.fov);
            in.camera.aspectRatio = c.aspectRatio;
            in.camera.nearPlane = c.nearPlane;
            in.camera.farPlane = c.farPlane;
        }
    }
#if defined(_WIN32)
    if (LogicOptions::keyboard()) {
        std::set<std::uint32_t> down;
        for (int vk = 1; vk < 255; ++vk) {
            if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                down.insert(static_cast<std::uint32_t>(vk));
            }
        }
        for (std::uint32_t k : down) {
            if (m_keysDown.count(k) == 0) {
                in.keysPressed.insert(k);
            }
        }
        m_keysDown = down;
        in.keysDown = std::move(down);
    }
#endif
    return in;
}

tap::IFrameProcessor::Output LogicFrameProcessor::processFrame(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws,
                                                               const scene::TranslatedFrame* translatedFrame, bool presented) {
    Output out = m_inner ? m_inner->processFrame(frame, draws, translatedFrame, presented) : Output{};
    if (!presented || m_replace == nullptr) {
        return out;
    }
    const replace::ReplacementEngine& engine = m_replace->engine();
    if (engine.generation() != m_generation) {
        reloadGraphs();
        m_generation = engine.generation();
    }
    std::size_t graphCount = 0;
    for (const ModGraphs& g : m_runtime.modGraphs()) {
        graphCount += g.graphCount();
    }
    if (graphCount == 0 && m_runtime.manager().instances().empty()) {
        return out; // no graphs: the record and the option system are left exactly as RL-3.4 leaves them
    }
    FrameInputs inputs = gatherInputs(frame, draws, translatedFrame);

    std::vector<GraphOwnerFrame> owners;
    const std::vector<std::optional<replace::ReplacedDraw>>& replaced = m_replace->lastDraws();
    std::set<std::uint64_t> seen;
    for (std::size_t i = 0; i < replaced.size() && i < draws.size(); ++i) {
        if (!replaced[i] || !replaced[i]->meshReplaced || replaced[i]->instanceId == 0) {
            continue;
        }
        const replace::ReplacedDraw& rd = *replaced[i];
        const ReplacementGraphs* graphs = m_runtime.find(rd.meshMod, rd.meshKey);
        if (graphs == nullptr || !seen.insert(rd.instanceId).second) {
            continue;
        }
        const replace::Mat4d drawToWorld = replace::toMat4d(draws[i].translation.transforms.objectToWorld);
        inputs.prims[rd.instanceId] = snapshotPrims(*graphs, rd, meshDef(engine, rd), draws[i], drawToWorld);
        owners.push_back({rd.instanceId, graphs, rd.meshMod});
    }

    m_lastReport = m_runtime.runFrame(inputs, owners, LogicRunOptions::fromOptions());
    for (const LogMessage& m : takeLogMessages()) {
        if (m.severity != LogSeverity::Info) {
            std::fprintf(stderr, "fuse-relight: logic: %s: %s\n", logSeverityName(m.severity), m.text.c_str());
        }
    }
    const std::string logic = "\"logic\":{" + logicFrameJson(m_lastReport, m_pathBase) + "}";
    out.frame = out.frame.empty() ? logic : out.frame + "," + logic;
    return out;
}

std::unique_ptr<tap::IFrameProcessor> attachLogicProcessor(std::unique_ptr<tap::IFrameProcessor> processor) {
    registerLogicOptions();
    if (!processor || !LogicOptions::enable()) {
        return processor;
    }
    return std::make_unique<LogicFrameProcessor>(std::move(processor));
}

} // namespace fuse::relight::logic
