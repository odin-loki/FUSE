// FUSE Relight RL-3.1: the canonical flattened dump (fixture expectations, USDA == USDC checks, and the usd-core
// cross-check in Tools/FUSE/Relight/usd_flatten_check.py, which writes the same shape from pxr.Usd).
//
// Line 1:   {"stage": {"defaultPrim": ..., "metadata": {...}, "customLayerData": {...}}}
// Per prim: {"path": ..., "specifier": ..., "type": ..., "active": ..., "instanceable": ..., "kind": ...,
//            "apiSchemas": [...], "variantSelections": {...}, "children": [...],
//            "attributes": {name: {"type", "custom", "uniform", "default"?, "timeSamples"?, "connections"?,
//                                  "metadata"?}}, "relationships": {name: [targets]}}
// Then (optional) one {"diagnostic": {...}} line per diagnostic, in report order.
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <algorithm>

namespace fuse::relight::mods::usd {

namespace {

std::string metadataJson(const std::vector<std::pair<std::string, Value>>& md, const std::string& base) {
    std::vector<std::pair<std::string, Value>> sorted = md;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string s = "{";
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        s += (i ? ", " : "") + jsonQuote(sorted[i].first) + ": " + valueToJson(sorted[i].second, base);
    }
    return s + "}";
}

std::string stringList(const std::vector<std::string>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        s += (i ? ", " : "") + jsonQuote(v[i]);
    }
    return s + "]";
}

const char* severityName(Diagnostic::Severity s) { return s == Diagnostic::Severity::Error ? "error" : "warning"; }

} // namespace

std::string canonicalDump(const ComposedStage& stage, bool includeDiagnostics) {
    const std::string base = parentDir(stage.rootLayer);
    std::string out = "{\"stage\": {\"defaultPrim\": " + jsonQuote(stage.defaultPrim) +
                      ", \"metadata\": " + metadataJson(stage.layerMetadata, base) +
                      ", \"customLayerData\": " + valueToJson(stage.customLayerData, base) + "}}\n";
    for (const auto& [path, p] : stage.prims) {
        std::string line = "{\"path\": " + jsonQuote(path) + ", \"specifier\": " + jsonQuote(specifierName(p.specifier)) +
                           ", \"type\": " + jsonQuote(p.typeName) + ", \"active\": " + (p.active ? "true" : "false") +
                           ", \"instanceable\": " + (p.instanceable ? "true" : "false") + ", \"kind\": " + jsonQuote(p.kind) +
                           ", \"apiSchemas\": " + stringList(p.apiSchemas);
        line += ", \"variantSelections\": {";
        bool first = true;
        for (const auto& [set, sel] : p.variantSelections) {
            line += (first ? "" : ", ") + jsonQuote(set) + ": " + jsonQuote(sel);
            first = false;
        }
        line += "}, \"children\": " + stringList(p.children) + ", \"attributes\": {";
        first = true;
        for (const auto& [name, a] : p.attributes) {
            line += (first ? "" : ", ") + jsonQuote(name) + ": {\"type\": " + jsonQuote(a.typeName) +
                    ", \"custom\": " + (a.custom ? "true" : "false") + ", \"uniform\": " + (a.uniform ? "true" : "false");
            if (a.hasDefault) {
                line += ", \"default\": " + valueToJson(a.defaultValue, base);
            }
            if (!a.timeSamples.empty()) {
                line += ", \"timeSamples\": [";
                for (std::size_t i = 0; i < a.timeSamples.size(); ++i) {
                    line += (i ? ", " : "") + std::string("[") + formatNumber(a.timeSamples[i].first) + ", " +
                            valueToJson(a.timeSamples[i].second, base) + "]";
                }
                line += "]";
            }
            if (!a.connections.empty()) {
                line += ", \"connections\": " + stringList(a.connections);
            }
            if (!a.metadata.empty()) {
                line += ", \"metadata\": " + metadataJson(a.metadata, base);
            }
            line += "}";
            first = false;
        }
        line += "}, \"relationships\": {";
        first = true;
        for (const auto& [name, r] : p.relationships) {
            line += (first ? "" : ", ") + jsonQuote(name) + ": " + stringList(r.targets);
            first = false;
        }
        line += "}}\n";
        out += line;
    }
    if (includeDiagnostics) {
        for (const Diagnostic& d : stage.diagnostics) {
            std::string layer = d.layer;
            if (!base.empty() && layer.compare(0, base.size() + 1, base + "/") == 0) {
                layer = layer.substr(base.size() + 1);
            }
            out += "{\"diagnostic\": {\"severity\": " + jsonQuote(severityName(d.severity)) + ", \"code\": " + jsonQuote(d.code) +
                   ", \"prim\": " + jsonQuote(d.primPath) + ", \"layer\": " + jsonQuote(layer) + "}}\n";
        }
    }
    return out;
}

} // namespace fuse::relight::mods::usd
