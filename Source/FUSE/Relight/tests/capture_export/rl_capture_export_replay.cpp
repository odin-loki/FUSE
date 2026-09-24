// FUSE Relight RL-1.8: writes the capture of a recorded Relight run from the tap replay path.
//
//   rl_capture_export_replay --stream relight_tap.jsonl --sidecar <app>.json --out DIR
//                            [--game ID] [--first-frame F] [--frames N] [--conf rtx.conf] [--record FILE]
//
// Input is one run of an RL-0.4 app with relight.tap.mode = capture + captureRecord (as tests/instances and
// tests/tap_capture use it). The recording tap hashes content instead of dumping it, so the bytes of every
// buffer write, texture upload, UP draw and shader come back from the app sidecar's blobs, which are keyed by
// the same SHA-256 (as tests/capture_geometry and tests/capture_texture restore them). Every recorded event
// becomes the tap::IRelightTap event it was and is delivered to a CaptureTap (TextureTracker, TranslateTap,
// GeometryCapture: the live capture packages); at each Present its frame sink feeds the committed draws to
// SceneModel (RL-1.7 instances) and GameCapturer (this package), with the frame's lights and main camera.
// The capture is written to DIR/capture (capture_writer.hpp) and then checked from disk:
//   * re-ingest of the store (every record, blob, DDS, DB row) and the USDA (minimal parser, references,
//     mesh / material / instance / light structure): the key sets must match the written one;
//   * every captured texture's DDS reads back to the canonical bytes TextureTracker hashed.
// DIR/summary.json holds the counts, the key set and the check results (read by rl_capture_export.py).
// Exit: 0 written and consistent, 1 a failed check or unreadable input, 2 usage.
#include "capture_check.hpp"

#include <fuse/relight/capture/export/capture_builder.hpp>
#include <fuse/relight/capture/export/capture_writer.hpp>
#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/capture/export/usda_writer.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/scene_input.hpp>
#include <fuse/relight/tap/capture_tap.hpp>
#include <fuse/relight/tap/d3d9_names.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace fuse::relight;
namespace ex = fuse::relight::capture::exporter;
namespace inst = fuse::relight::scene::instances;
using ex::json::Value;

// ---- blobs ------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> base64Decode(const std::string& s) {
    static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : s) {
        if (c == '=') {
            break;
        }
        const std::size_t v = kAlphabet.find(c);
        if (v == std::string::npos) {
            continue;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

template <std::size_t N>
std::uint32_t valueOf(const tap::names::NameEntry (&table)[N], const std::string& name, std::uint32_t fallback) {
    for (const auto& e : table) {
        if (name == e.name) {
            return e.value;
        }
    }
    // Unnamed values are recorded as "<PREFIX>_<n>".
    const std::size_t us = name.rfind('_');
    if (us != std::string::npos && us + 1 < name.size() && name[us + 1] >= '0' && name[us + 1] <= '9') {
        return static_cast<std::uint32_t>(std::strtoul(name.c_str() + us + 1, nullptr, 10));
    }
    return fallback;
}

float at(const Value* a, std::size_t i) {
    return a && a->isArray() && i < a->a.size() && a->a[i].isNumber() ? static_cast<float>(a->a[i].n) : 0.f;
}
tap::Color4 color4(const Value* j) { return {at(j, 0), at(j, 1), at(j, 2), at(j, 3)}; }
tap::Vec3 vec3(const Value* j) { return {at(j, 0), at(j, 1), at(j, 2)}; }
tap::ResourceId idOf(const Value* v) { return v && v->isNumber() ? static_cast<tap::ResourceId>(v->n) : tap::kNoResource; }

/// A recorded state block turned back into the arrays tap::DrawState points at.
struct StateBlock {
    std::uint32_t renderStates[tap::kRenderStateCount] = {};
    std::uint32_t textureStageStates[tap::kTextureStageCount][32] = {};
    std::uint32_t samplerStates[tap::kSamplerSlotCount][tap::kSamplerStateCount] = {};
    tap::ResourceId textures[tap::kSamplerSlotCount] = {};
    std::vector<tap::Light> lights;
    tap::Material material;
    tap::Viewport viewport;
    tap::ResourceId renderTargets[tap::kRenderTargetCount] = {};
    tap::ResourceId depthStencil = tap::kNoResource;
    float clipPlanes[tap::kClipPlaneCount][4] = {};
    bool hasAlphaSwizzleMask = false;
    std::uint32_t alphaSwizzle = 0;
    bool softwareVp = false;
    // RL-1.6: the vertex shader constants (the vertexshader hash component), registers not listed = 0.
    std::vector<float> vsF;
    std::vector<std::int32_t> vsI;
    std::vector<std::uint32_t> vsB;
};

std::unique_ptr<StateBlock> parseStateBlock(const Value& st) {
    auto b = std::make_unique<StateBlock>();
    if (const Value* rs = st.get("render_states")) {
        for (const auto& kv : rs->o) {
            const std::uint32_t index = valueOf(tap::names::kRenderStates, kv.first, tap::kRenderStateCount);
            if (index < tap::kRenderStateCount) {
                b->renderStates[index] = static_cast<std::uint32_t>(kv.second.n);
            }
        }
    }
    if (const Value* stages = st.get("texture_stages")) {
        for (const Value& s : stages->a) {
            const std::uint32_t stage = s.u32("stage");
            if (const Value* states = s.get("states"); states && stage < tap::kTextureStageCount) {
                for (const auto& kv : states->o) {
                    const std::uint32_t type = valueOf(tap::names::kTextureStageStates, kv.first, 0);
                    if (type >= 1 && type <= 32) {
                        b->textureStageStates[stage][type - 1] = static_cast<std::uint32_t>(kv.second.n);
                    }
                }
            }
        }
    }
    if (const Value* samplers = st.get("samplers")) {
        for (const Value& s : samplers->a) {
            const std::uint32_t slot = tap::samplerSlot(s.u32("sampler"));
            if (const Value* states = s.get("states"); states && slot < tap::kSamplerSlotCount) {
                for (const auto& kv : states->o) {
                    const std::uint32_t type = valueOf(tap::names::kSamplerStates, kv.first, tap::kSamplerStateCount);
                    if (type < tap::kSamplerStateCount) {
                        b->samplerStates[slot][type] = static_cast<std::uint32_t>(kv.second.n);
                    }
                }
            }
        }
    }
    if (const Value* textures = st.get("textures")) {
        for (const Value& t : textures->a) {
            const std::uint32_t slot = tap::samplerSlot(t.u32("stage"));
            if (slot < tap::kSamplerSlotCount) {
                b->textures[slot] = t.u32("texture");
            }
        }
    }
    if (const Value* lights = st.get("lights")) {
        for (const Value& l : lights->a) {
            tap::Light light;
            light.index = l.u32("index");
            light.enabled = l.flag("enabled");
            light.type = valueOf(tap::names::kLightTypes, l.str("type"), 0);
            light.diffuse = color4(l.get("diffuse"));
            light.specular = color4(l.get("specular"));
            light.ambient = color4(l.get("ambient"));
            light.position = vec3(l.get("position"));
            light.direction = vec3(l.get("direction"));
            light.range = static_cast<float>(l.num("range"));
            light.falloff = static_cast<float>(l.num("falloff"));
            const Value* att = l.get("attenuation");
            light.attenuation0 = at(att, 0);
            light.attenuation1 = at(att, 1);
            light.attenuation2 = at(att, 2);
            light.theta = static_cast<float>(l.num("theta"));
            light.phi = static_cast<float>(l.num("phi"));
            b->lights.push_back(light);
        }
    }
    if (const Value* m = st.get("material"); m && m->isObject()) {
        b->material.diffuse = color4(m->get("diffuse"));
        b->material.ambient = color4(m->get("ambient"));
        b->material.specular = color4(m->get("specular"));
        b->material.emissive = color4(m->get("emissive"));
        b->material.power = static_cast<float>(m->num("power"));
    }
    if (const Value* vp = st.get("viewport")) {
        b->viewport.x = vp->u32("x");
        b->viewport.y = vp->u32("y");
        b->viewport.width = vp->u32("width");
        b->viewport.height = vp->u32("height");
        b->viewport.minZ = static_cast<float>(vp->num("min_z"));
        b->viewport.maxZ = static_cast<float>(vp->num("max_z", 1));
    }
    if (const Value* rts = st.get("render_targets")) {
        for (std::size_t i = 0; i < rts->a.size() && i < tap::kRenderTargetCount; ++i) {
            b->renderTargets[i] = idOf(&rts->a[i]);
        }
    }
    b->depthStencil = idOf(st.get("depth_stencil"));
    if (const Value* planes = st.get("clip_planes")) {
        for (std::size_t p = 0; p < planes->a.size() && p < tap::kClipPlaneCount; ++p) {
            for (std::size_t k = 0; k < 4; ++k) {
                b->clipPlanes[p][k] = at(&planes->a[p], k);
            }
        }
    }
    if (const Value* sw = st.get("alpha_swizzle_rts"); sw && sw->isNumber()) {
        b->hasAlphaSwizzleMask = true;
        b->alphaSwizzle = static_cast<std::uint32_t>(sw->n);
    }
    b->softwareVp = st.flag("software_vp");
    const std::size_t nf = b->softwareVp ? 8192 : 256, no = b->softwareVp ? 2048 : 16;
    b->vsF.assign(nf * 4, 0.0f);
    b->vsI.assign(no * 4, 0);
    b->vsB.assign((no + 31) / 32, 0u);
    if (const Value* cf = st.get("vs_const_f")) {
        for (const Value& c : cf->a) {
            const std::uint32_t r = c.u32("register");
            const Value* v = c.get("value");
            for (std::size_t k = 0; v && r < nf && k < 4 && k < v->a.size(); ++k) {
                b->vsF[r * 4 + k] = static_cast<float>(v->a[k].n);
            }
        }
    }
    if (const Value* ci = st.get("vs_const_i")) {
        for (const Value& c : ci->a) {
            const std::uint32_t r = c.u32("register");
            const Value* v = c.get("value");
            for (std::size_t k = 0; v && r < no && k < 4 && k < v->a.size(); ++k) {
                b->vsI[r * 4 + k] = static_cast<std::int32_t>(v->a[k].n);
            }
        }
    }
    if (const Value* cb = st.get("vs_const_b")) {
        for (const Value& c : cb->a) {
            const std::uint32_t r = c.u32("register");
            if (r < no && c.flag("value")) {
                b->vsB[r / 32] |= 1u << (r % 32);
            }
        }
    }
    return b;
}

std::uint32_t transformSlot(const std::string& name) {
    if (name == "VIEW") {
        return tap::kTransformView;
    }
    if (name == "PROJECTION") {
        return tap::kTransformProjection;
    }
    if (name == "WORLD") {
        return tap::kTransformWorld0;
    }
    if (name.rfind("TEXTURE", 0) == 0) {
        return tap::kTransformTexture0 + static_cast<std::uint32_t>(std::atoi(name.c_str() + 7));
    }
    if (name.rfind("WORLDMATRIX_", 0) == 0) {
        return tap::kTransformWorld0 + static_cast<std::uint32_t>(std::atoi(name.c_str() + 12));
    }
    return tap::kTransformCount;
}

/// "vs_2_0" / "ps_1_4" -> D3DVS_VERSION / D3DPS_VERSION token.
std::uint32_t shaderVersion(const std::string& v) {
    if (v.size() < 6) {
        return 0;
    }
    const std::uint32_t major = static_cast<std::uint32_t>(std::atoi(v.c_str() + 3));
    const std::uint32_t minor = static_cast<std::uint32_t>(std::atoi(v.c_str() + 5));
    return (v[0] == 'p' ? 0xFFFF0000u : 0xFFFE0000u) | (major << 8) | minor;
}

/// Fake, distinct VkImage handles for the textures that have one.
std::uint64_t fakeImage(tap::ResourceId id) { return 0x100000ull + id; }

struct Counters {
    std::size_t lines = 0, draws = 0, frames = 0;
    std::size_t bufferWrites = 0, bufferWritesMissing = 0;
    std::size_t uploads = 0, uploadsMissing = 0;
    std::size_t upMissing = 0, shadersMissing = 0;
};

int usage() {
    std::fprintf(stderr, "usage: rl_capture_export_replay --stream <relight_tap.jsonl> --sidecar <app.json> --out <dir> "
                         "[--game <id>] [--first-frame <n>] [--frames <n>] [--conf <rtx.conf>] [--record <file>]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string streamPath, sidecarPath, outDir, confPath, recordPath, gameId;
    std::uint64_t firstFrame = 0;
    std::uint64_t maxFrames = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const std::string v = argv[i + 1];
        if (k == "--stream") {
            streamPath = v;
        } else if (k == "--sidecar") {
            sidecarPath = v;
        } else if (k == "--out") {
            outDir = v;
        } else if (k == "--conf") {
            confPath = v;
        } else if (k == "--record") {
            recordPath = v;
        } else if (k == "--game") {
            gameId = v;
        } else if (k == "--first-frame") {
            firstFrame = std::strtoull(v.c_str(), nullptr, 10);
        } else if (k == "--frames") {
            maxFrames = std::strtoull(v.c_str(), nullptr, 10);
        } else {
            return usage();
        }
    }
    if (streamPath.empty() || sidecarPath.empty() || outDir.empty()) {
        return usage();
    }

    // Options: no dxvk.conf / rtx.conf from the environment; an explicit --conf layer.
    options::setEnvironmentVariable(options::kDxvkConfEnvVar, "");
    options::setEnvironmentVariable(options::kRtxConfEnvVar, "");
    inst::registerInstanceOptions();
    options::OptionLayerHandle layer;
    if (!confPath.empty()) {
        bool found = false;
        const options::OptionConfig config = options::OptionConfig::loadFile(confPath, {}, nullptr, &found);
        if (!found) {
            std::fprintf(stderr, "rl_capture_export_replay: cannot read %s\n", confPath.c_str());
            return 1;
        }
        layer = options::OptionManager::acquireLayer("", {10000u, "rl_capture_export_replay"}, 1.0f, 0.1f, false, &config);
    }
    options::OptionManager::applyPendingValues(nullptr, false);

    // The sidecar's blobs, by SHA-256.
    std::map<std::string, std::vector<std::uint8_t>> blobs;
    std::string appName = "app";
    {
        std::string text;
        if (!ex::readFile(sidecarPath, text)) {
            std::fprintf(stderr, "rl_capture_export_replay: cannot read %s\n", sidecarPath.c_str());
            return 1;
        }
        std::string err;
        const auto sidecar = ex::json::parse(text, &err);
        if (!sidecar) {
            std::fprintf(stderr, "rl_capture_export_replay: %s: %s\n", sidecarPath.c_str(), err.c_str());
            return 1;
        }
        appName = sidecar->str("app", "app");
        if (const Value* b = sidecar->get("blobs")) {
            for (const auto& kv : b->o) {
                blobs.emplace(kv.first, base64Decode(kv.second.s));
            }
        }
    }
    if (gameId.empty()) {
        gameId = appName;
    }
    auto blob = [&](const Value* sha) -> const std::vector<std::uint8_t>* {
        if (!sha || !sha->isString()) {
            return nullptr;
        }
        const auto it = blobs.find(sha->s);
        return it == blobs.end() ? nullptr : &it->second;
    };

    // The capture packages, live: CaptureTap keeps the canonical mip 0 of every texture for the DDS files.
    tap::CaptureTapConfig config = tap::CaptureTapConfig::fromOptions();
    config.path = recordPath;
    config.texture.retainShadowAfterHash = true;
    config.geometry.asyncJobs = false; // deterministic, and nothing to overlap with here
    const hash::HashRule assetRule = config.geometry.assetRule;
    tap::CaptureTap capture(std::move(config));

    ex::CaptureOptions opt;
    opt.meta.gameId = gameId;
    opt.meta.windowTitle = appName;
    opt.meta.exeName = appName + ".exe";
    opt.meta.stageName = "capture_" + appName;
    opt.assetRule = assetRule;
    opt.assetRuleString = hash::formatHashRule(assetRule);
    opt.generationRule = capture.geometry().config().generationRule;
    ex::GameCapturer capturer(opt, [&capture](hash::Hash64 h, tap::ResourceId texture) -> std::optional<ex::CaptureTexture> {
        auto describe = [&](const capture::texture::TrackedTexture& t) -> std::optional<ex::CaptureTexture> {
            ex::CaptureTexture out;
            out.hash = h;
            out.obsoleteHash = t.obsoleteHash;
            out.descriptorHash = t.descriptorHash;
            out.d3dFormat = t.desc.format;
            out.width = t.desc.width;
            out.height = t.desc.height;
            out.mip0 = capture.textures().mip0Bytes(t.desc.id); // empty for a render target
            return out;
        };
        if (auto t = capture.textures().find(texture); t && t->imageHash == h) {
            return describe(*t);
        }
        for (const auto& t : capture.textures().textures()) {
            if (t.imageHash == h) {
                return describe(t);
            }
        }
        return std::nullopt;
    });
    inst::SceneModel model;

    // Per draw (frame, draw in frame): what the capture needs from the draw state besides the records.
    struct DrawExtra {
        ex::SamplerState samplers[tap::kSamplerSlotCount];
        std::uint32_t cullMode = 2;
    };
    std::map<std::pair<std::uint64_t, std::uint32_t>, DrawExtra> extras;
    bool destroying = false;
    std::size_t framesCaptured = 0, committedDraws = 0, instanceDraws = 0;
    Value drawLog = Value::array(); // per captured draw: frame, di, instance, mesh key (for the driver's checks)
    capture.setFrameSink([&](std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws) {
        if (destroying) {
            return; // draws after the last Present: not a presented frame
        }
        const scene::TranslatedFrame& tf = capture.lastTranslatedFrame();
        std::optional<scene::CameraState> mainCamera;
        for (const scene::CameraState& c : tf.cameras) {
            if (c.type == scene::CameraType::Main) {
                mainCamera = c;
            }
        }
        ex::CaptureFrame cf;
        std::vector<std::uint32_t> drawIndices; // draw in frame of each cf.draws entry
        cf.lights = tf.lights;
        cf.mainCamera = mainCamera;
        for (const tap::CaptureDrawRecord& r : draws) {
            if (!r.translated || !r.classification.committed()) {
                continue;
            }
            ++committedDraws;
            inst::SceneDrawInput input = inst::sceneDrawInput(r.translation);
            input.categories = r.classification.categories; // with the geometry categories applied
            if (r.geometry && r.geometry->captured()) {
                input = inst::sceneDrawInput(r.translation, *r.geometry, assetRule);
                input.categories = r.classification.categories;
            }
            const inst::SceneDrawResult res = model.submitDraw(input);
            if (!r.geometry || !r.geometry->captured()) {
                continue;
            }
            ex::CaptureDraw d;
            d.geometry = r.geometry.get();
            d.translation = &r.translation;
            d.categories = r.classification.categories;
            d.instance = res;
            const auto ext = extras.find({frame, r.drawInFrame});
            const std::int32_t slot = r.translation.material.colorTextureSlots[0];
            if (ext != extras.end()) {
                d.cullMode = ext->second.cullMode;
                if (slot >= 0 && slot < std::int32_t(tap::kSamplerSlotCount)) {
                    d.colorSampler = ext->second.samplers[slot];
                }
            }
            for (const tap::CaptureDrawRecord::BoundTexture& t : r.textures) {
                if (std::int32_t(t.slot) == slot) {
                    d.colorTexture = t.texture;
                }
            }
            cf.draws.push_back(d);
            drawIndices.push_back(r.drawInFrame);
            ++instanceDraws;
        }
        const scene::CameraState* cam = mainCamera ? &*mainCamera : nullptr;
        if (frame >= firstFrame && (maxFrames == 0 || frame < firstFrame + maxFrames)) {
            capturer.captureFrame(cf);
            ++framesCaptured;
            for (std::size_t k = 0; k < cf.draws.size(); ++k) {
                const ex::CaptureDraw& d = cf.draws[k];
                Value row = Value::object();
                row["frame"] = Value::number(double(frame));
                row["di"] = Value::number(double(drawIndices[k]));
                row["instance"] = Value::number(double(d.instance.instanceId));
                row["key"] = Value::string(hash::hashToString(d.geometry->assetHash(assetRule)));
                row["material"] = Value::string(hash::hashToString(d.translation->material.hash()));
                drawLog.push(std::move(row));
            }
        }
        model.endFrame(cam);
        for (auto it = extras.begin(); it != extras.end();) {
            it = it->first.first <= frame ? extras.erase(it) : std::next(it);
        }
    });

    // ---- replay -------------------------------------------------------------------------------------------
    std::ifstream in(streamPath, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "rl_capture_export_replay: cannot read %s\n", streamPath.c_str());
        return 1;
    }
    Counters n;
    std::vector<std::unique_ptr<StateBlock>> blocks;
    std::map<tap::ResourceId, std::vector<std::uint8_t>> buffers; // restored buffer contents
    std::vector<float> identity(tap::kTransformCount * 16, 0.0f);
    for (std::uint32_t t = 0; t < tap::kTransformCount; ++t) {
        for (std::uint32_t k = 0; k < 4; ++k) {
            identity[t * 16 + k * 5] = 1.0f;
        }
    }
    std::vector<float> transforms;
    std::uint32_t drawInFrame = 0;
    std::uint64_t currentFrame = 0;
    for (std::string line; std::getline(in, line);) {
        ++n.lines;
        if (line.empty()) {
            continue;
        }
        std::string err;
        const auto evOpt = ex::json::parse(line, &err);
        if (!evOpt || !evOpt->isObject()) {
            std::fprintf(stderr, "rl_capture_export_replay: %s:%zu: %s\n", streamPath.c_str(), n.lines, err.c_str());
            return 1;
        }
        const Value& ev = *evOpt;
        const std::string type = ev.str("ev");
        if (type == "device_create" || type == "device_reset") {
            tap::DeviceEvent e;
            e.adapter = ev.u32("adapter");
            e.deviceType = ev.u32("device_type");
            e.behaviorFlags = ev.u32("behavior_flags");
            e.extended = ev.flag("extended");
            e.d3d8 = ev.flag("d3d8");
            if (const Value* p = ev.get("present")) {
                e.present.backBufferWidth = p->u32("back_buffer_width");
                e.present.backBufferHeight = p->u32("back_buffer_height");
                e.present.backBufferFormat = p->u32("back_buffer_format");
                e.present.backBufferCount = p->u32("back_buffer_count");
                e.present.multiSampleType = p->u32("multisample");
                e.present.swapEffect = p->u32("swap_effect");
                e.present.windowed = p->flag("windowed");
                e.present.enableAutoDepthStencil = p->flag("auto_depth_stencil");
                e.present.autoDepthStencilFormat = p->u32("auto_depth_stencil_format");
                e.present.flags = p->u32("flags");
                e.present.presentationInterval = p->u32("presentation_interval");
            }
            e.backBuffer = idOf(ev.get("back_buffer"));
            e.autoDepthStencil = idOf(ev.get("auto_depth_stencil"));
            type == "device_create" ? capture.onDeviceCreate(e) : capture.onDeviceReset(e);
        } else if (type == "device_destroy") {
            destroying = true;
            capture.onDeviceDestroy();
        } else if (type == "texture_create") {
            tap::TextureDesc d;
            d.id = ev.u32("id");
            d.type = ev.u32("type");
            d.width = ev.u32("width");
            d.height = ev.u32("height");
            d.depth = ev.u32("depth");
            d.mipLevels = ev.u32("levels");
            d.arraySize = ev.u32("array_size");
            d.format = ev.u32("format");
            d.usage = ev.u32("usage");
            d.pool = valueOf(tap::names::kPools, ev.str("pool"), 0);
            d.multiSample = ev.u32("multisample");
            d.isBackBuffer = ev.flag("back_buffer");
            d.isAttachmentOnly = ev.flag("attachment_only");
            d.vkImage = ev.flag("has_image") ? fakeImage(d.id) : 0;
            capture.onTextureCreate(d);
        } else if (type == "texture_upload") {
            ++n.uploads;
            tap::TextureUpload u;
            u.texture = ev.u32("texture");
            u.face = ev.u32("face");
            u.level = ev.u32("level");
            u.width = ev.u32("width");
            u.height = ev.u32("height");
            u.depth = 1;
            u.rowPitch = ev.u32("row_pitch");
            u.rows = ev.u32("rows");
            u.slicePitch = u.rowPitch * u.rows;
            u.lockFlags = ev.u32("lock_flags");
            u.fullUpdate = ev.flag("full");
            u.box = {0, 0, u.width, u.height, 0, 1};
            const std::vector<std::uint8_t>* bytes = blob(ev.get("blob"));
            if (bytes && bytes->size() >= std::size_t(u.rowPitch) * u.rows) {
                u.data = bytes->data();
            } else if (ev.get("blob") && ev.get("blob")->isString()) {
                ++n.uploadsMissing;
            }
            capture.onTextureUpload(u);
        } else if (type == "texture_copy") {
            tap::TextureCopy c;
            c.method = ev.str("method") == "UpdateSurface" ? tap::CopyMethod::UpdateSurface : tap::CopyMethod::UpdateTexture;
            c.source = ev.u32("source");
            c.destination = ev.u32("destination");
            c.sourceFace = ev.u32("source_face");
            c.sourceLevel = ev.u32("source_level");
            c.destFace = ev.u32("dest_face");
            c.destLevel = ev.u32("dest_level");
            c.hasSourceRect = ev.flag("has_rect");
            c.width = ev.u32("width");
            c.height = ev.u32("height");
            c.destX = ev.u32("dest_x");
            c.destY = ev.u32("dest_y");
            capture.onTextureCopy(c);
        } else if (type == "texture_write_lock") {
            tap::TextureWriteLock l;
            l.texture = ev.u32("texture");
            l.face = ev.u32("face");
            l.level = ev.u32("level");
            l.lockFlags = ev.u32("lock_flags");
            capture.onTextureWriteLock(l);
        } else if (type == "image_destroy") {
            tap::ImageDestroy d;
            d.texture = ev.u32("texture");
            d.vkImage = fakeImage(d.texture);
            capture.onImageDestroy(d);
        } else if (type == "buffer_create") {
            tap::BufferDesc d;
            d.id = ev.u32("id");
            d.kind = ev.str("kind") == "index" ? tap::BufferKind::Index : tap::BufferKind::Vertex;
            d.size = ev.u32("size");
            d.usage = ev.u32("usage");
            d.pool = valueOf(tap::names::kPools, ev.str("pool"), 0);
            d.fvf = ev.u32("fvf");
            d.format = d.kind == tap::BufferKind::Index ? valueOf(tap::names::kIndexFormats, ev.str("format"), 0) : 0;
            buffers[d.id].assign(d.size, 0);
            capture.onBufferCreate(d);
        } else if (type == "buffer_write") {
            ++n.bufferWrites;
            tap::BufferWrite w;
            w.buffer = ev.u32("buffer");
            w.offset = ev.u32("offset");
            w.size = ev.u32("size");
            w.lockFlags = ev.u32("lock_flags");
            std::vector<std::uint8_t>& shadow = buffers[w.buffer];
            // The blob is the whole buffer after the write (the sidecar's buffer version).
            if (const std::vector<std::uint8_t>* bytes = blob(ev.get("blob")); bytes && bytes->size() == shadow.size()) {
                shadow = *bytes;
            } else {
                ++n.bufferWritesMissing;
            }
            w.base = shadow.data();
            w.bufferSize = static_cast<std::uint32_t>(shadow.size());
            w.data = w.offset < shadow.size() ? shadow.data() + w.offset : nullptr;
            capture.onBufferWrite(w);
        } else if (type == "buffer_destroy") {
            const tap::ResourceId id = ev.u32("buffer");
            capture.onBufferDestroy(id);
            buffers.erase(id);
        } else if (type == "shader") {
            // RL-1.6: the shader's bytecode (a D3D8 app's shaders are not in its sidecar: d3d8 translates them).
            const std::string data = ev.str("data");
            std::vector<std::uint8_t> bytes(data.size() / 2);
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<std::uint8_t>(std::stoul(data.substr(2 * i, 2), nullptr, 16));
            }
            blobs.emplace(ev.str("blob"), std::move(bytes));
        } else if (type == "state_block") {
            const std::size_t index = static_cast<std::size_t>(ev.num("index"));
            if (blocks.size() <= index) {
                blocks.resize(index + 1);
            }
            if (const Value* st = ev.get("state")) {
                blocks[index] = parseStateBlock(*st);
            }
        } else if (type == "draw") {
            ++n.draws;
            const std::size_t index = static_cast<std::size_t>(ev.num("state"));
            if (index >= blocks.size() || !blocks[index]) {
                std::fprintf(stderr, "rl_capture_export_replay: %s:%zu: unknown state block %zu\n", streamPath.c_str(), n.lines, index);
                return 1;
            }
            const StateBlock& blk = *blocks[index];
            tap::DrawCall call;
            const std::string callName = ev.str("call");
            call.call = callName == "DrawIndexedPrimitive"     ? tap::DrawCallType::DrawIndexedPrimitive
                        : callName == "DrawPrimitiveUP"        ? tap::DrawCallType::DrawPrimitiveUP
                        : callName == "DrawIndexedPrimitiveUP" ? tap::DrawCallType::DrawIndexedPrimitiveUP
                                                               : tap::DrawCallType::DrawPrimitive;
            call.primitiveType = valueOf(tap::names::kPrimitiveTypes, ev.str("primitive"), 0);
            call.primitiveCount = ev.u32("prim_count");
            call.startVertex = ev.u32("start_vertex");
            call.vertexCount = ev.u32("vertex_count");
            call.baseVertex = static_cast<std::int32_t>(ev.num("base_vertex"));
            call.minIndex = ev.u32("min_index");
            call.numVertices = ev.u32("num_vertices");
            call.startIndex = ev.u32("start_index");
            call.indexCount = ev.u32("index_count");
            call.instanceCount = ev.u32("instance_count", 1);
            if (const Value* up = ev.get("up"); up && up->isObject()) {
                call.upVertexStride = up->u32("vertex_stride");
                if (const auto* v = blob(up->get("vertex_blob"))) {
                    call.upVertexData = v->data();
                    call.upVertexBytes = static_cast<std::uint32_t>(v->size());
                } else {
                    ++n.upMissing;
                }
                if (const auto* ib = blob(up->get("index_blob"))) {
                    call.upIndexData = ib->data();
                    call.upIndexBytes = static_cast<std::uint32_t>(ib->size());
                    call.upIndexFormat = valueOf(tap::names::kIndexFormats, up->str("index_format"), 0);
                }
            }

            tap::DrawState s;
            s.renderStates = blk.renderStates;
            s.textureStageStates = blk.textureStageStates;
            s.samplerStates = blk.samplerStates;
            std::memcpy(s.textures, blk.textures, sizeof s.textures);
            std::memcpy(s.renderTargets, blk.renderTargets, sizeof s.renderTargets);
            s.depthStencil = blk.depthStencil;
            s.viewport = blk.viewport;
            s.lights = blk.lights.empty() ? nullptr : blk.lights.data();
            s.lightCount = static_cast<std::uint32_t>(blk.lights.size());
            s.material = blk.material;
            s.clipPlanes = blk.clipPlanes;
            s.hasAlphaSwizzleMask = blk.hasAlphaSwizzleMask;
            s.alphaSwizzleRenderTargets = blk.alphaSwizzle;
            s.softwareVertexProcessing = blk.softwareVp;
            s.vsConstF = reinterpret_cast<const float(*)[4]>(blk.vsF.data());
            s.vsConstFCount = static_cast<std::uint32_t>(blk.vsF.size() / 4);
            s.vsConstI = reinterpret_cast<const std::int32_t(*)[4]>(blk.vsI.data());
            s.vsConstICount = static_cast<std::uint32_t>(blk.vsI.size() / 4);
            s.vsConstB = blk.vsB.data();
            s.vsConstBCount = blk.softwareVp ? 2048u : 16u;
            s.lightsVersion = ev.u32("lights_version");
            s.clipPlanesVersion = ev.u32("clip_planes_version");
            if (const Value* elems = ev.get("elements")) {
                for (const Value& e : elems->a) {
                    if (s.elementCount >= tap::kMaxVertexElements) {
                        break;
                    }
                    tap::VertexElement& ve = s.elements[s.elementCount++];
                    ve.stream = static_cast<std::uint16_t>(e.u32("stream"));
                    ve.offset = static_cast<std::uint16_t>(e.u32("offset"));
                    ve.type = static_cast<std::uint8_t>(valueOf(tap::names::kDeclTypes, e.str("type"), 17));
                    ve.method = static_cast<std::uint8_t>(e.u32("method"));
                    ve.usage = static_cast<std::uint8_t>(valueOf(tap::names::kDeclUsages, e.str("usage"), 0));
                    ve.usageIndex = static_cast<std::uint8_t>(e.u32("usage_index"));
                }
            }
            if (const Value* fvf = ev.get("fvf"); fvf && fvf->isNumber()) {
                s.fvf = static_cast<std::uint32_t>(fvf->n);
            }
            if (const Value* streams = ev.get("streams")) {
                for (const Value& b : streams->a) {
                    const std::uint32_t k = b.u32("stream");
                    if (k >= tap::kStreamCount) {
                        continue;
                    }
                    tap::StreamBinding& sb = s.streams[k];
                    sb.buffer = b.u32("buffer");
                    sb.offset = b.u32("offset");
                    sb.stride = b.u32("stride");
                    sb.frequency = b.u32("frequency");
                    if (auto it = buffers.find(sb.buffer); it != buffers.end()) {
                        sb.base = it->second.data();
                        sb.bufferSize = static_cast<std::uint32_t>(it->second.size());
                    }
                }
            }
            if (const Value* ib = ev.get("index_buffer"); ib && ib->isObject()) {
                s.indices.buffer = ib->u32("buffer");
                s.indices.format = valueOf(tap::names::kIndexFormats, ib->str("format"), 0);
                if (auto it = buffers.find(s.indices.buffer); it != buffers.end()) {
                    s.indices.base = it->second.data();
                    s.indices.bufferSize = static_cast<std::uint32_t>(it->second.size());
                }
            }
            transforms = identity;
            if (const Value* t = ev.get("transforms")) {
                for (const auto& kv : t->o) {
                    const std::uint32_t slot = transformSlot(kv.first);
                    for (std::size_t k = 0; slot < tap::kTransformCount && k < 16 && k < kv.second.a.size(); ++k) {
                        transforms[slot * 16 + k] = static_cast<float>(kv.second.a[k].n);
                    }
                }
            }
            s.transforms = reinterpret_cast<const float(*)[16]>(transforms.data());
            auto shader = [&](const Value* v, tap::ShaderRef& ref) {
                if (!v || !v->isObject()) {
                    return;
                }
                ref.id = v->u32("id");
                ref.version = shaderVersion(v->str("version"));
                ref.byteSize = v->u32("byte_size");
                if (const auto* bytes = blob(v->get("blob")); bytes && bytes->size() >= ref.byteSize) {
                    ref.tokens = reinterpret_cast<const std::uint32_t*>(bytes->data());
                } else {
                    ++n.shadersMissing;
                    ref.byteSize = 0;
                }
            };
            shader(ev.get("vertex_shader"), s.vertexShader);
            shader(ev.get("pixel_shader"), s.pixelShader);

            currentFrame = static_cast<std::uint64_t>(ev.num("frame"));
            DrawExtra& x = extras[{currentFrame, drawInFrame}];
            x.cullMode = blk.renderStates[22]; // D3DRS_CULLMODE
            for (std::uint32_t slot = 0; slot < tap::kSamplerSlotCount; ++slot) {
                x.samplers[slot].addressU = blk.samplerStates[slot][1]; // D3DSAMP_ADDRESSU
                x.samplers[slot].addressV = blk.samplerStates[slot][2]; // D3DSAMP_ADDRESSV
                x.samplers[slot].magFilter = blk.samplerStates[slot][5]; // D3DSAMP_MAGFILTER
            }
            capture.onDraw(call, s);
            ++drawInFrame;
        } else if (type == "query_begin" || type == "query_end") {
            tap::QueryEvent q;
            q.type = ev.u32("type");
            type == "query_begin" ? capture.onQueryBegin(q) : capture.onQueryEnd(q);
        } else if (type == "clear") {
            tap::ClearEvent c;
            c.rectCount = ev.u32("rect_count");
            c.flags = ev.u32("flags");
            c.color = static_cast<std::uint32_t>(std::strtoul(ev.str("color").c_str(), nullptr, 16));
            c.z = static_cast<float>(ev.num("z"));
            c.stencil = ev.u32("stencil");
            if (const Value* rts = ev.get("render_targets")) {
                for (std::size_t i = 0; i < rts->a.size() && i < tap::kRenderTargetCount; ++i) {
                    c.renderTargets[i] = idOf(&rts->a[i]);
                }
            }
            c.depthStencil = idOf(ev.get("depth_stencil"));
            capture.onClear(c);
        } else if (type == "set_render_target") {
            tap::SetRenderTargetEvent e;
            e.index = ev.u32("index");
            e.texture = idOf(ev.get("texture"));
            capture.onSetRenderTarget(e);
        } else if (type == "inject_point") {
            tap::FrameEvent f;
            f.frame = static_cast<std::uint64_t>(ev.num("frame"));
            f.backBuffer = ev.u32("back_buffer");
            capture.onInjectPoint(f);
        } else if (type == "present") {
            tap::FrameEvent f;
            f.frame = static_cast<std::uint64_t>(ev.num("frame"));
            f.backBuffer = ev.u32("back_buffer");
            f.backBufferVkImage = ev.flag("has_image") ? fakeImage(f.backBuffer) : 0;
            f.width = ev.u32("width");
            f.height = ev.u32("height");
            f.format = ev.u32("format");
            capture.onPresent(f);
            drawInFrame = 0;
            ++n.frames;
        }
    }
    if (!destroying) {
        destroying = true;
        capture.onDeviceDestroy();
    }
    layer = options::OptionLayerHandle();
    options::OptionManager::applyPendingValues(nullptr, false);

    // ---- write and check ----------------------------------------------------------------------------------
    const ex::CaptureData data = capturer.finish();
    const std::filesystem::path out(outDir);
    const std::filesystem::path capDir = out / "capture";
    std::error_code ec;
    std::filesystem::remove_all(capDir, ec);
    ex::CaptureWriteReport report;
    const bool written = ex::writeCapture(capDir, data, assetRule, &report);
    const std::string stage = ex::captureStageFileName(data.meta);
    const rl_capture_check::Result check = rl_capture_check::checkCapture(capDir, stage, opt.assetRuleString);

    std::vector<std::string> failures = check.errors;
    for (const std::string& e : report.errors) {
        failures.push_back("writer: " + e);
    }
    if (!written) {
        failures.push_back("writer: I/O error");
    }
    if (check.storeKeys != report.keys) {
        failures.push_back("re-ingest: the store's key set differs from the written one");
    }
    if (report.keys != ex::captureKeys(data, assetRule)) {
        failures.push_back("writer: the written key set differs from the capture's");
    }
    // DDS files hold exactly the bytes TextureTracker hashed.
    std::size_t ddsChecked = 0;
    for (const auto& [h, tex] : data.textures) {
        std::vector<std::uint8_t> bytes;
        if (!ex::readFile(capDir / ex::captureTexturePath(h), bytes)) {
            continue; // reported by the writer (no DDS form)
        }
        const auto img = ex::readDds(bytes);
        if (!img || img->mips.empty() || img->mips[0] != tex.mip0) {
            failures.push_back("dds " + hash::hashToString(h) + ": does not read back to the captured canonical bytes");
        }
        ++ddsChecked;
    }

    // summary.json
    Value sum = Value::object();
    sum["app"] = Value::string(appName);
    sum["stage"] = Value::string(stage);
    sum["asset_rule"] = Value::string(opt.assetRuleString);
    Value counts = Value::object();
    counts["stream_lines"] = Value::number(double(n.lines));
    counts["draws"] = Value::number(double(n.draws));
    counts["frames"] = Value::number(double(n.frames));
    counts["frames_captured"] = Value::number(double(framesCaptured));
    counts["committed_draws"] = Value::number(double(committedDraws));
    counts["instance_draws"] = Value::number(double(instanceDraws));
    counts["buffer_writes"] = Value::number(double(n.bufferWrites));
    counts["buffer_writes_unrestored"] = Value::number(double(n.bufferWritesMissing));
    counts["uploads"] = Value::number(double(n.uploads));
    counts["uploads_unrestored"] = Value::number(double(n.uploadsMissing));
    counts["up_unrestored"] = Value::number(double(n.upMissing));
    counts["meshes"] = Value::number(double(data.meshes.size()));
    counts["materials"] = Value::number(double(data.materials.size()));
    counts["textures"] = Value::number(double(data.textures.size()));
    counts["dds_checked"] = Value::number(double(ddsChecked));
    counts["instances"] = Value::number(double(data.instances.size()));
    counts["sphere_lights"] = Value::number(double(data.sphereLights.size()));
    counts["distant_lights"] = Value::number(double(data.distantLights.size()));
    counts["camera"] = Value::boolean(data.camera.valid);
    counts["usda_layers"] = Value::number(double(check.layers));
    counts["textures_without_bytes"] = Value::number(double(capturer.stats().texturesMissing));
    counts["mesh_updates"] = Value::number(double(capturer.stats().meshUpdates));
    counts["mesh_samples"] = Value::number(double(capturer.stats().meshSamples));
    counts["mesh_topology_changes"] = Value::number(double(capturer.stats().meshTopologyChanges));
    counts["duplicate_instance_draws"] = Value::number(double(capturer.stats().duplicateInstanceDraws));
    counts["skipped_no_instance"] = Value::number(double(capturer.stats().skippedNoInstance));
    sum["counts"] = std::move(counts);
    sum["draws"] = std::move(drawLog);
    Value keys = Value::array();
    for (const ex::CaptureKey& k : report.keys) {
        Value row = Value::object();
        row["algo"] = Value::string(k.algo);
        row["value"] = Value::string(k.value);
        row["kind"] = Value::string(k.kind);
        keys.push(std::move(row));
    }
    sum["keys"] = std::move(keys);
    Value ingested = Value::array();
    for (const ex::CaptureKey& k : check.storeKeys) {
        Value row = Value::object();
        row["algo"] = Value::string(k.algo);
        row["value"] = Value::string(k.value);
        ingested.push(std::move(row));
    }
    sum["ingested_keys"] = std::move(ingested);
    Value usdKeys = Value::array();
    for (const auto& [algo, value] : check.usdKeys) {
        Value row = Value::object();
        row["algo"] = Value::string(algo);
        row["value"] = Value::string(value);
        usdKeys.push(std::move(row));
    }
    sum["usda_keys"] = std::move(usdKeys);
    Value fails = Value::array();
    for (const std::string& f : failures) {
        fails.push(Value::string(f));
    }
    sum["failures"] = std::move(fails);
    ex::writeFile(out / "summary.json", ex::json::writePretty(sum));

    std::printf("rl_capture_export_replay: %s: %zu frame(s), %zu draw(s) (%zu committed), %zu frame(s) captured: %zu mesh(es), "
                "%zu material(s), %zu texture(s), %zu instance(s), %zu light(s), %zu key(s); %zu USDA layer(s); "
                "%zu buffer write(s) / %zu upload(s) not restorable\n",
                appName.c_str(), n.frames, n.draws, committedDraws, framesCaptured, data.meshes.size(), data.materials.size(),
                data.textures.size(), data.instances.size(), data.sphereLights.size() + data.distantLights.size(),
                report.keys.size(), check.layers, n.bufferWritesMissing, n.uploadsMissing);
    for (const std::string& f : failures) {
        std::fprintf(stderr, "rl_capture_export_replay: FAIL: %s\n", f.c_str());
    }
    return failures.empty() ? 0 : 1;
}
