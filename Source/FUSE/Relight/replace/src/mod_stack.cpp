// FUSE Relight RL-3.4: mod discovery, stacking order and the stacked index (see mod_stack.hpp).
#include <fuse/relight/replace/mod_stack.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <system_error>
#include <tuple>

namespace fuse::relight::replace {

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::vector<std::string> splitList(const std::string& list) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : list) {
        if (c == ',' || c == ';') {
            if (!trim(cur).empty()) {
                out.push_back(trim(cur));
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) {
        out.push_back(trim(cur));
    }
    return out;
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string baseName(const std::string& dir) {
    const std::size_t slash = dir.rfind('/');
    return slash == std::string::npos ? dir : dir.substr(slash + 1);
}

std::string lastSegment(const std::string& source) { return baseName(source); }

} // namespace

std::string normalizeDir(std::string dir) {
    std::replace(dir.begin(), dir.end(), '\\', '/');
    while (dir.size() > 1 && dir.back() == '/') {
        dir.pop_back();
    }
    return dir;
}

std::vector<ModRoot> parseModRootList(const std::string& list, ModKind fallback) {
    std::vector<ModRoot> out;
    for (const std::string& e : splitList(list)) {
        ModRoot r;
        r.kind = fallback;
        std::string dir = e;
        if (dir.compare(0, 5, "fuse:") == 0) {
            r.kind = ModKind::FuseNative;
            dir = dir.substr(5);
        } else if (dir.compare(0, 6, "remix:") == 0) {
            r.kind = ModKind::Remix;
            dir = dir.substr(6);
        }
        r.dir = normalizeDir(trim(dir));
        if (!r.dir.empty()) {
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<std::string> parseModOrder(const std::string& list) { return splitList(list); }

std::optional<ModFormat> modFormatOf(const std::string& dir) {
    for (const char* n : {"mod.usda", "mod.usdc", "mod.usd"}) {
        if (isFile(fs::path(dir) / n)) {
            return ModFormat::Usd;
        }
    }
    if (isFile(fs::path(dir) / "db" / "remaster_db.json")) {
        return ModFormat::Store;
    }
    return std::nullopt;
}

std::vector<ModLocation> discoverMods(const std::vector<ModRoot>& roots, const std::vector<ModRoot>& mods) {
    std::vector<ModLocation> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& dir, ModKind kind) {
        const auto format = modFormatOf(dir);
        if (!format) {
            return;
        }
        if (*format == ModFormat::Usd) {
            ModLocation l{baseName(dir), dir, kind, ModFormat::Usd, {}};
            if (seen.insert(l.id()).second) {
                out.push_back(std::move(l));
            }
            return;
        }
        for (const std::string& source : storeSources(dir)) {
            ModLocation l{lastSegment(source), dir, kind, ModFormat::Store, source};
            if (seen.insert(l.id()).second) {
                out.push_back(std::move(l));
            }
        }
    };
    for (const ModRoot& root : roots) {
        std::vector<std::string> children;
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(root.dir), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code ec2;
            if (it->is_directory(ec2)) {
                children.push_back(it->path().filename().generic_string());
            }
        }
        std::sort(children.begin(), children.end());
        for (const std::string& c : children) {
            add(normalizeDir(root.dir + "/" + c), root.kind);
        }
    }
    for (const ModRoot& m : mods) {
        add(m.dir, m.kind);
    }
    return out;
}

void sortStack(std::vector<const ModContent*>& mods, const std::vector<std::string>& modOrder) {
    auto rank = [&](const ModContent* m) {
        const auto it = std::find(modOrder.begin(), modOrder.end(), m->location.name);
        const std::size_t order = it == modOrder.end() ? modOrder.size() : std::size_t(it - modOrder.begin());
        const std::int64_t priority = m->priority.value_or(0);
        const int kind = m->location.kind == ModKind::FuseNative ? 0 : 1;
        return std::make_tuple(order, -priority, kind, m->location.name, m->location.dir, m->location.source);
    };
    std::stable_sort(mods.begin(), mods.end(), [&](const ModContent* a, const ModContent* b) { return rank(a) < rank(b); });
}

// ---- ReplacementIndex -------------------------------------------------------------------------------------------

void ReplacementIndex::clear() {
    m_stack.clear();
    m_rules.clear();
    m_meshes.clear();
    m_materials.clear();
    m_lights.clear();
}

void ReplacementIndex::build(const std::vector<const ModContent*>& stack) {
    clear();
    m_stack = stack;
    for (const ModContent* mod : stack) {
        if (!mod->ok) {
            continue;
        }
        for (const auto& [key, def] : mod->meshes) {
            MeshHit& h = m_meshes[key];
            if (!h.def) {
                h.def = &def;
                h.mod = mod;
                if (std::find(m_rules.begin(), m_rules.end(), def.rule) == m_rules.end()) {
                    m_rules.push_back(def.rule);
                }
            } else {
                h.shadowed.push_back(mod);
            }
        }
        for (const auto& [key, def] : mod->materials) {
            MaterialHit& h = m_materials[key];
            if (!h.def) {
                h.def = &def;
                h.mod = mod;
            } else {
                h.shadowed.push_back(mod);
            }
        }
        for (const auto& [key, def] : mod->lights) {
            LightHit& h = m_lights[key];
            if (!h.def) {
                h.def = &def;
                h.mod = mod;
            } else {
                h.shadowed.push_back(mod);
            }
        }
    }
}

const ReplacementIndex::MeshHit* ReplacementIndex::mesh(hash::HashRule rule, Hash64 key) const {
    const auto it = m_meshes.find({rule.bits, key});
    return it == m_meshes.end() ? nullptr : &it->second;
}

const ReplacementIndex::MaterialHit* ReplacementIndex::material(Hash64 textureHash) const {
    const auto it = m_materials.find(textureHash);
    return it == m_materials.end() ? nullptr : &it->second;
}

const ReplacementIndex::LightHit* ReplacementIndex::light(Hash64 lightHash) const {
    const auto it = m_lights.find(lightHash);
    return it == m_lights.end() ? nullptr : &it->second;
}

const MaterialDef* ReplacementIndex::boundMaterial(const ModContent* mod, const std::string& id) const {
    if (!mod || id.empty()) {
        return nullptr;
    }
    const auto it = mod->boundMaterials.find(id);
    return it == mod->boundMaterials.end() ? nullptr : &it->second;
}

namespace {

template <typename Map, typename KeyOf>
void diffMaps(const Map& a, const Map& b, std::vector<Hash64>& out, KeyOf keyOf) {
    std::set<Hash64> changed;
    for (const auto& [k, hit] : a) {
        const auto it = b.find(k);
        if (it == b.end() || it->second.mod->location.id() != hit.mod->location.id() ||
            it->second.def->fingerprint != hit.def->fingerprint) {
            changed.insert(keyOf(k));
        }
    }
    for (const auto& [k, hit] : b) {
        if (!a.count(k)) {
            changed.insert(keyOf(k));
        }
    }
    out.assign(changed.begin(), changed.end());
}

} // namespace

ReplacementIndex::Diff ReplacementIndex::diff(const ReplacementIndex& before, const ReplacementIndex& after) {
    Diff d;
    diffMaps(before.m_meshes, after.m_meshes, d.meshes, [](const std::pair<std::uint32_t, Hash64>& k) { return k.second; });
    diffMaps(before.m_materials, after.m_materials, d.materials, [](Hash64 k) { return k; });
    diffMaps(before.m_lights, after.m_lights, d.lights, [](Hash64 k) { return k; });
    return d;
}

} // namespace fuse::relight::replace
