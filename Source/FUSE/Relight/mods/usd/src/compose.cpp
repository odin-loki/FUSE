// FUSE Relight RL-3.1: the Remix-profile composition engine (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.3).
//
// A small Pcp: every composed prim has an index, a tree of nodes. A node is a site (a layer stack plus a prim
// path in it) and holds that site's prim specs, strongest layer first. The root node is the root layer stack;
// arcs (variants, references, payloads) add child nodes. Strength order is a depth-first walk of the tree
// (node before its children; siblings by arc type V < R < P, then direct before ancestral, then authored list
// order). A child prim's index is derived from its parent's (every node's site gains the child name: ancestral
// arcs), after which each node's own specs add their direct arcs. Variant selections are the strongest authored
// opinion across the prim's whole index, so an index is rebuilt until its selections stop changing.
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fuse::relight::mods::usd {

namespace {

enum class Arc : unsigned char { Root, Variant, Reference, Payload };

int arcOrder(Arc a) {
    switch (a) {
    case Arc::Root: return 0;
    case Arc::Variant: return 1;
    case Arc::Reference: return 2;
    case Arc::Payload: return 3;
    }
    return 4;
}

struct LayerStack {
    std::string rootId;
    std::vector<const LayerData*> layers; ///< strongest first
};

struct SpecRef {
    const LayerData* layer = nullptr;
    const PrimSpecData* spec = nullptr;
};

/// (from, to) prefix pairs applied in order; `identity` lets paths outside `from` through unchanged (internal
/// arcs keep the root identity, as Pcp does for arcs within one layer stack).
struct MapStep {
    std::string from, to;
    bool rootIdentity = false;
};

struct Node {
    const LayerStack* stack = nullptr;
    std::vector<SpecRef> specs;
    std::string nsPath;   ///< prim path in the site's namespace (variant selections not included)
    std::string specPath; ///< for SpecSite: nsPath with "{set=variant}" decorations
    std::vector<MapStep> map;
    Arc arc = Arc::Root;
    int nsDepth = 0;
    std::size_t listIndex = 0;
    /// (layer stack root, nsPath) of this node's site and of every arc ancestor's site (cycle detection).
    std::vector<std::pair<std::string, std::string>> chain;
    std::vector<Node> children;
};

bool hasPrefix(const std::string& path, const std::string& prefix) {
    if (prefix == "/") {
        return !path.empty() && path[0] == '/';
    }
    if (path.size() < prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return path.size() == prefix.size() || path[prefix.size()] == '/' || path[prefix.size()] == '.' ||
           path[prefix.size()] == '{';
}

std::optional<std::string> mapPath(const std::vector<MapStep>& map, std::string path) {
    for (const MapStep& s : map) {
        if (hasPrefix(path, s.from)) {
            path = s.to + path.substr(s.from.size());
        } else if (!s.rootIdentity) {
            return std::nullopt;
        }
    }
    return path;
}

std::string childPath(const std::string& parent, const std::string& name) {
    return parent == "/" ? "/" + name : parent + "/" + name;
}

const PrimSpecData* findChild(const PrimSpecData& spec, const std::string& name) {
    for (const PrimSpecData& c : spec.children) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

const PrimSpecData* findSpec(const LayerData& layer, const std::string& path) {
    if (path.empty() || path[0] != '/') {
        return nullptr;
    }
    const PrimSpecData* s = &layer.pseudoRoot;
    std::size_t b = 1;
    while (s && b < path.size()) {
        std::size_t e = path.find('/', b);
        if (e == std::string::npos) {
            e = path.size();
        }
        s = findChild(*s, path.substr(b, e - b));
        b = e + 1;
    }
    return s;
}

template <typename T>
void applyListOp(std::vector<T>& result, const ListOp<T>& op) {
    auto remove = [&](const T& item) { result.erase(std::remove(result.begin(), result.end(), item), result.end()); };
    switch (op.kind) {
    case ListOpKind::Explicit:
        result.clear();
        for (const T& i : op.items) {
            if (std::find(result.begin(), result.end(), i) == result.end()) {
                result.push_back(i);
            }
        }
        break;
    case ListOpKind::Delete:
        for (const T& i : op.items) {
            remove(i);
        }
        break;
    case ListOpKind::Add:
        for (const T& i : op.items) {
            if (std::find(result.begin(), result.end(), i) == result.end()) {
                result.push_back(i);
            }
        }
        break;
    case ListOpKind::Append:
        for (const T& i : op.items) {
            remove(i);
            result.push_back(i);
        }
        break;
    case ListOpKind::Prepend: {
        std::vector<T> front;
        for (const T& i : op.items) {
            if (std::find(front.begin(), front.end(), i) == front.end()) {
                front.push_back(i);
            }
        }
        for (const T& i : front) {
            remove(i);
        }
        result.insert(result.begin(), front.begin(), front.end());
        break;
    }
    case ListOpKind::Order:
        break; // reorder: not in the profile (diagnosed by the caller)
    }
}

/// Sdf applies one spec's list edits in a fixed order, whatever the authored order: explicit (replaces the
/// list), then delete, add, prepend, append (reorder is outside the profile).
int listOpRank(ListOpKind k) {
    switch (k) {
    case ListOpKind::Explicit: return 0;
    case ListOpKind::Delete: return 1;
    case ListOpKind::Add: return 2;
    case ListOpKind::Prepend: return 3;
    case ListOpKind::Append: return 4;
    case ListOpKind::Order: return 5;
    }
    return 6;
}

template <typename T>
std::vector<const ListOp<T>*> specOrder(const std::vector<ListOp<T>>& ops) {
    std::vector<const ListOp<T>*> out;
    for (const ListOp<T>& op : ops) {
        out.push_back(&op);
    }
    std::stable_sort(out.begin(), out.end(), [](const ListOp<T>* a, const ListOp<T>* b) { return listOpRank(a->kind) < listOpRank(b->kind); });
    return out;
}

class Composer {
public:
    Composer(const ReadOptions& options, ComposedStage& out) : m_opt(options), m_out(out) {
        if (!m_opt.files) {
            m_opt.files = diskFileSource();
        }
    }

    void run(const std::string& rootPath) {
        const std::string rootId = normalizePath(rootPath);
        m_out.rootLayer = rootId;
        const LayerStack* stack = layerStack(rootId, "");
        if (!stack) {
            return;
        }
        m_out.ok = true;
        const LayerData& root = *stack->layers.front();
        m_out.defaultPrim = root.defaultPrim;
        m_out.layerMetadata = root.metadata;
        m_out.customLayerData = root.customLayerData;

        Node pseudo;
        pseudo.stack = stack;
        pseudo.nsPath = "/";
        pseudo.specPath = "/";
        pseudo.chain = {{stack->rootId, "/"}};
        for (const LayerData* l : stack->layers) {
            pseudo.specs.push_back({l, &l->pseudoRoot});
        }
        m_out.rootPrims = childNames(pseudo);
        for (const std::string& name : m_out.rootPrims) {
            composePrim(pseudo, "/", name, 1);
        }
    }

private:
    // ---- layers --------------------------------------------------------------------------------------------

    const LayerData* layer(const std::string& id, const std::string& primPath) {
        if (auto it = m_layers.find(id); it != m_layers.end()) {
            return it->second.get();
        }
        std::unique_ptr<LayerData> data;
        const auto bytes = m_opt.files(id);
        if (!bytes) {
            diag(Diagnostic::Severity::Error, "missing_layer", primPath, id, "layer not found: " + id);
        } else {
            std::string err;
            auto loaded = loadLayer(id, *bytes, &err);
            if (!loaded) {
                diag(Diagnostic::Severity::Error, "layer_parse_error", primPath, id, err);
            } else {
                if (!err.empty()) {
                    diag(Diagnostic::Severity::Warning, "value_text", primPath, id, err);
                }
                data = std::make_unique<LayerData>(std::move(*loaded));
                m_out.layers.push_back(id);
            }
        }
        const LayerData* p = data.get();
        m_layers[id] = std::move(data);
        return p;
    }

    const LayerStack* layerStack(const std::string& id, const std::string& primPath) {
        if (auto it = m_stacks.find(id); it != m_stacks.end()) {
            return it->second.get();
        }
        const LayerData* root = layer(id, primPath);
        if (!root) {
            m_stacks[id] = nullptr;
            return nullptr;
        }
        auto stack = std::make_unique<LayerStack>();
        stack->rootId = id;
        std::vector<std::string> visiting;
        addSubLayers(*stack, root, visiting, primPath);
        const LayerStack* p = stack.get();
        m_stacks[id] = std::move(stack);
        return p;
    }

    void addSubLayers(LayerStack& stack, const LayerData* l, std::vector<std::string>& visiting, const std::string& primPath) {
        if (std::find(visiting.begin(), visiting.end(), l->identifier) != visiting.end()) {
            diag(Diagnostic::Severity::Error, "sublayer_cycle", primPath, l->identifier, "sublayer cycle through " + l->identifier);
            return;
        }
        if (std::find(stack.layers.begin(), stack.layers.end(), l) != stack.layers.end()) {
            return; // already in the stack (diamond): the stronger position wins
        }
        stack.layers.push_back(l);
        if (l->layerRelocates) {
            diag(Diagnostic::Severity::Warning, "relocates", primPath, l->identifier,
                 "layer relocates are outside the Remix profile and ignored");
        }
        if (l->subLayerOffsets) {
            diag(Diagnostic::Severity::Warning, "layer_offset_ignored", primPath, l->identifier,
                 "sublayer offsets are ignored by the Remix profile");
        }
        visiting.push_back(l->identifier);
        for (const std::string& s : l->subLayers) {
            const std::string id = anchorAssetPath(parentDir(l->identifier), s);
            if (const LayerData* sub = layer(id, primPath)) {
                addSubLayers(stack, sub, visiting, primPath);
            }
        }
        visiting.pop_back();
    }

    // ---- index construction ----------------------------------------------------------------------------------

    using Selections = std::map<std::string, std::string>;

    /// Direct arcs authored by `n`'s own specs: variants, references, payloads (recursively expanded).
    void expandDirect(Node& n, int depth, const Selections& sel, const std::string& stagePath, std::uint32_t arcDepth) {
        if (n.specs.empty()) {
            return;
        }
        // Variant sets authored on this site (union over its specs, strongest first).
        std::vector<std::string> setNames;
        for (const SpecRef& s : n.specs) {
            for (const auto& [set, variants] : s.spec->variantSets) {
                if (std::find(setNames.begin(), setNames.end(), set) == setNames.end()) {
                    setNames.push_back(set);
                }
            }
        }
        std::size_t listIndex = 0;
        for (const std::string& set : setNames) {
            std::string chosen;
            if (auto it = sel.find(set); it != sel.end()) {
                chosen = it->second;
            } else if (auto fb = m_opt.variantFallbacks.find(set); fb != m_opt.variantFallbacks.end()) {
                for (const std::string& f : fb->second) {
                    if (variantExists(n, set, f)) {
                        chosen = f;
                        break;
                    }
                }
            }
            if (chosen.empty()) {
                continue;
            }
            Node v;
            v.stack = n.stack;
            v.nsPath = n.nsPath;
            v.specPath = n.specPath + "{" + set + "=" + chosen + "}";
            v.map = n.map;
            v.arc = Arc::Variant;
            v.nsDepth = depth;
            v.listIndex = listIndex++;
            v.chain = n.chain;
            for (const SpecRef& s : n.specs) {
                for (const auto& [setName, variants] : s.spec->variantSets) {
                    if (setName != set) {
                        continue;
                    }
                    for (const auto& [variantName, vspec] : variants) {
                        if (variantName == chosen) {
                            v.specs.push_back({s.layer, &vspec});
                        }
                    }
                }
            }
            if (v.specs.empty()) {
                diag(Diagnostic::Severity::Warning, "variant_missing", stagePath, n.stack->rootId,
                     "variant selection " + set + "=" + chosen + " names no variant");
                continue;
            }
            expandDirect(v, depth, sel, stagePath, arcDepth);
            n.children.push_back(std::move(v));
        }
        addArcs(n, depth, sel, stagePath, arcDepth, Arc::Reference);
        if (m_opt.loadPayloads) {
            addArcs(n, depth, sel, stagePath, arcDepth, Arc::Payload);
        }
        std::stable_sort(n.children.begin(), n.children.end(), [](const Node& a, const Node& b) {
            if (arcOrder(a.arc) != arcOrder(b.arc)) {
                return arcOrder(a.arc) < arcOrder(b.arc);
            }
            if (a.nsDepth != b.nsDepth) {
                return a.nsDepth > b.nsDepth; // direct (deeper) before ancestral
            }
            return a.listIndex < b.listIndex;
        });
    }

    static bool variantExists(const Node& n, const std::string& set, const std::string& variant) {
        for (const SpecRef& s : n.specs) {
            for (const auto& [setName, variants] : s.spec->variantSets) {
                for (const auto& [variantName, vspec] : variants) {
                    if (setName == set && variantName == variant) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    struct AnchoredArc {
        ArcTarget target;
        std::string anchorDir; ///< directory of the layer that authored the item
        friend bool operator==(const AnchoredArc& a, const AnchoredArc& b) {
            return a.target.primPath == b.target.primPath &&
                   anchorAssetPath(a.anchorDir, a.target.asset) == anchorAssetPath(b.anchorDir, b.target.asset);
        }
    };

    void addArcs(Node& n, int depth, const Selections& sel, const std::string& stagePath, std::uint32_t arcDepth, Arc kind) {
        std::vector<AnchoredArc> items;
        for (auto it = n.specs.rbegin(); it != n.specs.rend(); ++it) { // weakest first
            const auto& ops = kind == Arc::Reference ? it->spec->references : it->spec->payloads;
            const std::string dir = parentDir(it->layer->identifier);
            for (const ListOp<ArcTarget>* opp : specOrder(ops)) {
                const ListOp<ArcTarget>& op = *opp;
                if (op.kind == ListOpKind::Order) {
                    diag(Diagnostic::Severity::Warning, "reorder", stagePath, it->layer->identifier,
                         "reorder of references / payloads is outside the Remix profile");
                }
                ListOp<AnchoredArc> anchored;
                anchored.kind = op.kind;
                for (const ArcTarget& t : op.items) {
                    anchored.items.push_back({t, dir});
                }
                applyListOp(items, anchored);
            }
        }
        std::size_t listIndex = 0;
        for (const AnchoredArc& a : items) {
            const char* what = kind == Arc::Reference ? "reference" : "payload";
            if (arcDepth + 1 > m_opt.maxArcDepth) {
                diag(Diagnostic::Severity::Error, "arc_depth", stagePath, n.stack->rootId,
                     std::string(what) + " nesting deeper than " + std::to_string(m_opt.maxArcDepth));
                continue;
            }
            if (a.target.hasLayerOffset) {
                diag(Diagnostic::Severity::Warning, "layer_offset_ignored", stagePath, n.stack->rootId,
                     std::string(what) + " layer offset ignored by the Remix profile");
            }
            const bool internal = a.target.asset.empty();
            const LayerStack* target = n.stack;
            if (!internal) {
                const std::string id = anchorAssetPath(a.anchorDir, a.target.asset);
                target = layerStack(id, stagePath);
                if (!target) {
                    continue; // diagnosed by layer()
                }
            }
            std::string targetPath = a.target.primPath;
            if (targetPath.empty()) {
                targetPath = target->layers.front()->defaultPrim.empty() ? "" : "/" + target->layers.front()->defaultPrim;
                if (targetPath.empty()) {
                    diag(Diagnostic::Severity::Error, "missing_default_prim", stagePath, target->rootId,
                         std::string(what) + " to a layer without defaultPrim and no prim path");
                    continue;
                }
            }
            const bool cycle = std::any_of(n.chain.begin(), n.chain.end(), [&](const auto& c) {
                return c.first == target->rootId && hasPrefix(c.second, targetPath); // the site or an ancestor of it
            });
            if (cycle) {
                diag(Diagnostic::Severity::Error, "arc_cycle", stagePath, target->rootId,
                     std::string(what) + " cycle through " + targetPath);
                continue;
            }
            Node r;
            r.stack = target;
            r.nsPath = targetPath;
            r.specPath = targetPath;
            r.map.push_back({targetPath, n.nsPath, internal});
            r.map.insert(r.map.end(), n.map.begin(), n.map.end());
            r.arc = kind;
            r.nsDepth = depth;
            r.listIndex = listIndex++;
            r.chain = n.chain;
            r.chain.emplace_back(target->rootId, targetPath);
            for (const LayerData* l : target->layers) {
                if (const PrimSpecData* s = findSpec(*l, targetPath)) {
                    r.specs.push_back({l, s});
                }
            }
            if (r.specs.empty()) {
                diag(Diagnostic::Severity::Error, "unresolved_arc_path", stagePath, target->rootId,
                     std::string(what) + " target " + targetPath + " not found");
                continue;
            }
            expandDirect(r, depth, sel, stagePath, arcDepth + 1);
            n.children.push_back(std::move(r));
        }
    }

    /// The node for child `name` of `parent`'s site, with its ancestral subtree and its direct arcs.
    Node childOf(const Node& parent, const std::string& name, int depth, const Selections& sel, const std::string& stagePath,
                 std::uint32_t arcDepth) {
        Node c;
        c.stack = parent.stack;
        c.nsPath = childPath(parent.nsPath, name);
        c.specPath = childPath(parent.specPath, name);
        c.map = parent.map;
        c.arc = parent.arc;
        c.nsDepth = parent.nsDepth;
        c.listIndex = parent.listIndex;
        c.chain = parent.chain;
        c.chain.back().second = childPath(c.chain.back().second, name);
        for (const SpecRef& s : parent.specs) {
            if (const PrimSpecData* child = findChild(*s.spec, name)) {
                c.specs.push_back({s.layer, child});
            }
        }
        for (const Node& k : parent.children) {
            Node kc = childOf(k, name, depth, sel, stagePath, arcDepth + (k.arc == Arc::Reference || k.arc == Arc::Payload ? 1u : 0u));
            if (!kc.specs.empty() || !kc.children.empty()) {
                c.children.push_back(std::move(kc));
            }
        }
        expandDirect(c, depth, sel, stagePath, arcDepth);
        return c;
    }

    struct Opinion {
        const LayerData* layer;
        const PrimSpecData* spec;
        const Node* node;
    };

    static void flatten(const Node& n, std::vector<Opinion>& out) {
        for (const SpecRef& s : n.specs) {
            out.push_back({s.layer, s.spec, &n});
        }
        for (const Node& c : n.children) {
            flatten(c, out);
        }
    }

    static Selections selectionsOf(const std::vector<Opinion>& ops) {
        Selections sel;
        for (const Opinion& o : ops) { // strongest first: first opinion wins
            for (const auto& [set, variant] : o.spec->variantSelections) {
                sel.emplace(set, variant);
            }
        }
        return sel;
    }

    static std::vector<std::string> childNames(const Node& n) {
        std::vector<Opinion> ops;
        flatten(n, ops);
        std::vector<std::string> names;
        for (auto it = ops.rbegin(); it != ops.rend(); ++it) { // weak to strong, first appearance keeps its slot
            for (const PrimSpecData& c : it->spec->children) {
                if (std::find(names.begin(), names.end(), c.name) == names.end()) {
                    names.push_back(c.name);
                }
            }
        }
        return names;
    }

    void composePrim(const Node& parent, const std::string& parentPath, const std::string& name, int depth) {
        const std::string path = childPath(parentPath, name);
        if (depth > 256) {
            diag(Diagnostic::Severity::Error, "namespace_depth", path, "", "prim namespace deeper than 256 levels");
            return;
        }
        Selections sel;
        Node index;
        for (int iteration = 0;; ++iteration) {
            index = childOf(parent, name, depth, sel, path, 0);
            std::vector<Opinion> ops;
            flatten(index, ops);
            Selections next = selectionsOf(ops);
            if (next == sel) {
                break;
            }
            if (iteration == 8) {
                diag(Diagnostic::Severity::Warning, "variant_selection_unstable", path, "",
                     "variant selections did not converge; using the last iteration");
                break;
            }
            sel = std::move(next);
        }
        std::vector<Opinion> ops;
        flatten(index, ops);
        if (ops.empty()) {
            return;
        }
        Prim prim;
        prim.path = path;
        prim.name = name;
        buildPrim(prim, ops, sel);
        // As on a UsdStage, an inactive prim's subtree is not populated.
        const std::vector<std::string> children = prim.active ? childNames(index) : std::vector<std::string>{};
        prim.children = children;
        m_out.prims[path] = std::move(prim);
        for (const std::string& c : children) {
            composePrim(index, path, c, depth + 1);
        }
    }

    void buildPrim(Prim& prim, const std::vector<Opinion>& ops, const Selections& sel) {
        const std::string& path = prim.path;
        bool typed = false, defined = false, activeSet = false, instSet = false, kindSet = false;
        std::set<std::string> diagnosed;
        auto outOfProfile = [&](bool flag, const char* code, const std::string& layerId, const char* what) {
            if (flag && diagnosed.insert(code).second) {
                diag(Diagnostic::Severity::Warning, code, path, layerId, std::string(what) + " is outside the Remix profile and ignored");
            }
        };
        for (const Opinion& o : ops) {
            const PrimSpecData& s = *o.spec;
            SpecSite site;
            site.layer = o.layer->identifier;
            site.path = o.node->specPath;
            for (const PropertySpec& p : s.properties) {
                site.propertyNames.push_back(p.name);
            }
            prim.stack.push_back(std::move(site));
            if (!typed && !s.typeName.empty()) {
                prim.typeName = s.typeName;
                typed = true;
            }
            if (!defined && s.specifier != Specifier::Over) {
                prim.specifier = s.specifier;
                defined = true;
            }
            if (!activeSet && s.active) {
                prim.active = *s.active;
                activeSet = true;
            }
            if (!instSet && s.instanceable) {
                prim.instanceable = *s.instanceable;
                instSet = true;
            }
            if (!kindSet && s.kind) {
                prim.kind = *s.kind;
                kindSet = true;
            }
            for (const auto& [k, v] : s.metadata) {
                if (std::none_of(prim.metadata.begin(), prim.metadata.end(), [&](const auto& e) { return e.first == k; })) {
                    prim.metadata.emplace_back(k, v);
                }
            }
            for (const auto& [set, variants] : s.variantSets) {
                auto& names = prim.variantSets[set];
                for (const auto& [variantName, vspec] : variants) {
                    if (std::find(names.begin(), names.end(), variantName) == names.end()) {
                        names.push_back(variantName);
                    }
                }
            }
            outOfProfile(s.hasInherits, "inherits", o.layer->identifier, "inherits");
            outOfProfile(s.hasSpecializes, "specializes", o.layer->identifier, "specializes");
            outOfProfile(s.hasClips, "value_clips", o.layer->identifier, "value clips");
            outOfProfile(s.hasRelocates, "relocates", o.layer->identifier, "relocates");
        }
        for (const auto& [set, names] : prim.variantSets) {
            if (auto it = sel.find(set); it != sel.end()) {
                prim.variantSelections[set] = it->second;
            } else if (auto fb = m_opt.variantFallbacks.find(set); fb != m_opt.variantFallbacks.end()) {
                for (const std::string& f : fb->second) {
                    if (std::find(names.begin(), names.end(), f) != names.end()) {
                        prim.variantSelections[set] = f;
                        break;
                    }
                }
            }
        }
        for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
            for (const ListOp<std::string>* op : specOrder(it->spec->apiSchemas)) {
                applyListOp(prim.apiSchemas, *op);
            }
        }
        // Properties: strongest opinion per field; relationship targets are list ops (weak to strong).
        std::vector<std::string> names;
        for (const Opinion& o : ops) {
            for (const PropertySpec& p : o.spec->properties) {
                if (std::find(names.begin(), names.end(), p.name) == names.end()) {
                    names.push_back(p.name);
                }
            }
        }
        for (const std::string& pname : names) {
            std::vector<std::pair<const PropertySpec*, const Opinion*>> specs;
            for (const Opinion& o : ops) {
                for (const PropertySpec& p : o.spec->properties) {
                    if (p.name == pname) {
                        specs.emplace_back(&p, &o);
                    }
                }
            }
            const PropertySpec& strongest = *specs.front().first;
            if (strongest.relationship) {
                Relationship rel;
                rel.custom = strongest.custom;
                std::vector<std::string> targets;
                // Opinions weaker than the strongest explicit list cannot contribute: start there.
                auto first = specs.rend();
                for (auto it = specs.begin(); it != specs.end(); ++it) {
                    if (it->first->relationship && it->first->hasTargets && it->first->targetsOp == ListOpKind::Explicit) {
                        first = std::make_reverse_iterator(it + 1);
                        break;
                    }
                }
                for (auto it = first; it != specs.rend(); ++it) {
                    const PropertySpec& p = *it->first;
                    if (!p.relationship || !p.hasTargets) {
                        continue;
                    }
                    ListOp<std::string> op;
                    op.kind = p.targetsOp;
                    for (const std::string& t : p.targets) {
                        if (auto m = mapTarget(*it->second, t, path, pname)) {
                            op.items.push_back(*m);
                        }
                    }
                    applyListOp(targets, op);
                }
                rel.targets = std::move(targets);
                prim.relationships[pname] = std::move(rel);
                continue;
            }
            Attribute attr;
            bool typeSet = false, valueSet = false, samplesSet = false, connSet = false;
            attr.custom = strongest.custom;
            attr.uniform = strongest.uniform;
            for (const auto& [p, o] : specs) {
                if (p->relationship) {
                    continue;
                }
                if (!typeSet && !p->typeName.empty()) {
                    attr.typeName = p->typeName;
                    typeSet = true;
                }
                if (!valueSet && p->hasDefault) {
                    attr.hasDefault = true;
                    attr.defaultValue = p->defaultValue;
                    valueSet = true;
                    if (attr.definingLayer.empty()) {
                        attr.definingLayer = o->layer->identifier;
                    }
                }
                if (!samplesSet && p->hasTimeSamples) {
                    attr.timeSamples = p->timeSamples;
                    samplesSet = true;
                    if (attr.definingLayer.empty()) {
                        attr.definingLayer = o->layer->identifier;
                    }
                }
                if (!connSet && p->hasConnections) {
                    for (const std::string& t : p->targets) {
                        if (auto m = mapTarget(*o, t, path, pname)) {
                            attr.connections.push_back(*m);
                        }
                    }
                    connSet = true;
                }
                for (const auto& [k, v] : p->metadata) {
                    if (std::none_of(attr.metadata.begin(), attr.metadata.end(), [&](const auto& e) { return e.first == k; })) {
                        attr.metadata.emplace_back(k, v);
                    }
                }
            }
            if (attr.definingLayer.empty()) {
                attr.definingLayer = specs.front().second->layer->identifier;
            }
            prim.attributes[pname] = std::move(attr);
        }
    }

    std::optional<std::string> mapTarget(const Opinion& o, const std::string& target, const std::string& path, const std::string& prop) {
        std::string t = target;
        if (t.empty()) {
            return std::nullopt;
        }
        if (t[0] != '/') {
            // Relative target: relative to the prim that authored it (in its site's namespace).
            t = normalizePath(o.node->nsPath + "/" + t);
        }
        auto mapped = mapPath(o.node->map, t);
        if (!mapped) {
            diag(Diagnostic::Severity::Warning, "target_outside_arc", path, o.layer->identifier,
                 prop + ": target " + target + " lies outside the referenced namespace and is dropped");
        }
        return mapped;
    }

    void diag(Diagnostic::Severity s, std::string code, std::string prim, std::string layerId, std::string message) {
        // An index is rebuilt while variant selections settle: report each finding once.
        if (!m_reported.insert(code + '\x1f' + prim + '\x1f' + layerId + '\x1f' + message).second) {
            return;
        }
        m_out.diagnostics.push_back({s, std::move(code), std::move(prim), std::move(layerId), std::move(message)});
    }

    ReadOptions m_opt;
    ComposedStage& m_out;
    std::unordered_map<std::string, std::unique_ptr<LayerData>> m_layers;
    std::unordered_map<std::string, std::unique_ptr<LayerStack>> m_stacks;
    std::unordered_set<std::string> m_reported;
};

} // namespace

bool Prim::hasApiSchema(std::string_view name) const {
    return std::find(apiSchemas.begin(), apiSchemas.end(), name) != apiSchemas.end();
}

const Attribute* Prim::attribute(std::string_view name) const {
    const auto it = attributes.find(std::string(name));
    return it == attributes.end() ? nullptr : &it->second;
}

const Relationship* Prim::relationship(std::string_view name) const {
    const auto it = relationships.find(std::string(name));
    return it == relationships.end() ? nullptr : &it->second;
}

const Prim* ComposedStage::find(std::string_view path) const {
    const auto it = prims.find(std::string(path));
    return it == prims.end() ? nullptr : &it->second;
}

std::size_t ComposedStage::count(Diagnostic::Severity s) const {
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [s](const Diagnostic& d) { return d.severity == s; }));
}

std::vector<const Prim*> ComposedStage::descendants(std::string_view path) const {
    std::vector<const Prim*> out;
    std::function<void(const Prim&)> walk = [&](const Prim& p) {
        for (const std::string& c : p.children) {
            if (const Prim* child = find(childPath(p.path, c))) {
                out.push_back(child);
                walk(*child);
            }
        }
    };
    if (path == "/" || path.empty()) {
        for (const std::string& r : rootPrims) {
            if (const Prim* p = find("/" + r)) {
                out.push_back(p);
                walk(*p);
            }
        }
    } else if (const Prim* p = find(path)) {
        walk(*p);
    }
    return out;
}

ComposedStage readStage(const std::string& rootLayerPath, const ReadOptions& options) {
    ComposedStage out;
    Composer c(options, out);
    c.run(rootLayerPath);
    return out;
}

} // namespace fuse::relight::mods::usd
