// FUSE Relight RL-3.4: mod discovery, stacking order and the stacked replacement index
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.2).
//
// Discovery. Every search root is a directory whose children are mods (upstream <game>/rtx-remix/mods/<Mod>; Relight
// also <game>/fuse-relight/mods/<Mod> for FUSE-native mods). A child holding mod.usda / mod.usdc / mod.usd is a USD
// (Remix) mod; a child holding db/remaster_db.json is an imported store (fuse_relight_import output), one mod per DB
// `source`. Explicit mods (relight.replace.mods) are single directories of either form.
//
// Stacking order (strongest first; the first mod in the order that has a replacement for a key wins it whole, as
// upstream AssetReplacer walks its mods by priority):
//   1. position in relight.modOrder (a comma list of mod names; listed mods above unlisted ones);
//   2. `relight.mod.priority = <int>` in the mod's own rtx.conf (higher first; absent = 0);
//   3. FUSE-native mods above Remix mods;
//   4. mod name, then directory and source (ascending), so the order is total and deterministic.
#pragma once

#include <fuse/relight/replace/mod_content.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fuse::relight::replace {

struct ModRoot {
    std::string dir;
    ModKind kind = ModKind::Remix;
};

/// Parses a relight.replace.modPaths / relight.replace.mods list: entries separated by ',' or ';', each
/// optionally prefixed "fuse:" (FUSE-native) or "remix:" (default `fallback`).
std::vector<ModRoot> parseModRootList(const std::string& list, ModKind fallback = ModKind::Remix);
/// Splits a relight.modOrder list (',' or ';', trimmed, empty entries dropped).
std::vector<std::string> parseModOrder(const std::string& list);

/// Normalizes a path for mod ids ('\\' -> '/', no trailing '/').
std::string normalizeDir(std::string dir);

/// The form of one directory: USD mod, store, or nullopt (neither).
std::optional<ModFormat> modFormatOf(const std::string& dir);

/// The mods under `roots` (children, sorted by name) and the explicit `mods` (in list order), as locations; store
/// directories expand to one location per DB source. Duplicate ids are dropped (first wins).
std::vector<ModLocation> discoverMods(const std::vector<ModRoot>& roots, const std::vector<ModRoot>& mods);

/// Sorts mods strongest first (see the header comment).
void sortStack(std::vector<const ModContent*>& mods, const std::vector<std::string>& modOrder);

/// The stacked index: per key the winning mod's replacement, plus the mods it shadows.
class ReplacementIndex {
public:
    template <typename T>
    struct Hit {
        const T* def = nullptr;
        const ModContent* mod = nullptr;
        std::vector<const ModContent*> shadowed; ///< weaker mods with a replacement for the same key
    };
    using MeshHit = Hit<MeshReplacementDef>;
    using MaterialHit = Hit<MaterialDef>;
    using LightHit = Hit<LightReplacementDef>;

    /// `stack`: strongest first (sortStack). Mods that are not ok contribute nothing.
    void build(const std::vector<const ModContent*>& stack);
    void clear();

    const MeshHit* mesh(hash::HashRule rule, Hash64 key) const;
    const MaterialHit* material(Hash64 textureHash) const;
    const LightHit* light(Hash64 lightHash) const;
    /// A bound material of `mod` (part materials), nullptr when unknown.
    const MaterialDef* boundMaterial(const ModContent* mod, const std::string& id) const;

    /// The distinct mesh rules of the stack (in first-use order).
    const std::vector<hash::HashRule>& meshRules() const { return m_rules; }
    const std::vector<const ModContent*>& stack() const { return m_stack; }
    std::size_t meshCount() const { return m_meshes.size(); }
    std::size_t materialCount() const { return m_materials.size(); }
    std::size_t lightCount() const { return m_lights.size(); }

    /// Keys whose winner (mod or content) differs between two indexes (per hash invalidation on reload).
    struct Diff {
        std::vector<Hash64> meshes, materials, lights;
        bool empty() const { return meshes.empty() && materials.empty() && lights.empty(); }
    };
    static Diff diff(const ReplacementIndex& before, const ReplacementIndex& after);

private:
    std::vector<const ModContent*> m_stack;
    std::vector<hash::HashRule> m_rules;
    std::map<std::pair<std::uint32_t, Hash64>, MeshHit> m_meshes;
    std::map<Hash64, MaterialHit> m_materials;
    std::map<Hash64, LightHit> m_lights;
};

} // namespace fuse::relight::replace
