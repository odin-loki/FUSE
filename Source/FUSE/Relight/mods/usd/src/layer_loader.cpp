// FUSE Relight RL-3.1: TinyUSDZ layer (USDA / USDC) -> FUSE LayerData. The only file that includes TinyUSDZ
// headers besides the fixture tool; values cross over as USDA text (value::pprint_value) parsed by usd_value.cpp.
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include "core/prim-spec.hh"
#include "layer.hh"
#include "pprint-enum.hh"
#include "timesamples-pprint.hh"
#include "tinyusdz.hh"
#include "value-pprint.hh"

#include "fuse_tinyusdz_version.h" // after tinyusdz.hh: checks the vendored header against the pin

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fuse::relight::mods::usd {

namespace tu = tinyusdz;

namespace {

ListOpKind listOpKind(tu::ListEditQual q) {
    switch (q) {
    case tu::ListEditQual::Prepend: return ListOpKind::Prepend;
    case tu::ListEditQual::Append: return ListOpKind::Append;
    case tu::ListEditQual::Add: return ListOpKind::Add;
    case tu::ListEditQual::Delete: return ListOpKind::Delete;
    case tu::ListEditQual::Order: return ListOpKind::Order;
    case tu::ListEditQual::ResetToExplicit:
    case tu::ListEditQual::Invalid: break;
    }
    return ListOpKind::Explicit;
}

class Converter {
public:
    explicit Converter(std::string layerDir) : m_dir(std::move(layerDir)) {}

    std::vector<std::string> problems;

    /// value::Value -> Value through its USDA text.
    Value value(const tu::value::Value& v, const char* where) {
        if (v.type_id() == tu::value::TYPE_ID_VALUEBLOCK) {
            return Value::none();
        }
        const std::string text = tu::value::pprint_value(v);
        return parse(text, where);
    }

    Value parse(const std::string& text, const char* where) {
        std::string err;
        auto parsed = parseValue(text, &err);
        if (!parsed) {
            problems.push_back(std::string(where) + ": cannot read value text '" + text.substr(0, 80) + "' (" + err + ")");
            return Value::makeString(text);
        }
        anchor(*parsed);
        return std::move(*parsed);
    }

    /// Asset paths anchored to the defining layer's directory.
    void anchor(Value& v) const {
        if (v.kind == Value::Kind::Asset) {
            v.resolved = anchorAssetPath(m_dir, v.text);
        }
        for (Value& i : v.items) {
            anchor(i);
        }
        for (auto& [k, i] : v.dict) {
            anchor(i);
        }
    }

    /// Dictionary (customData, customLayerData) -> Dict value, "type name" keys ("dictionary name" for nested).
    Value dictionary(const tu::CustomDataType& d) {
        Value out;
        out.kind = Value::Kind::Dict;
        for (const auto& [name, mv] : d) {
            if (auto nested = mv.get_value<tu::CustomDataType>()) {
                out.dict.emplace_back("dictionary " + name, dictionary(*nested));
            } else {
                out.dict.emplace_back(mv.type_name() + " " + name, value(mv.get_raw_value(), "dictionary"));
            }
        }
        return out;
    }

    /// MetadataBase entries -> (key, value), skipping keys the caller handles.
    void metadata(const tu::Dictionary& data, std::vector<std::pair<std::string, Value>>& out,
                  std::initializer_list<const char*> skip) {
        for (const auto& [key, mv] : data) {
            bool skipped = false;
            for (const char* s : skip) {
                skipped = skipped || key == s;
            }
            if (skipped) {
                continue;
            }
            if (auto nested = mv.get_value<tu::CustomDataType>()) {
                out.emplace_back(key, dictionary(*nested));
            } else {
                out.emplace_back(key, value(mv.get_raw_value(), key.c_str()));
            }
        }
    }

    PropertySpec property(const std::string& name, const tu::Property& p) {
        PropertySpec out;
        out.name = name;
        out.custom = p.has_custom();
        if (p.is_relationship()) {
            out.relationship = true;
            const tu::Relationship& rel = p.get_relationship();
            out.targetsOp = listOpKind(p.get_listedit_qual() != tu::ListEditQual::ResetToExplicit ? p.get_listedit_qual()
                                                                                                    : rel.get_listedit_qual());
            if (rel.is_blocked()) {
                out.hasTargets = true;
            } else if (rel.is_path()) {
                out.hasTargets = true;
                out.targets.push_back(rel.targetPath.full_path_name());
            } else if (rel.is_pathvector()) {
                out.hasTargets = true;
                for (const tu::Path& t : rel.targetPathVector) {
                    out.targets.push_back(t.full_path_name());
                }
            }
            metadata(rel.metas().data(), out.metadata, {});
            return out;
        }
        if (!p.is_attribute()) {
            return out;
        }
        const tu::Attribute& a = p.get_attribute();
        out.typeName = a.type_name();
        out.uniform = a.variability() == tu::Variability::Uniform;
        const tu::primvar::PrimVar& var = a.get_var();
        if (var.is_blocked()) {
            out.hasDefault = true;
        } else if (var.has_default()) {
            out.hasDefault = true;
            out.defaultValue = value(var.value_raw(), name.c_str());
        }
        if (var.has_timesamples()) {
            out.hasTimeSamples = true;
            const Value samples = parse(tu::pprint_timesamples(var.ts_raw()), name.c_str());
            for (const auto& [t, v] : samples.dict) {
                out.timeSamples.emplace_back(std::strtod(t.c_str(), nullptr), v);
            }
        }
        if (a.has_connections()) {
            out.hasConnections = true;
            for (const tu::Path& c : a.connections()) {
                out.targets.push_back(c.full_path_name());
            }
        }
        metadata(a.metas().data(), out.metadata, {});
        return out;
    }

    template <typename Item, typename F>
    void listOps(const nonstd::optional<std::vector<std::pair<tu::ListEditQual, std::vector<Item>>>>& src, F&& convert,
                 auto& out) {
        if (!src) {
            return;
        }
        for (const auto& [qual, items] : *src) {
            auto& op = out.emplace_back();
            op.kind = listOpKind(qual);
            for (const Item& i : items) {
                op.items.push_back(convert(i));
            }
        }
    }

    static ArcTarget arc(const tu::value::AssetPath& asset, const tu::Path& path, const tu::LayerOffset& offset) {
        ArcTarget t;
        t.asset = asset.GetAssetPath();
        if (path.is_valid() && !path.is_root_path() && !path.full_path_name().empty()) {
            t.primPath = path.full_path_name();
        }
        t.hasLayerOffset = offset._offset != 0.0 || offset._scale != 1.0;
        return t;
    }

    PrimSpecData prim(const tu::PrimSpec& ps, const std::string& nameOverride = {}) {
        PrimSpecData out;
        out.name = nameOverride.empty() ? ps.name() : nameOverride;
        switch (ps.specifier()) {
        case tu::Specifier::Over: out.specifier = Specifier::Over; break;
        case tu::Specifier::Class: out.specifier = Specifier::Class; break;
        default: out.specifier = Specifier::Def; break;
        }
        out.typeName = ps.typeName();
        const tu::PrimMeta& m = ps.metas();
        listOps(m.references, [](const tu::Reference& r) { return arc(r.asset_path, r.prim_path, r.layerOffset); }, out.references);
        listOps(m.payload, [](const tu::Payload& p) { return arc(p.asset_path, p.prim_path, p.layerOffset); }, out.payloads);
        listOps(m.variantSets, [](const std::string& s) { return s; }, out.variantSetNames);
        if (m.variants) {
            for (const auto& [set, sel] : *m.variants) {
                out.variantSelections.emplace_back(set, sel);
            }
        }
        if (const tu::APISchemas* api = m.get_apiSchemas_ptr()) {
            auto& op = out.apiSchemas.emplace_back();
            op.kind = listOpKind(api->listOpQual);
            for (const auto& [n, inst] : api->names) {
                op.items.push_back(tu::to_string(n) + (inst.empty() ? "" : ":" + inst));
            }
            for (const auto& [n, inst] : api->unknownSchemas) {
                op.items.push_back(n + (inst.empty() ? "" : ":" + inst));
            }
        }
        out.hasInherits = (m.inherits && !m.inherits->empty()) || (m.inheritPaths && !m.inheritPaths->empty());
        out.hasSpecializes = (m.specializes && !m.specializes->empty()) || (m.specializePaths && !m.specializePaths->empty());
        out.hasClips = m.has(tu::MetadataBase::kClips) || m.unregisteredMetas.count("clips") || m.unregisteredMetas.count("clipSets");
        out.hasRelocates = m.unregisteredMetas.count("relocates") != 0;
        if (m.has_active()) {
            out.active = m.get_active();
        }
        if (m.has(tu::MetadataBase::kInstanceable)) {
            if (auto b = m.get<bool>(tu::MetadataBase::kInstanceable)) {
                out.instanceable = *b;
            }
        }
        if (m.has_hidden()) {
            out.hidden = m.get_hidden();
        }
        if (m.has_kind()) {
            out.kind = m.get_kind_str();
        }
        metadata(m.data(), out.metadata,
                 {tu::MetadataBase::kActive, tu::MetadataBase::kInstanceable, tu::MetadataBase::kHidden, tu::PrimMetas::kKind});
        metadata(m.meta, out.metadata, {});
        for (const auto& [k, text] : m.unregisteredMetas) {
            out.metadata.emplace_back(k, parse(text, k.c_str()));
        }
        for (const auto& [name, p] : ps.props()) {
            out.properties.push_back(property(name, p));
        }
        for (const auto& [setName, vs] : ps.variantSets()) {
            auto& set = out.variantSets.emplace_back();
            set.first = setName;
            for (const auto& [variantName, vps] : vs.variantSet) {
                set.second.emplace_back(variantName, prim(vps, variantName));
            }
        }
        for (const tu::PrimSpec& c : ps.children()) {
            out.children.push_back(prim(c));
        }
        // Crate files record the authored child order in primChildren; TinyUSDZ builds children in path order.
        if (!m.primChildren.empty()) {
            auto rank = [&](const PrimSpecData& c) {
                for (std::size_t i = 0; i < m.primChildren.size(); ++i) {
                    if (m.primChildren[i].str() == c.name) {
                        return i;
                    }
                }
                return m.primChildren.size();
            };
            std::stable_sort(out.children.begin(), out.children.end(),
                             [&](const PrimSpecData& a, const PrimSpecData& b) { return rank(a) < rank(b); });
        }
        return out;
    }

private:
    std::string m_dir;
};

} // namespace

std::vector<std::pair<double, double>> stripSubLayerOffsets(std::string& text) {
    std::vector<std::pair<double, double>> offsets;
    if (text.compare(0, 5, "#usda") != 0) {
        return offsets;
    }
    std::size_t i = text.find('\n');
    auto skipWsAndComments = [&](std::size_t& k) {
        while (k < text.size()) {
            if (std::isspace(static_cast<unsigned char>(text[k]))) {
                ++k;
            } else if (text[k] == '#') {
                while (k < text.size() && text[k] != '\n') {
                    ++k;
                }
            } else {
                break;
            }
        }
    };
    if (i == std::string::npos) {
        return {};
    }
    skipWsAndComments(i);
    if (i >= text.size() || text[i] != '(') {
        return {};
    }
    // Find "subLayers" at depth 1 of the header block.
    const std::size_t key = text.find("subLayers", i);
    std::size_t firstPrim = text.size();
    for (const char* kw : {"\ndef ", "\nover ", "\nclass "}) {
        firstPrim = std::min(firstPrim, text.find(kw, i));
    }
    if (key == std::string::npos || key > firstPrim) {
        return {};
    }
    std::size_t k = text.find('[', key);
    if (k == std::string::npos) {
        return {};
    }
    bool changed = false;
    int depth = 0;
    for (++k; k < text.size(); ++k) {
        const char c = text[k];
        if (c == '@') { // asset path (also @@@...@@@): one sublayer
            offsets.emplace_back(0.0, 1.0);
            const bool triple = text.compare(k, 3, "@@@") == 0;
            const std::size_t e = text.find(triple ? "@@@" : "@", k + (triple ? 3 : 1));
            if (e == std::string::npos) {
                break;
            }
            k = e + (triple ? 2 : 0);
        } else if (c == '"' || c == '\'') {
            std::size_t e = k + 1;
            while (e < text.size() && text[e] != c) {
                e += text[e] == '\\' ? 2 : 1;
            }
            k = e;
        } else if (c == '(') {
            if (depth == 0) {
                std::size_t e = k;
                int d = 0;
                for (; e < text.size(); ++e) {
                    d += text[e] == '(' ? 1 : (text[e] == ')' ? -1 : 0);
                    if (d == 0) {
                        break;
                    }
                }
                if (e >= text.size()) {
                    break;
                }
                const std::string group = text.substr(k, e - k + 1);
                auto number = [&](const char* key, double fallback) {
                    const std::size_t at = group.find(key);
                    if (at == std::string::npos) {
                        return fallback;
                    }
                    const std::size_t eq = group.find('=', at);
                    return eq == std::string::npos ? fallback : std::strtod(group.c_str() + eq + 1, nullptr);
                };
                if (!offsets.empty()) {
                    offsets.back() = {number("offset", 0.0), number("scale", 1.0)};
                }
                text.erase(k, e - k + 1);
                changed = true;
                --k;
            } else {
                ++depth;
            }
        } else if (c == ']') {
            break;
        }
    }
    if (!changed) {
        offsets.clear();
    }
    return offsets;
}

namespace {

} // namespace

const char* specifierName(Specifier s) {
    switch (s) {
    case Specifier::Def: return "def";
    case Specifier::Over: return "over";
    case Specifier::Class: return "class";
    }
    return "def";
}

const char* listOpKindName(ListOpKind k) {
    switch (k) {
    case ListOpKind::Explicit: return "explicit";
    case ListOpKind::Prepend: return "prepend";
    case ListOpKind::Append: return "append";
    case ListOpKind::Add: return "add";
    case ListOpKind::Delete: return "delete";
    case ListOpKind::Order: return "order";
    }
    return "explicit";
}

FileSource diskFileSource() {
    return [](const std::string& path) -> std::optional<std::string> {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
}

FileSource memoryFileSource(std::map<std::string, std::string> files) {
    std::map<std::string, std::string> normalized;
    for (auto& [k, v] : files) {
        normalized[normalizePath(k)] = std::move(v);
    }
    return [fs = std::move(normalized)](const std::string& path) -> std::optional<std::string> {
        const auto it = fs.find(normalizePath(path));
        if (it == fs.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

std::optional<LayerData> loadLayer(const std::string& identifier, std::string_view bytes, std::string* err) {
    tu::Layer layer;
    std::string warn, error;
    tu::USDLoadOptions opts;
    opts.load_assets = false;
    opts.do_composition = false;
    std::string text;
    bool strippedOffsets = false;
    if (bytes.substr(0, 5) == "#usda") {
        text.assign(bytes);
        const auto offsets = stripSubLayerOffsets(text);
        strippedOffsets = std::any_of(offsets.begin(), offsets.end(), [](const auto& o) { return o.first != 0.0 || o.second != 1.0; });
        if (!offsets.empty()) {
            bytes = text;
        }
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (!tu::LoadLayerFromMemory(data, bytes.size(), identifier, &layer, &warn, &error, opts)) {
        if (err) {
            *err = error.empty() ? "TinyUSDZ could not read the layer" : error;
        }
        return std::nullopt;
    }
    LayerData out;
    out.identifier = normalizePath(identifier);
    out.format = bytes.substr(0, 8) == "PXR-USDC" ? "usdc" : "usda";
    Converter conv(parentDir(out.identifier));
    const tu::LayerMetas& m = layer.metas();
    out.defaultPrim = m.defaultPrim.str();
    out.subLayerOffsets = strippedOffsets;
    out.layerRelocates = !m.layerRelocates.empty();
    for (const tu::SubLayer& s : m.subLayers) {
        out.subLayers.push_back(s.assetPath.GetAssetPath());
        out.subLayerOffsets = out.subLayerOffsets || s.layerOffset._offset != 0.0 || s.layerOffset._scale != 1.0;
    }
    auto meta = [&](const char* key, const auto& attr) {
        if (attr.authored()) {
            out.metadata.emplace_back(key, Value::makeNumber(double(attr.get_value())));
        }
    };
    if (m.upAxis.authored()) {
        const tu::Axis a = m.upAxis.get_value();
        out.metadata.emplace_back("upAxis", Value::makeString(a == tu::Axis::Z ? "Z" : (a == tu::Axis::X ? "X" : "Y")));
    }
    meta("metersPerUnit", m.metersPerUnit);
    meta("timeCodesPerSecond", m.timeCodesPerSecond);
    meta("framesPerSecond", m.framesPerSecond);
    meta("startTimeCode", m.startTimeCode);
    meta("endTimeCode", m.endTimeCode);
    meta("kilogramsPerUnit", m.kilogramsPerUnit);
    if (m.customLayerDataAuthored || !m.customLayerData.empty()) {
        out.customLayerData = conv.dictionary(m.customLayerData);
    }
    // Root prim order: primChildren when the format recorded it, then the rest by name.
    std::vector<std::string> order;
    for (const tu::value::token& t : m.primChildren) {
        if (layer.primspecs().count(t.str()) != 0) {
            order.push_back(t.str());
        }
    }
    std::vector<std::string> rest;
    for (const auto& [name, ps] : layer.primspecs()) {
        if (std::find(order.begin(), order.end(), name) == order.end()) {
            rest.push_back(name);
        }
    }
    std::sort(rest.begin(), rest.end());
    order.insert(order.end(), rest.begin(), rest.end());
    for (const std::string& name : order) {
        out.pseudoRoot.children.push_back(conv.prim(layer.primspecs().at(name), name));
    }
    if (!conv.problems.empty() && err) {
        *err = conv.problems.front();
    }
    return out;
}

} // namespace fuse::relight::mods::usd
