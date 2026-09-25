// FUSE Relight RL-6.3: game-setup assistant (see setup_assistant.hpp).
#include <fuse/relight/setup/setup_assistant.hpp>

#include "setup_json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fuse::relight::setup {

namespace {

// RL-1.5 material blend factors (Vulkan VkBlendFactor numbering).
constexpr int kBlendOne = 1;

constexpr double kSkyConfidence = 0.95;
constexpr double kUiConfidence = 0.9;
constexpr double kPositionTUiConfidence = 0.85;
constexpr double kRaytracedRtConfidence = 0.7;
constexpr double kParticleConfidence = 0.6;
constexpr double kDecalConfidence = 0.6;
constexpr double kIgnoreConfidence = 0.6;
constexpr double kFullScreenPositionTConfidence = 0.5;
constexpr double kHashRuleConfidence = 0.5;
constexpr double kConflictConfidence = 0.4;
constexpr double kPositionTSceneConfidence = 0.3;

constexpr std::uint32_t kSmallBatchVertices = 64; // particle quads / sprites per draw
constexpr float kFullScreenCoverage = 0.95f;

enum class DrawSpace : std::uint8_t { Skip, Sky, Screen, World };

struct Draw {
    std::uint32_t frame = 0;
    std::uint32_t index = 0;
    DrawSpace space = DrawSpace::Skip;
    bool positionT = false;
    bool fullScreen = false;
    bool raytraced = false;
    bool blended = false;
    bool additive = false;
    bool alphaTest = false;
    bool zEnable = false;
    bool zWrite = false;
    std::uint64_t colorTexture = 0;
    std::uint64_t descriptor = 0;
    std::int64_t textureId = -1;
    std::uint32_t vertices = 0;
    std::uint64_t assetKey = 0;
    std::string reason;
    std::vector<std::string> categories;
};

bool hexField(const json::Value& v, std::string_view key, std::uint64_t& out) {
    const json::Value* f = v.get(key);
    return f && f->isString() && parseHash(f->s, out);
}

std::uint32_t u32Field(const json::Value& v, std::string_view key) {
    const json::Value* f = v.get(key);
    if (!f) {
        return 0;
    }
    if (f->isNumber()) {
        return f->n > 0 ? static_cast<std::uint32_t>(f->n) : 0u;
    }
    if (f->isString()) {
        return static_cast<std::uint32_t>(std::strtoul(f->s.c_str(), nullptr, 10));
    }
    return 0;
}

float hexFloat(std::string_view text) {
    std::uint64_t bits = 0;
    if (!parseHash(text, bits)) {
        return 0.0f;
    }
    const auto u = static_cast<std::uint32_t>(bits);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

std::vector<std::string> splitCategories(const std::string& s) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : s) {
        if (c == '|') {
            if (!current.empty()) {
                out.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

bool has(const std::vector<std::string>& list, std::string_view v) {
    return std::find(list.begin(), list.end(), v) != list.end();
}

// Screen-space coverage of a POSITIONT draw: its AABB is in pixels (RL-1.3 "aabb": 6 hex floats).
bool coversViewport(const json::Value& geometry, const json::Value* viewport) {
    const std::string aabb = geometry.str("aabb");
    if (aabb.empty() || !viewport || !viewport->isArray() || viewport->a.size() < 4) {
        return false;
    }
    float b[6] = {};
    std::size_t start = 0;
    for (int k = 0; k < 6; ++k) {
        const std::size_t comma = aabb.find(',', start);
        b[k] = hexFloat(std::string_view(aabb).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) {
            if (k != 5) {
                return false;
            }
            break;
        }
        start = comma + 1;
    }
    const double vx = viewport->a[0].n, vy = viewport->a[1].n, vw = viewport->a[2].n, vh = viewport->a[3].n;
    if (vw <= 0.0 || vh <= 0.0) {
        return false;
    }
    const double w = std::max(0.0, std::min<double>(b[3], vx + vw) - std::max<double>(b[0], vx));
    const double h = std::max(0.0, std::min<double>(b[4], vy + vh) - std::max<double>(b[1], vy));
    return w * h >= kFullScreenCoverage * vw * vh;
}

Draw readDraw(const json::Value& r) {
    Draw d;
    d.frame = u32Field(r, "frame");
    d.index = u32Field(r, "di");
    const json::Value* c = r.get("classification");
    const json::Value* t = r.get("translation");
    const json::Value* g = r.get("geometry");
    if (c) {
        d.reason = c->str("reason");
        d.categories = splitCategories(c->str("categories"));
        d.raytraced = c->str("status") == "raytraced";
        hexField(*c, "color_texture", d.colorTexture);
    }
    if (g) {
        d.vertices = u32Field(*g, "vc");
        hexField(*g, "key", d.assetKey);
    }
    if (const json::Value* texs = r.get("textures"); texs && texs->isArray()) {
        for (const json::Value& e : texs->a) {
            std::uint64_t h = 0;
            if (hexField(e, "hash", h) && h == d.colorTexture && d.colorTexture != 0) {
                hexField(e, "desc", d.descriptor);
                d.textureId = static_cast<std::int64_t>(e.num("texture", -1));
                break;
            }
        }
    }
    d.positionT = d.reason == "PositionT" || d.reason == "PositionTAsUI";
    const bool offscreen = d.reason == "NonPrimaryTarget" || d.reason == "DrawingToRaytracedTarget" ||
                           d.reason == "SkyInRaytracedTarget";
    const bool dropped = c && c->str("status") == "ignored" && d.reason != "IgnoreTexture";
    if (offscreen || dropped || !c) {
        d.space = DrawSpace::Skip;
        return d;
    }
    const std::string camera = t ? t->str("camera") : std::string();
    if (t) {
        d.zEnable = t->flag("z_enable");
        d.zWrite = t->flag("z_write");
        if (const json::Value* m = t->get("material")) {
            d.blended = m->flag("blend");
            d.alphaTest = m->flag("alpha_test");
            d.additive = d.blended && static_cast<int>(m->num("color_dst", -1)) == kBlendOne;
        }
    }
    const double minZ = t ? t->num("min_z", 0.0) : 0.0;
    if (has(d.categories, "Sky") || camera == "Sky" || (t && minZ >= 1.0 && !d.zWrite)) {
        d.space = DrawSpace::Sky;
    } else if (d.positionT || d.reason == "UserInterface" || (t && camera == "Unknown" && !d.zEnable)) {
        d.space = DrawSpace::Screen;
        if (d.positionT && g) {
            d.fullScreen = coversViewport(*g, t ? t->get("viewport") : nullptr);
        }
    } else if (t) {
        d.space = DrawSpace::World;
    } else {
        d.space = DrawSpace::Skip; // post-injection raster draw without translation: nothing to learn
    }
    return d;
}

ProfileSuggestion textureSuggestion(const char* category, std::uint64_t hash, double confidence, std::string reason) {
    ProfileSuggestion s;
    s.kind = ProfileSuggestion::Kind::Texture;
    s.category = category;
    s.hash = hash;
    s.confidence = confidence;
    s.reason = std::move(reason);
    return s;
}

ProfileSuggestion optionSuggestion(const char* key, const char* value, double confidence, std::string reason) {
    ProfileSuggestion s;
    s.kind = ProfileSuggestion::Kind::Option;
    s.key = key;
    s.value = value;
    s.confidence = confidence;
    s.reason = std::move(reason);
    return s;
}

std::string counts(const TextureUsage& u) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%u draw(s) in %zu frame(s): %u sky, %u screen-space, %u world", u.draws,
                  u.frames.size(), u.skyDraws, u.screenDraws, u.worldDraws);
    return buf;
}

void proposeTextures(SetupReport& report) {
    for (const auto& [hash, u] : report.textures) {
        const std::string stats = counts(u);
        const bool world = u.worldDraws > 0;
        if (u.renderTarget && world) {
            report.proposals.push_back(textureSuggestion(
                "raytracedRenderTarget", u.descriptor != 0 ? u.descriptor : hash, kRaytracedRtConfidence,
                "render-target texture sampled by world geometry (descriptor hash); ray trace its offscreen pass "
                "(" + stats + ")"));
        }
        if (u.skyDraws > 0 && !world && u.screenDraws == 0) {
            report.proposals.push_back(textureSuggestion("sky", hash, kSkyConfidence, "only on sky draws (" + stats + ")"));
        } else if (u.skyDraws > 0) {
            report.proposals.push_back(textureSuggestion(
                "sky", hash, kConflictConfidence, "on sky draws but also elsewhere; tagging moves those to the sky (" + stats + ")"));
        }
        if (u.screenDraws > 0 && u.fullScreenDraws == u.screenDraws && u.renderTarget) {
            report.proposals.push_back(textureSuggestion(
                "ignore", hash, kIgnoreConfidence, "full-screen screen-space pass sampling a render target (post effect) (" + stats + ")"));
        } else if (u.screenDraws > 0 && !world && u.skyDraws == 0) {
            report.proposals.push_back(textureSuggestion("ui", hash, kUiConfidence, "only on screen-space draws (" + stats + ")"));
        } else if (u.screenDraws > 0) {
            report.proposals.push_back(textureSuggestion(
                "ui", hash, kConflictConfidence,
                "on screen-space HUD draws but also on " + std::to_string(u.worldDraws) +
                    " world draw(s): as a UI texture those become UI too and the first one the injection point (" + stats + ")"));
        }
        if (world && u.skyDraws == 0 && u.screenDraws == 0 && !u.renderTarget) {
            if (u.additiveWorld == u.worldDraws && u.noZWriteWorld == u.worldDraws && u.maxVertices <= kSmallBatchVertices) {
                report.proposals.push_back(textureSuggestion(
                    "particle", hash, kParticleConfidence, "additive, depth writes off, small batches (" + stats + ")"));
            } else if (u.noZWriteWorld == u.worldDraws && u.additiveWorld == 0 &&
                       (u.blendedWorld == u.worldDraws || u.alphaTestedWorld == u.worldDraws)) {
                if (u.maxVertices <= kSmallBatchVertices && u.blendedWorld == u.worldDraws) {
                    report.proposals.push_back(textureSuggestion(
                        "particle", hash, kParticleConfidence * 0.75,
                        "alpha-blended small batches with depth writes off: particles or decals (" + stats + ")"));
                }
                report.proposals.push_back(textureSuggestion(
                    "decal", hash, kDecalConfidence, "blended / alpha-tested, depth-tested without depth writes (" + stats + ")"));
            }
        }
    }
}

void proposeOptions(SetupReport& report, bool positionTFullScreenOnly) {
    if (report.positionTDraws > 0 && report.positionTAsUI == 0) {
        if (report.positionTBeforeScene > 0) {
            report.proposals.push_back(optionSuggestion(
                "rtx.preTransformedVerticesIsUI", "True", kPositionTSceneConfidence,
                std::to_string(report.positionTBeforeScene) +
                    " POSITIONT draw(s) come before 3D draws of the same frame: they may be scene geometry"));
        } else if (positionTFullScreenOnly) {
            report.proposals.push_back(optionSuggestion(
                "rtx.preTransformedVerticesIsUI", "True", kFullScreenPositionTConfidence,
                "every POSITIONT draw is a full-screen quad after the 3D pass: a post effect (e.g. stencil shadow "
                "darkening) rather than a HUD; as UI it would be rasterized on top of the ray-traced image"));
        } else {
            report.proposals.push_back(optionSuggestion(
                "rtx.preTransformedVerticesIsUI", "True", kPositionTUiConfidence,
                std::to_string(report.positionTDraws) + " POSITIONT draw(s), all after the 3D draws of their frame: screen-space UI"));
        }
    }
    if (report.comparableAssetDraws > 0 && report.unstableAssetDraws * 4 >= report.comparableAssetDraws) {
        report.proposals.push_back(optionSuggestion(
            "rtx.geometryAssetHashRuleString", "indices,texcoords,geometrydescriptor", kHashRuleConfidence,
            std::to_string(report.unstableAssetDraws) + " of " + std::to_string(report.comparableAssetDraws) +
                " draw slots change their asset hash between frames (animated or re-uploaded vertices): an asset "
                "hash rule without positions keeps replacements attached"));
    }
    std::sort(report.proposals.begin(), report.proposals.end(), [](const ProfileSuggestion& a, const ProfileSuggestion& b) {
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        if (a.kind == ProfileSuggestion::Kind::Texture) {
            return a.category != b.category ? a.category < b.category : a.hash < b.hash;
        }
        return a.key < b.key;
    });
}

void checkCameras(SetupReport& report, const std::vector<const json::Value*>& frames,
                  const std::set<std::uint32_t>& framesWith3D) {
    if (frames.empty() && !framesWith3D.empty()) {
        report.cameraIssues.push_back({-1, "the capture has no translate_frame records (no camera information)"});
        return;
    }
    bool sawSky = false;
    for (const json::Value* f : frames) {
        const auto frame = static_cast<std::int64_t>(f->num("frame", -1));
        const json::Value* cams = f->get("cameras");
        std::uint32_t mains = 0;
        if (cams && cams->isArray()) {
            for (const json::Value& cam : cams->a) {
                const std::string type = cam.str("type");
                sawSky = sawSky || type == "Sky";
                if (type != "Main") {
                    continue;
                }
                ++mains;
                const double fov = cam.num("fov"), nearZ = cam.num("near"), farZ = cam.num("far");
                if (!(fov > 0.17 && fov < 2.8)) {
                    report.cameraIssues.push_back({frame, "main camera FOV " + json::number(fov) + " rad is implausible"});
                }
                if (!(nearZ > 0.0) || !(farZ > nearZ)) {
                    report.cameraIssues.push_back({frame, "main camera near/far " + json::number(nearZ) + "/" + json::number(farZ) + " are invalid"});
                }
            }
        }
        if (mains == 0 && framesWith3D.count(static_cast<std::uint32_t>(frame)) != 0) {
            report.cameraIssues.push_back({frame, "3D draws but no main camera"});
        }
    }
    if (sawSky) {
        report.notes.push_back("a separate sky camera was detected (sky box drawn with its own view)");
    }
}

} // namespace

std::optional<SetupReport> analyzeCaptureRecord(std::string_view jsonl, std::string* error) {
    std::vector<json::Value> records;
    std::size_t lineNo = 0;
    std::size_t start = 0;
    while (start < jsonl.size()) {
        std::size_t end = jsonl.find('\n', start);
        if (end == std::string_view::npos) {
            end = jsonl.size();
        }
        std::string_view line = jsonl.substr(start, end - start);
        start = end + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        std::string err;
        std::optional<json::Value> v = json::parse(line, &err);
        if (!v || !v->isObject()) {
            if (error) {
                *error = "line " + std::to_string(lineNo) + ": " + (v ? std::string("not an object") : err);
            }
            return std::nullopt;
        }
        records.push_back(std::move(*v));
    }

    SetupReport report;
    // Texture origins by id (the per-frame "textures" records).
    std::map<std::int64_t, std::string> origins;
    std::vector<const json::Value*> cameraFrames;
    std::set<std::uint32_t> frames;
    for (const json::Value& r : records) {
        const std::string ev = r.str("ev");
        if (ev == "textures") {
            if (const json::Value* list = r.get("textures"); list && list->isArray()) {
                for (const json::Value& t : list->a) {
                    origins[static_cast<std::int64_t>(t.num("id", -1))] = t.str("origin");
                }
            }
        } else if (ev == "translate_frame") {
            cameraFrames.push_back(&r);
        } else if (ev == "frame") {
            frames.insert(u32Field(r, "frame"));
        }
    }

    std::vector<Draw> draws;
    for (const json::Value& r : records) {
        if (r.str("ev") == "draw") {
            draws.push_back(readDraw(r));
            frames.insert(draws.back().frame);
        }
    }
    report.frames = static_cast<std::uint32_t>(frames.size());
    report.draws = static_cast<std::uint32_t>(draws.size());

    // Per frame: the last 3D (sky / world) draw, for the POSITIONT ordering rule.
    std::map<std::uint32_t, std::uint32_t> last3D;
    std::set<std::uint32_t> framesWith3D;
    for (const Draw& d : draws) {
        if (d.space == DrawSpace::World || d.space == DrawSpace::Sky) {
            last3D[d.frame] = std::max(last3D[d.frame], d.index + 1);
            framesWith3D.insert(d.frame);
        }
    }

    bool positionTFullScreenOnly = true;
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::set<std::uint64_t>> assetKeysBySlot;
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t> slotFrames;
    for (const Draw& d : draws) {
        if (d.positionT) {
            ++report.positionTDraws;
            report.positionTAsUI += d.reason == "PositionTAsUI" ? 1u : 0u;
            const auto it = last3D.find(d.frame);
            if (it != last3D.end() && d.index < it->second) {
                ++report.positionTBeforeScene;
            }
            positionTFullScreenOnly = positionTFullScreenOnly && d.fullScreen;
        }
        if (d.space == DrawSpace::Screen && d.raytraced) {
            ++report.raytracedScreenSpace;
        }
        if (d.space == DrawSpace::World && d.assetKey != 0) {
            // A draw slot: its index in the frame and its texture; vertex count folded into the texture key.
            const auto slot = std::make_pair(d.index, d.colorTexture ^ (static_cast<std::uint64_t>(d.vertices) << 48));
            assetKeysBySlot[slot].insert(d.assetKey);
            ++slotFrames[slot];
        }
        if (d.colorTexture == 0 || d.space == DrawSpace::Skip) {
            continue;
        }
        TextureUsage& u = report.textures[d.colorTexture];
        u.hash = d.colorTexture;
        ++u.draws;
        u.frames.insert(d.frame);
        u.reasons.insert(d.reason);
        for (const std::string& c : d.categories) {
            u.classifiedCategories.insert(c);
        }
        u.maxVertices = std::max(u.maxVertices, d.vertices);
        if (d.descriptor != 0) {
            u.descriptor = d.descriptor;
        }
        if (const auto o = origins.find(d.textureId); o != origins.end() && o->second == "render_target") {
            u.renderTarget = true;
        }
        switch (d.space) {
        case DrawSpace::Sky: ++u.skyDraws; break;
        case DrawSpace::Screen:
            ++u.screenDraws;
            u.fullScreenDraws += d.fullScreen ? 1u : 0u;
            break;
        case DrawSpace::World:
            ++u.worldDraws;
            u.blendedWorld += d.blended ? 1u : 0u;
            u.additiveWorld += d.additive ? 1u : 0u;
            u.alphaTestedWorld += d.alphaTest ? 1u : 0u;
            u.noZWriteWorld += (d.zEnable && !d.zWrite) ? 1u : 0u;
            break;
        case DrawSpace::Skip: break;
        }
    }
    for (const auto& [slot, keys] : assetKeysBySlot) {
        if (slotFrames[slot] < 2) {
            continue;
        }
        ++report.comparableAssetDraws;
        report.unstableAssetDraws += keys.size() > 1 ? 1u : 0u;
    }

    proposeTextures(report);
    proposeOptions(report, report.positionTDraws > 0 && positionTFullScreenOnly);
    checkCameras(report, cameraFrames, framesWith3D);
    if (report.raytracedScreenSpace > 0) {
        report.notes.push_back(std::to_string(report.raytracedScreenSpace) +
                               " screen-space draw(s) (orthographic, depth test off) are ray traced: Remix's "
                               "isRenderingUI also needs depth writes off. Tag their textures as UI (rtx.uiTextures).");
    }
    return report;
}

std::optional<SetupReport> analyzeCaptureFile(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot read " + path;
        }
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return analyzeCaptureRecord(text.str(), error);
}

GameProfile proposeProfile(const SetupReport& report, const std::string& name, const std::string& exeName,
                           std::optional<std::uint64_t> exeHash, const AssistantOptions& options) {
    GameProfile p;
    p.name = name;
    p.description = "Generated by the FUSE Relight setup assistant from a capture of " + std::to_string(report.frames) +
                    " frame(s), " + std::to_string(report.draws) + " draw(s).";
    if (!exeName.empty()) {
        p.match.exeNames.push_back(exeName);
    }
    if (exeHash) {
        p.match.exeHashes.push_back(*exeHash);
    }
    p.suggestions = report.proposals;
    for (ProfileSuggestion& s : p.suggestions) {
        if (s.confidence + 1e-9 < options.applyThreshold) {
            continue;
        }
        if (s.kind == ProfileSuggestion::Kind::Texture) {
            s.applied = p.addTexture(s.category, s.hash);
        } else {
            p.options[s.key] = s.value;
            s.applied = true;
        }
    }
    for (const CameraIssue& issue : report.cameraIssues) {
        p.notes.push_back("camera" + (issue.frame >= 0 ? " (frame " + std::to_string(issue.frame) + ")" : std::string()) +
                          ": " + issue.message);
    }
    for (const std::string& note : report.notes) {
        p.notes.push_back(note);
    }
    return p;
}

std::string formatReport(const SetupReport& report) {
    std::string out = "frames " + std::to_string(report.frames) + ", draws " + std::to_string(report.draws) +
                      ", textures " + std::to_string(report.textures.size()) + ", POSITIONT draws " +
                      std::to_string(report.positionTDraws) + "\n";
    for (const auto& [hash, u] : report.textures) {
        out += "  texture " + formatHash(hash) + (u.renderTarget ? " (render target)" : "") + ": " + counts(u) + "\n";
    }
    for (const ProfileSuggestion& s : report.proposals) {
        out += "  propose ";
        out += s.kind == ProfileSuggestion::Kind::Texture ? s.category + " " + formatHash(s.hash) : s.key + " = " + s.value;
        out += " (" + json::number(std::round(s.confidence * 100.0) / 100.0) + "): " + s.reason + "\n";
    }
    for (const CameraIssue& issue : report.cameraIssues) {
        out += "  camera: " + issue.message + "\n";
    }
    for (const std::string& n : report.notes) {
        out += "  note: " + n + "\n";
    }
    return out;
}

} // namespace fuse::relight::setup
