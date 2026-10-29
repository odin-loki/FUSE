// Linked prefab instances: baseline / override tracking, propagation and the `.prefablinks` sidecar.
#include <fuse/editor/editor_host.hpp>

#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <type_traits>

namespace fuse::editor {

namespace {

template <typename T>
bool sameComponent(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, ecs::Transform>) {
        return std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 &&
               std::memcmp(&a.rotation, &b.rotation, sizeof(a.rotation)) == 0 &&
               std::memcmp(&a.scale, &b.scale, sizeof(a.scale)) == 0;
    } else if constexpr (std::is_empty_v<T>) {
        return true;
    } else {
        static_assert(std::is_trivially_copyable_v<T>, "prefab override diff needs POD components");
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    }
}

template <typename T>
bool sameOpt(const std::optional<T>& a, const std::optional<T>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a.has_value() || sameComponent(*a, *b);
}

/// Per component slot: `fn(index, name, current, baseline, incoming)`.
template <typename Fn, std::size_t... I>
void forEachSlot_(EntityComponentSet& cur, const EntityComponentSet& base, const EntityComponentSet& in, Fn&& fn,
                  std::index_sequence<I...>) {
    (fn(I, std::tuple_element_t<I, EntityComponentSet>::value_type::component_name, std::get<I>(cur),
        std::get<I>(base), std::get<I>(in)),
     ...);
}
template <typename Fn>
void forEachSlot(EntityComponentSet& cur, const EntityComponentSet& base, const EntityComponentSet& in, Fn&& fn) {
    forEachSlot_(cur, base, in, std::forward<Fn>(fn),
                 std::make_index_sequence<std::tuple_size_v<EntityComponentSet>>{});
}

constexpr usize kTransformSlot = 0;

} // namespace

const EditorHost::PrefabLink* EditorHost::findPrefabLink_(ecs::EntityID entity, usize* indexOut) const {
    for (usize l = 0; l < m_prefabLinks.size(); ++l) {
        for (ecs::EntityID e : m_prefabLinks[l].entities) {
            if (e == entity) {
                if (indexOut != nullptr) *indexOut = l;
                return &m_prefabLinks[l];
            }
        }
    }
    return nullptr;
}

std::vector<std::string> EditorHost::prefabOverridesOf(ecs::EntityID entity) const {
    std::vector<std::string> names;
    const PrefabLink* link = findPrefabLink_(entity);
    const ecs::Registry& reg = m_editorScene.registry();
    if (link == nullptr || !reg.alive(entity)) return names;
    for (usize i = 0; i < link->entities.size(); ++i) {
        if (link->entities[i] != entity) continue;
        EntityComponentSet cur = captureEntityComponents(reg, entity);
        forEachSlot(cur, link->baseline[i], link->baseline[i], [&](usize slot, const char* name, auto& c, const auto& b, const auto&) {
            if (i == 0 && slot == kTransformSlot) return; // root placement is per instance
            if (!sameOpt(c, b)) names.emplace_back(name);
        });
    }
    return names;
}

std::vector<EditorHost::PrefabLinkInfo> EditorHost::prefabLinks() const {
    std::vector<PrefabLinkInfo> out;
    for (const PrefabLink& l : m_prefabLinks) {
        PrefabLinkInfo info;
        info.path = l.path;
        info.root = l.entities.empty() ? ecs::EntityID::null() : l.entities.front();
        info.entityCount = l.entities.size();
        for (ecs::EntityID e : l.entities) info.overrideCount += prefabOverridesOf(e).size();
        out.push_back(std::move(info));
    }
    return out;
}

std::optional<EditorHost::PrefabLinkInfo> EditorHost::prefabLinkOf(ecs::EntityID entity) const {
    usize index = 0;
    if (findPrefabLink_(entity, &index) == nullptr) return std::nullopt;
    return prefabLinks()[index];
}

bool EditorHost::loadPrefabSet_(const std::string& path, std::vector<EntityComponentSet>& out, std::string& error) const {
    scene::Scene loadedScene;
    ecs::Registry in;
    in.init(4096);
    const scene::SerialiseResult r = scene::SceneSerialiser::loadWithRegistry(path, loadedScene, in);
    if (r.status != scene::SerialiseStatus::Ok) {
        error = r.error;
        return false;
    }
    // Same order instantiatePrefab records (registry iteration over Transform).
    in.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform&) { out.push_back(captureEntityComponents(in, id)); });
    return true;
}

usize EditorHost::reloadPrefabInstances(const std::string& path) {
    std::vector<EntityComponentSet> incoming;
    std::string error;
    bool loaded = false;
    usize updated = 0;
    ecs::Registry& reg = m_editorScene.registry();
    for (PrefabLink& link : m_prefabLinks) {
        if (link.path != path) continue;
        if (!loaded) {
            if (!loadPrefabSet_(path, incoming, error)) {
                postConsoleOutput(fuse::log::Level::Error, "reload prefab " + path + " failed: " + error);
                return 0;
            }
            loaded = true;
        }
        if (incoming.size() != link.entities.size()) {
            postConsoleOutput(fuse::log::Level::Warn, "prefab " + path + ": entity count changed (" +
                                                          std::to_string(link.entities.size()) + " -> " +
                                                          std::to_string(incoming.size()) +
                                                          "); matching instance entities updated by order");
        }
        bool changed = false;
        const usize n = std::min(incoming.size(), link.entities.size());
        for (usize i = 0; i < n; ++i) {
            const ecs::EntityID e = link.entities[i];
            if (!reg.alive(e)) continue;
            EntityComponentSet cur = captureEntityComponents(reg, e);
            const EntityComponentSet before = cur;
            forEachSlot(cur, link.baseline[i], incoming[i], [&](usize slot, const char*, auto& c, const auto& b, const auto& in) {
                if (i == 0 && slot == kTransformSlot) return;
                if (!sameOpt(c, b)) return; // overridden: keep
                if constexpr (std::is_same_v<std::decay_t<decltype(c)>, std::optional<ecs::Transform>>) {
                    if (c && in) { // keep hierarchy links, take local TRS
                        c->position = in->position;
                        c->rotation = in->rotation;
                        c->scale = in->scale;
                        c->dirty = true;
                        return;
                    }
                }
                c = in;
            });
            // Baseline = new prefab values (overrides keep differing from it).
            EntityComponentSet base = incoming[i];
            if (auto& t = std::get<std::optional<ecs::Transform>>(base); t) {
                if (const auto& ct = std::get<std::optional<ecs::Transform>>(cur); ct) t->parent = ct->parent;
            }
            link.baseline[i] = base;
            forEachSlot(cur, before, before, [&](usize, const char*, auto& c, const auto& b, const auto&) {
                changed = changed || !sameOpt(c, b);
            });
            restoreEntityComponents(reg, e, cur);
        }
        if (changed) {
            ++updated;
            m_state.sceneModified = true;
        }
    }
    return updated;
}

bool EditorHost::revertPrefabOverrides(ecs::EntityID entity) {
    usize index = 0;
    if (findPrefabLink_(entity, &index) == nullptr) return false;
    PrefabLink& link = m_prefabLinks[index];
    ecs::Registry& reg = m_editorScene.registry();
    for (usize i = 0; i < link.entities.size(); ++i) {
        const ecs::EntityID e = link.entities[i];
        if (!reg.alive(e)) continue;
        EntityComponentSet cur = captureEntityComponents(reg, e);
        forEachSlot(cur, link.baseline[i], link.baseline[i], [&](usize slot, const char*, auto& c, const auto& b, const auto&) {
            if (i == 0 && slot == kTransformSlot) return;
            if constexpr (std::is_same_v<std::decay_t<decltype(c)>, std::optional<ecs::Transform>>) {
                if (c && b) {
                    c->position = b->position;
                    c->rotation = b->rotation;
                    c->scale = b->scale;
                    c->dirty = true;
                    return;
                }
            }
            c = b;
        });
        restoreEntityComponents(reg, e, cur);
    }
    m_state.sceneModified = true;
    postConsoleOutput(fuse::log::Level::Info, "reverted prefab overrides (" + link.path + ")");
    return true;
}

bool EditorHost::unlinkPrefabInstance(ecs::EntityID entity) {
    usize index = 0;
    if (findPrefabLink_(entity, &index) == nullptr) return false;
    m_prefabLinks.erase(m_prefabLinks.begin() + static_cast<std::ptrdiff_t>(index));
    m_state.sceneModified = true;
    return true;
}

namespace {

struct PrefabImage {
    std::vector<ecs::EntityID> ids; ///< file-registry ids, Transform iteration order
    std::vector<EntityComponentSet> sets;
};

bool loadPrefabImage(const std::string& path, PrefabImage& out, std::string& error) {
    scene::Scene loadedScene;
    ecs::Registry in;
    in.init(4096);
    const scene::SerialiseResult r = scene::SceneSerialiser::loadWithRegistry(path, loadedScene, in);
    if (r.status != scene::SerialiseStatus::Ok) {
        error = r.error;
        return false;
    }
    in.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform&) {
        out.ids.push_back(id);
        out.sets.push_back(captureEntityComponents(in, id));
    });
    return true;
}

bool writePrefabImage(const std::string& path, const PrefabImage& image, scene::SceneDimension dim, std::string& error) {
    ecs::Registry out;
    out.init(4096);
    const auto key = [](ecs::EntityID e) { return (static_cast<u64>(e.generation) << 32) | e.index; };
    std::unordered_map<u64, ecs::EntityID> remap;
    std::vector<ecs::EntityID> created;
    for (ecs::EntityID id : image.ids) {
        created.push_back(out.create());
        remap.emplace(key(id), created.back());
    }
    for (usize i = 0; i < image.sets.size(); ++i) {
        EntityComponentSet c = image.sets[i];
        if (auto& t = std::get<std::optional<ecs::Transform>>(c); t) {
            const auto it = t->parent.valid() ? remap.find(key(t->parent)) : remap.end();
            t->parent = it == remap.end() ? ecs::EntityID::null() : it->second;
            t->dirty = true;
        }
        restoreEntityComponents(out, created[i], c);
    }
    scene::Scene empty;
    const scene::SerialiseResult r = scene::SceneSerialiser::saveWithRegistry(empty, out, path, dim);
    if (r.status != scene::SerialiseStatus::Ok) {
        error = r.error;
        return false;
    }
    return true;
}

std::string readVariantBase(const std::string& variantPath) {
    std::ifstream in(variantPath + ".variant");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("base ", 0) == 0) return line.substr(5);
    }
    return {};
}

} // namespace

std::string EditorHost::prefabVariantBase(const std::string& path) { return readVariantBase(path); }

bool EditorHost::applyPrefabOverrides(ecs::EntityID entity) {
    usize index = 0;
    if (m_state.playing || findPrefabLink_(entity, &index) == nullptr) return false;
    const PrefabLink link = m_prefabLinks[index];
    ecs::Registry& reg = m_editorScene.registry();
    const ecs::EntityID root = link.entities.front();
    ecs::Transform* t = reg.get<ecs::Transform>(root);
    if (t == nullptr) return false;
    // The prefab keeps its own root placement; the instance keeps its placement.
    const ecs::Transform placed = *t;
    if (const auto& b = std::get<std::optional<ecs::Transform>>(link.baseline.front()); b) {
        t->position = b->position;
        t->rotation = b->rotation;
        t->scale = b->scale;
    }
    const bool ok = savePrefab(root, link.path);
    if (ecs::Transform* t2 = reg.get<ecs::Transform>(root)) {
        t2->position = placed.position;
        t2->rotation = placed.rotation;
        t2->scale = placed.scale;
        t2->dirty = true;
    }
    if (ok) postConsoleOutput(fuse::log::Level::Info, "applied overrides to " + link.path);
    return ok;
}

bool EditorHost::savePrefabVariant(ecs::EntityID entity, const std::string& variantPath) {
    usize index = 0;
    if (variantPath.empty() || findPrefabLink_(entity, &index) == nullptr) {
        return fileFail_(CommandKind::SavePrefab, variantPath, "save prefab variant: select a linked prefab instance");
    }
    const std::string basePath = m_prefabLinks[index].path;
    if (basePath == variantPath) {
        return fileFail_(CommandKind::SavePrefab, variantPath, "save prefab variant: pick a new path");
    }
    std::error_code ec;
    std::filesystem::copy_file(basePath, variantPath + ".variantbase", std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return fileFail_(CommandKind::SavePrefab, variantPath, "save prefab variant: " + ec.message());
    {
        std::ofstream side(variantPath + ".variant", std::ios::trunc);
        side << "fuse-prefabvariant 1\nbase " << basePath << "\n";
    }
    // Write the variant from the instance (root placement as in the base), then relink to it.
    m_prefabLinks[index].path = variantPath;
    const ecs::EntityID root = m_prefabLinks[index].entities.front();
    const bool ok = applyPrefabOverrides(root);
    if (ok) {
        std::vector<EntityComponentSet> fresh;
        std::string error;
        PrefabLink& l = m_prefabLinks[index];
        if (loadPrefabSet_(variantPath, fresh, error) && fresh.size() == l.entities.size()) {
            for (usize i = 0; i < fresh.size(); ++i) {
                if (auto& ft = std::get<std::optional<ecs::Transform>>(fresh[i]); ft) {
                    if (const ecs::Transform* ct = m_editorScene.registry().get<ecs::Transform>(l.entities[i])) ft->parent = ct->parent;
                }
            }
            l.baseline = std::move(fresh);
        }
        postConsoleOutput(fuse::log::Level::Info, "saved prefab variant " + variantPath + " (base " + basePath + ")");
    } else {
        m_prefabLinks[index].path = basePath;
    }
    return ok;
}

usize EditorHost::updatePrefabVariants(const std::string& basePath, int depth) {
    if (depth > 8) return 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(basePath).parent_path();
    if (dir.empty() || !fs::is_directory(dir, ec)) return 0;
    usize rewritten = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        const std::string sidecar = it->path().string();
        if (sidecar.size() < 8 || sidecar.compare(sidecar.size() - 8, 8, ".variant") != 0) continue;
        const std::string variant = sidecar.substr(0, sidecar.size() - 8);
        const std::string base = readVariantBase(variant);
        if (base.empty() || !fs::equivalent(base, basePath, ec)) continue;
        PrefabImage v, snap, b;
        std::string error;
        if (!loadPrefabImage(variant, v, error) || !loadPrefabImage(variant + ".variantbase", snap, error) ||
            !loadPrefabImage(basePath, b, error)) {
            postConsoleOutput(fuse::log::Level::Warn, "prefab variant " + variant + ": " + error);
            continue;
        }
        const usize n = std::min({v.sets.size(), snap.sets.size(), b.sets.size()});
        for (usize i = 0; i < n; ++i) {
            forEachSlot(v.sets[i], snap.sets[i], b.sets[i], [&](usize, const char*, auto& c, const auto& s, const auto& in) {
                if constexpr (std::is_same_v<std::decay_t<decltype(c)>, std::optional<ecs::Transform>>) {
                    if (c && s && in) {
                        if (std::memcmp(&c->position, &s->position, sizeof(c->position)) == 0 &&
                            std::memcmp(&c->rotation, &s->rotation, sizeof(c->rotation)) == 0 &&
                            std::memcmp(&c->scale, &s->scale, sizeof(c->scale)) == 0) {
                            c->position = in->position;
                            c->rotation = in->rotation;
                            c->scale = in->scale;
                        }
                        return;
                    }
                }
                if (sameOpt(c, s)) c = in; // not overridden by the variant: follow the base
            });
        }
        if (!writePrefabImage(variant, v, m_sceneDimension, error)) {
            postConsoleOutput(fuse::log::Level::Warn, "prefab variant " + variant + " write failed: " + error);
            continue;
        }
        fs::copy_file(basePath, variant + ".variantbase", fs::copy_options::overwrite_existing, ec);
        ++rewritten;
        reloadPrefabInstances(variant);
        rewritten += updatePrefabVariants(variant, depth + 1);
    }
    return rewritten;
}

// Sidecar: "link <path>" then one "e <index> <generation>" line per instance entity (prefab order).
// Baselines are rebuilt from the prefab file on load, so overrides are relative to its current values.
void EditorHost::savePrefabLinks_(const std::string& scenePath) const {
    const std::string sidecar = prefabLinksSidecarPath(scenePath);
    if (m_prefabLinks.empty()) {
        std::remove(sidecar.c_str());
        return;
    }
    std::ofstream out(sidecar, std::ios::trunc);
    out << "fuse-prefablinks 1\n";
    for (const PrefabLink& l : m_prefabLinks) {
        out << "link " << l.path << "\n";
        for (ecs::EntityID e : l.entities) out << "e " << e.index << ' ' << e.generation << "\n";
    }
}

void EditorHost::loadPrefabLinks_(const std::string& scenePath) {
    m_prefabLinks.clear();
    std::ifstream in(prefabLinksSidecarPath(scenePath));
    if (!in) return;
    std::string line;
    const ecs::Registry& reg = m_editorScene.registry();
    while (std::getline(in, line)) {
        if (line.rfind("link ", 0) == 0) {
            m_prefabLinks.push_back(PrefabLink{line.substr(5), {}, {}});
        } else if (line.rfind("e ", 0) == 0 && !m_prefabLinks.empty()) {
            std::istringstream ss(line.substr(2));
            ecs::EntityID e{};
            ss >> e.index >> e.generation;
            m_prefabLinks.back().entities.push_back(e);
        }
    }
    for (auto it = m_prefabLinks.begin(); it != m_prefabLinks.end();) {
        std::vector<EntityComponentSet> base;
        std::string error;
        bool ok = !it->entities.empty() && reg.alive(it->entities.front()) && loadPrefabSet_(it->path, base, error);
        if (ok) {
            base.resize(it->entities.size(), base.empty() ? EntityComponentSet{} : base.back());
            for (usize i = 0; i < it->entities.size(); ++i) {
                if (auto& t = std::get<std::optional<ecs::Transform>>(base[i]); t && reg.alive(it->entities[i])) {
                    if (const ecs::Transform* ct = reg.get<ecs::Transform>(it->entities[i])) t->parent = ct->parent;
                }
            }
            it->baseline = std::move(base);
            ++it;
        } else {
            postConsoleOutput(fuse::log::Level::Warn, "prefab link to " + it->path + " dropped (missing prefab or entities)");
            it = m_prefabLinks.erase(it);
        }
    }
}

} // namespace fuse::editor
