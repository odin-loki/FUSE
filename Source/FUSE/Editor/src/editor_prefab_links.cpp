// Linked prefab instances: baseline / override tracking, propagation and the `.prefablinks` sidecar.
#include <fuse/editor/editor_host.hpp>

#include <cstring>
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
