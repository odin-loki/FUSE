// FUSE Relight RL-3.4: the runtime replacement engine (see replacement_engine.hpp).
#include <fuse/relight/replace/replacement_engine.hpp>

#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/replace/replace_options.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <thread>

namespace fuse::relight::replace {

const char* lightOriginName(ReplacedLight::Origin o) {
    switch (o) {
    case ReplacedLight::Origin::Game: return "game";
    case ReplacedLight::Origin::Replaced: return "replaced";
    case ReplacedLight::Origin::Attached: return "attached";
    }
    return "game";
}

EngineConfig EngineConfig::fromOptions(const std::string& gameId) {
    registerReplaceOptions();
    EngineConfig c;
    c.roots = parseModRootList(ReplaceOptions::modPaths(), ModKind::Remix);
    c.mods = parseModRootList(ReplaceOptions::mods(), ModKind::Remix);
    c.gameId = ReplaceOptions::gameId().empty() ? gameId : ReplaceOptions::gameId();
    if (c.gameId.empty()) {
        c.gameId = "game";
    }
    c.modOrder = parseModOrder(ReplaceOptions::modOrder());
    c.hotReload = ReplaceOptions::hotReload();
    c.watchBackend = parseWatchBackend(ReplaceOptions::watchBackend());
    c.pollIntervalMs = ReplaceOptions::pollIntervalMs();
    c.residency.budgetBytes = std::uint64_t(ReplaceOptions::textureBudgetMiB()) << 20;
    c.residency.mipBias = ReplaceOptions::textureMipBias();
    c.residency.preloadAll = ReplaceOptions::preloadTextures();
    c.debugReloadWaitFrame = ReplaceOptions::debugReloadWaitFrame();
    c.debugReloadMarker = ReplaceOptions::debugReloadMarker();
    c.debugReloadWaitMs = ReplaceOptions::debugReloadWaitMs();
    return c;
}

struct ReplacementEngine::LayerHandles {
    std::vector<options::OptionLayerHandle> handles;
};

ReplacementEngine::ReplacementEngine(EngineConfig config)
    : m_config(std::move(config)), m_layers(std::make_unique<LayerHandles>()), m_residency(m_config.residency) {
    registerReplaceOptions();
}

ReplacementEngine::~ReplacementEngine() {
    if (!m_layers->handles.empty()) {
        m_layers->handles.clear();
        options::OptionManager::applyPendingValues(nullptr, false);
    }
}

const char* ReplacementEngine::watchBackend() const { return m_watcher ? m_watcher->backend() : "off"; }

// ---- loading ------------------------------------------------------------------------------------------------------

void ReplacementEngine::loadInitial(std::uint64_t frame) {
    m_loaded = true;
    reload(frame, true, {}, frame);
}

void ReplacementEngine::reloadAll(std::uint64_t frame) {
    std::vector<std::string> dirs;
    for (const auto& c : m_contents) {
        dirs.push_back(c->location.dir);
    }
    m_loaded = true;
    reload(frame, true, dirs, frame);
    refreshSwitches();
}

void ReplacementEngine::reload(std::uint64_t frame, bool rediscover, const std::vector<std::string>& dirtyDirs,
                               std::uint64_t notifiedFrame) {
    std::vector<ModLocation> locations;
    if (rediscover) {
        locations = discoverMods(m_config.roots, m_config.mods);
    } else {
        for (const auto& c : m_contents) {
            locations.push_back(c->location);
        }
    }
    // A store directory that changed may have gained or lost sources: rediscover its sources.
    if (!rediscover) {
        for (const std::string& d : dirtyDirs) {
            if (modFormatOf(d) == ModFormat::Store) {
                locations = discoverMods(m_config.roots, m_config.mods);
                break;
            }
        }
    }
    const std::set<std::string> dirty(dirtyDirs.begin(), dirtyDirs.end());
    std::map<std::string, std::size_t> existing; // id -> index in m_contents
    for (std::size_t i = 0; i < m_contents.size(); ++i) {
        existing[m_contents[i]->location.id()] = i;
    }

    ReloadEvent event;
    std::set<std::string> nextIds;
    for (const ModLocation& loc : locations) {
        nextIds.insert(loc.id());
    }
    for (const auto& c : m_contents) {
        if (!nextIds.count(c->location.id())) {
            event.removed.push_back(c->location.name);
        }
    }
    // Unchanged mods move over (the objects stay where they are, so the old index stays valid for the diff).
    std::vector<std::unique_ptr<ModContent>> next;
    for (const ModLocation& loc : locations) {
        const auto it = existing.find(loc.id());
        if (it != existing.end() && !dirty.count(loc.dir)) {
            next.push_back(std::move(m_contents[it->second]));
            continue;
        }
        auto content = std::make_unique<ModContent>(loadMod(loc, m_config.gameId));
        if (it != existing.end()) {
            if (!content->ok) {
                event.failed.push_back(loc.name); // keep the last good content (a write in progress)
                next.push_back(std::move(m_contents[it->second]));
                continue;
            }
            event.changed.push_back(loc.name);
        } else {
            event.added.push_back(loc.name);
            if (!content->ok) {
                event.failed.push_back(loc.name);
            }
        }
        next.push_back(std::move(content));
    }
    const ReplacementIndex before = m_index; // points into `next` and `old`, both alive until the diff
    std::vector<std::unique_ptr<ModContent>> old = std::move(m_contents);
    m_contents = std::move(next);
    rebuild(frame, notifiedFrame, std::move(event), &before);
    old.clear();
}

void ReplacementEngine::rebuild(std::uint64_t frame, std::uint64_t notifiedFrame, ReloadEvent event,
                                const ReplacementIndex* before) {
    std::vector<const ModContent*> stack;
    for (const auto& c : m_contents) {
        stack.push_back(c.get());
    }
    sortStack(stack, m_config.modOrder);
    m_index.build(stack);
    m_modInfo.clear();
    m_diagnostics.clear();
    for (std::size_t r = 0; r < stack.size(); ++r) {
        const ModContent* c = stack[r];
        ModInfo info;
        info.location = c->location;
        info.rank = std::uint32_t(r);
        info.ok = c->ok;
        info.priority = c->priority;
        info.meshes = c->meshes.size();
        info.materials = c->materials.size();
        for (const auto& [h, l] : c->lights) {
            (l.deleted ? info.deletedLights : info.lights) += 1;
        }
        for (const ModDiagnostic& d : c->diagnostics) {
            if (d.severity == "error") {
                ++info.errors;
                m_diagnostics.push_back(c->location.name + ": " + d.code + ": " + d.where + ": " + d.message);
            }
        }
        m_modInfo.push_back(std::move(info));
    }
    if (m_config.optionLayers) {
        applyOptionLayers();
    }
    rebuildCatalog();
    rebuildWatcher();
    ++m_generation;
    event.generation = m_generation;
    event.notifiedFrame = notifiedFrame;
    event.appliedFrame = frame;
    if (before) {
        event.invalidated = ReplacementIndex::diff(*before, m_index);
    }
    m_pendingReload = std::move(event);
}

void ReplacementEngine::applyOptionLayers() {
    const bool had = !m_layers->handles.empty();
    m_layers->handles.clear();
    const std::vector<const ModContent*>& stack = m_index.stack();
    for (std::size_t r = 0; r < stack.size(); ++r) {
        const ModContent* c = stack[r];
        if (!c->ok || c->rtxConf.empty()) {
            continue;
        }
        char rank[32];
        std::snprintf(rank, sizeof rank, "%03zu", r);
        const std::string name = std::string("Mod Remix Config ") + rank + " " + c->location.name;
        const options::OptionConfig conf = options::OptionConfig::parse(c->rtxConf, options::OptionSystem::parseOptions());
        m_layers->handles.push_back(options::OptionManager::acquireLayer(
            "", options::OptionLayerKey(options::kBaseGameModLayerId.priority, name), options::kDefaultLayerBlendStrength,
            options::kDefaultLayerBlendThreshold, true, &conf));
        m_modInfo[r].optionLayer = name;
    }
    if (had || !m_layers->handles.empty()) {
        options::OptionManager::applyPendingValues(nullptr, false);
    }
}

void ReplacementEngine::rebuildCatalog() {
    std::map<std::string, TextureInfo> catalog;
    auto addMaterial = [&](const MaterialDef& m) {
        for (const TextureRef& t : m.textures) {
            TextureInfo& info = catalog[t.sha256];
            info.sha256 = t.sha256;
            info.format = t.format;
            info.width = t.width;
            info.height = t.height;
            info.mips = t.mips;
            info.fileBytes = t.fileBytes;
            info.preload = info.preload || m.preloadTextures;
        }
    };
    for (const ModContent* c : m_index.stack()) {
        if (!c->ok) {
            continue;
        }
        for (const auto& [h, m] : c->materials) {
            addMaterial(m);
        }
        for (const auto& [id, m] : c->boundMaterials) {
            addMaterial(m);
        }
    }
    m_residency.setConfig(m_config.residency);
    m_residency.setCatalog(std::move(catalog));
}

void ReplacementEngine::rebuildWatcher() {
    if (!m_config.hotReload) {
        m_watcher.reset();
        return;
    }
    std::vector<WatchedDir> dirs;
    std::vector<std::string> modDirs;
    for (const ModRoot& r : m_config.roots) {
        dirs.push_back({r.dir, false});
        modDirs.emplace_back();
    }
    std::set<std::string> seen;
    for (const ModRoot& m : m_config.mods) {
        if (seen.insert(m.dir).second) {
            dirs.push_back({m.dir, true});
            modDirs.push_back(m.dir);
        }
    }
    for (const auto& c : m_contents) {
        if (seen.insert(c->location.dir).second) {
            dirs.push_back({c->location.dir, true});
            modDirs.push_back(c->location.dir);
        }
    }
    if (m_watcher && modDirs == m_watchedModDirs) {
        return;
    }
    m_watchedModDirs = std::move(modDirs);
    m_watcher = std::make_unique<DirectoryWatcher>(std::move(dirs), m_config.watchBackend, m_config.pollIntervalMs);
}

void ReplacementEngine::debugWaitForNotification(std::uint64_t frame) {
    if (!m_watcher || m_config.debugReloadMarker.empty()) {
        return;
    }
    {
        std::ofstream marker(m_config.debugReloadMarker, std::ios::binary | std::ios::trunc);
        marker << frame << "\n";
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_config.debugReloadWaitMs);
    std::vector<std::size_t> changed;
    while (std::chrono::steady_clock::now() < deadline) {
        changed = m_watcher->poll(true);
        if (!changed.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    m_debugChanged = std::move(changed);
}

// ---- per frame ----------------------------------------------------------------------------------------------------

void ReplacementEngine::beginFrame(std::uint64_t frame) {
    m_frame = frame;
    m_stats = {};
    refreshSwitches(); // once per frame: an option change takes effect at the next frame
    m_attached.clear();
    m_usedTextures.clear();
    if (!m_loaded) {
        loadInitial(frame);
        refreshSwitches();
        return;
    }
    if (!m_watcher) {
        return;
    }
    m_debugChanged.clear();
    if (m_config.debugReloadWaitFrame != 0 && frame == m_config.debugReloadWaitFrame) {
        debugWaitForNotification(frame);
    }
    std::vector<std::size_t> changed = m_watcher->poll(false);
    changed.insert(changed.end(), m_debugChanged.begin(), m_debugChanged.end());
    if (changed.empty()) {
        return;
    }
    bool rediscover = false;
    std::vector<std::string> dirty;
    for (const std::size_t i : changed) {
        if (i >= m_watchedModDirs.size()) {
            continue;
        }
        if (m_watchedModDirs[i].empty()) {
            rediscover = true; // a search root: mods may have appeared or gone
        } else {
            dirty.push_back(m_watchedModDirs[i]);
        }
    }
    // An explicit mod directory that is not loaded yet (it had no mod when discovered) needs rediscovery too.
    for (const std::string& d : dirty) {
        const bool loaded = std::any_of(m_contents.begin(), m_contents.end(),
                                        [&](const std::unique_ptr<ModContent>& c) { return c->location.dir == d; });
        rediscover = rediscover || !loaded;
    }
    reload(frame, rediscover, dirty, frame);
    refreshSwitches(); // the reload may have changed the mods' option layers
}

void ReplacementEngine::refreshSwitches() {
    const bool assets = ReplaceOptions::enableReplacementAssets();
    m_meshesOn = assets && ReplaceOptions::enableReplacementMeshes();
    m_materialsOn = assets && ReplaceOptions::enableReplacementMaterials();
    m_lightsOn = assets && ReplaceOptions::enableReplacementLights();
}

ReplacedDraw ReplacementEngine::replaceDraw(const DrawInput& draw) {
    ReplacedDraw out;
    out.index = draw.index;
    out.instanceId = draw.instanceId;
    out.categories = draw.categories;
    ++m_stats.draws;
    const bool meshesOn = m_meshesOn, materialsOn = m_materialsOn, lightsOn = m_lightsOn;

    auto rankOf = [&](const ModContent* m) {
        const auto& s = m_index.stack();
        return std::size_t(std::find(s.begin(), s.end(), m) - s.begin());
    };
    auto names = [](const std::vector<const ModContent*>& mods) {
        std::vector<std::string> n;
        for (const ModContent* m : mods) {
            n.push_back(m->location.name);
        }
        return n;
    };
    auto useTextures = [&](const MaterialDef& m) {
        for (const TextureRef& t : m.textures) {
            m_usedTextures.insert(t.sha256);
        }
    };

    // Mesh replacement: the strongest mod over every rule of the stack.
    const ReplacementIndex::MeshHit* mesh = nullptr;
    if (meshesOn && draw.geometryValid) {
        std::optional<hash::DrawGeometryHashes> legacy;
        for (const hash::HashRule rule : m_index.meshRules()) {
            Hash64 key = 0;
            if (rule == hash::rules::kLegacyAsset0 || rule == hash::rules::kLegacyAsset1) {
                if (!legacy && draw.legacyHashes) {
                    legacy = draw.legacyHashes();
                }
                if (!legacy || !legacy->hashes.isRuleHashDefinedUpstream(rule)) {
                    continue;
                }
                key = hash::meshReplacementHashLegacy(*legacy, rule, 0);
            } else {
                key = hash::meshReplacementHash(draw.hashes, rule, 0);
            }
            const ReplacementIndex::MeshHit* hit = m_index.mesh(rule, key);
            if (hit && (!mesh || rankOf(hit->mod) < rankOf(mesh->mod))) {
                mesh = hit;
            }
        }
    }
    if (mesh) {
        const MeshReplacementDef& def = *mesh->def;
        out.meshReplaced = true;
        out.meshMod = mesh->mod->location.name;
        out.meshRecord = def.recordId;
        out.meshRule = def.ruleString;
        out.meshKey = def.key;
        out.meshShadowed = names(mesh->shadowed);
        out.drawOriginal = def.preserve();
        for (std::uint32_t i = 0; i < scene::kInstanceCategoryCount; ++i) {
            const auto c = static_cast<scene::InstanceCategories>(i);
            if (def.categoriesSet.test(c)) {
                out.categories.set(c);
            } else if (def.categoriesCleared.test(c)) {
                out.categories.clr(c);
            }
        }
        out.categoriesChanged = out.categories != draw.categories;
        const ReplacementIndex::MaterialHit* drawMaterial =
            materialsOn && draw.materialHash != 0 ? m_index.material(draw.materialHash) : nullptr;
        for (const MeshPartDef& part : def.parts) {
            ReplacedPart p;
            p.meshId = part.meshId;
            p.objectToWorld = multiply(part.transform, draw.objectToWorld);
            const MaterialDef* bound = nullptr;
            for (const std::string& id : part.materials) {
                if ((bound = m_index.boundMaterial(mesh->mod, id)) != nullptr) {
                    break;
                }
            }
            if (bound) {
                p.material = bound->recordId;
                p.materialMod = mesh->mod->location.name;
                p.materialSource = "bound";
                useTextures(*bound);
            } else if (drawMaterial) {
                p.material = drawMaterial->def->recordId;
                p.materialMod = drawMaterial->mod->location.name;
                p.materialSource = "replacement";
                useTextures(*drawMaterial->def);
                ++m_stats.partMaterialReplaced;
            } else {
                p.materialSource = "legacy";
            }
            out.parts.push_back(std::move(p));
        }
        if (lightsOn) {
            for (const LightDef& l : def.lights) {
                ReplacedLight rl;
                rl.origin = ReplacedLight::Origin::Attached;
                rl.mod = mesh->mod->location.name;
                rl.recordId = l.recordId;
                rl.type = l.type;
                const Mat4d world = multiply(l.transform, draw.objectToWorld);
                rl.position = transformPoint(world, {0.0, 0.0, 0.0});
                rl.direction = transformDirection(world, {0.0, 0.0, -1.0}); // UsdLux lights emit along -Z
                rl.color = l.color;
                rl.intensity = l.intensity;
                rl.instanceId = draw.instanceId;
                rl.drawIndex = draw.index;
                out.lights.push_back(rl);
                m_attached.push_back(std::move(rl));
            }
        }
        ++m_stats.meshReplaced;
        ++(out.drawOriginal ? m_stats.preserved : m_stats.hidden);
        m_stats.parts += std::uint32_t(out.parts.size());
    }

    // Material replacement of the kept original draw.
    if (out.drawOriginal && materialsOn && draw.materialHash != 0) {
        if (const ReplacementIndex::MaterialHit* hit = m_index.material(draw.materialHash)) {
            out.materialReplaced = true;
            out.materialMod = hit->mod->location.name;
            out.materialRecord = hit->def->recordId;
            out.materialShadowed = names(hit->shadowed);
            out.materialIgnored = hit->def->ignoreMaterial;
            useTextures(*hit->def);
            ++m_stats.materialReplaced;
        }
    }
    return out;
}

ReplacedFrame ReplacementEngine::endFrame(const std::vector<scene::LightRecord>& gameLights) {
    ReplacedFrame f;
    f.frame = m_frame;
    f.generation = m_generation;
    const bool lightsOn = m_lightsOn;
    for (const scene::LightRecord& rec : gameLights) {
        const ReplacementIndex::LightHit* hit = lightsOn ? m_index.light(rec.hash) : nullptr;
        if (hit && hit->def->deleted) {
            f.deletedLights.push_back(rec.hash);
            ++m_stats.lightsDeleted;
            continue;
        }
        ReplacedLight l;
        l.gameHash = rec.hash;
        if (hit) {
            const LightDef& d = hit->def->light;
            l.origin = ReplacedLight::Origin::Replaced;
            l.mod = hit->mod->location.name;
            l.recordId = d.recordId;
            l.type = d.type;
            l.position = transformPoint(d.transform, {0.0, 0.0, 0.0});
            l.direction = transformDirection(d.transform, {0.0, 0.0, -1.0});
            l.color = d.color;
            l.intensity = d.intensity;
            ++m_stats.lightsReplaced;
        } else {
            l.origin = ReplacedLight::Origin::Game;
            l.type = rec.type == hash::LightType::Sphere ? "sphere" : "distant";
            l.position = {rec.position[0], rec.position[1], rec.position[2]};
            l.direction = {rec.direction[0], rec.direction[1], rec.direction[2]};
            l.color = {rec.radiance[0], rec.radiance[1], rec.radiance[2]};
            l.intensity = rec.intensity;
            ++m_stats.lightsGame;
        }
        f.lights.push_back(std::move(l));
    }
    for (ReplacedLight& a : m_attached) {
        f.lights.push_back(std::move(a));
        ++m_stats.lightsAttached;
    }
    m_attached.clear();
    f.residency = m_residency.update(m_frame, m_usedTextures);
    f.stats = m_stats;
    f.reload = std::move(m_pendingReload);
    m_pendingReload.reset();
    return f;
}

} // namespace fuse::relight::replace
