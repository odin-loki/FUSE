// FUSE Relight RL-3.5: the Logic graph runtime (see logic_runtime.hpp).
#include <fuse/relight/logic/logic_runtime.hpp>
#include <fuse/relight/logic/logic_log.hpp>
#include <fuse/relight/logic/logic_options.hpp>

#include <fuse/relight/options/option_manager.hpp>

#include <cinttypes>
#include <cstdio>
#include <set>

namespace fuse::relight::logic {

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &LogicOptions::enable, &LogicOptions::pauseGraphUpdates, &LogicOptions::fixedDeltaTime,
    &LogicOptions::keyboard, &LogicOptions::recordValues,
};

std::string jsonString(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string jsonFloat(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
    return buf;
}

std::string relativeTo(const std::string& path, const std::string& base) {
    if (!base.empty() && path.size() > base.size() && path.compare(0, base.size(), base) == 0 && path[base.size()] == '/') {
        return path.substr(base.size() + 1);
    }
    return path;
}

std::string lastSegment(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

void registerLogicOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
}

LogicRunOptions LogicRunOptions::fromOptions() {
    registerLogicOptions();
    LogicRunOptions o;
    o.enabled = LogicOptions::enable();
    o.paused = LogicOptions::pauseGraphUpdates();
    o.recordValues = LogicOptions::recordValues();
    return o;
}

LogicRuntime::LogicRuntime() { registerAllComponents(); }

LogicRuntime::~LogicRuntime() { clear(); }

void LogicRuntime::clear() {
    m_manager.clear();
    m_owners.clear();
}

void LogicRuntime::setModGraphs(std::vector<ModGraphs> mods) {
    clear();
    m_mods = std::move(mods);
}

const ReplacementGraphs* LogicRuntime::find(const std::string& mod, std::uint64_t hash) const {
    for (const ModGraphs& m : m_mods) {
        if (m.mod != mod) {
            continue;
        }
        const auto it = m.meshes.find(hash);
        return it == m.meshes.end() ? nullptr : &it->second;
    }
    return nullptr;
}

std::vector<std::uint64_t> LogicRuntime::instancesOf(std::uint64_t owner) const {
    const auto it = m_owners.find(owner);
    return it == m_owners.end() ? std::vector<std::uint64_t>{} : it->second.instanceIds;
}

void LogicRuntime::removeOwner(std::map<std::uint64_t, OwnerState>::iterator it, std::size_t& removed) {
    for (std::uint64_t id : it->second.instanceIds) {
        m_manager.removeInstance(id);
        ++removed;
    }
    m_owners.erase(it);
}

LogicFrameReport LogicRuntime::runFrame(const FrameInputs& inputs, const std::vector<GraphOwnerFrame>& owners,
                                        const LogicRunOptions& options) {
    LogicFrameReport report;
    report.frame = inputs.frame;
    report.enabled = options.enabled;
    report.paused = options.paused;
    for (const ModGraphs& m : m_mods) {
        report.graphStates += m.graphCount();
    }
    const LogicContext ctx(inputs);

    if (!options.enabled) {
        for (auto it = m_owners.begin(); it != m_owners.end();) {
            removeOwner(it++, report.removed);
        }
    } else {
        // 1. Owners that are gone, or whose graphs changed.
        std::map<std::uint64_t, const GraphOwnerFrame*> present;
        for (const GraphOwnerFrame& o : owners) {
            if (o.graphs != nullptr && !o.graphs->graphs.empty()) {
                present.emplace(o.owner, &o);
            }
        }
        for (auto it = m_owners.begin(); it != m_owners.end();) {
            const auto p = present.find(it->first);
            if (p == present.end() || p->second->graphs != it->second.graphs) {
                removeOwner(it++, report.removed);
            } else {
                ++it;
            }
        }
        // 2. New owners: one instance per graph, updated once when added.
        for (const auto& [owner, frameOwner] : present) {
            if (m_owners.count(owner) != 0) {
                continue;
            }
            OwnerState state;
            state.graphs = frameOwner->graphs;
            state.mod = frameOwner->mod;
            for (const auto& graph : frameOwner->graphs->graphs) {
                if (GraphInstance* instance = m_manager.addInstance(ctx, graph, owner)) {
                    state.instanceIds.push_back(instance->id());
                    ++report.added;
                }
            }
            m_owners.emplace(owner, std::move(state));
        }
        // 3. Every instance.
        m_manager.update(ctx, options.paused);
        m_manager.applySceneOverrides(ctx, options.paused);
    }
    // 4. Option layer requests take effect (for the next frame).
    if (options.applyOptionLayers) {
        options::OptionManager::applyPendingValues(nullptr, false);
    }

    report.owners = m_owners.size();
    report.instances = m_manager.instances().size();
    report.batches = m_manager.batches().size();
    report.layers = heldOptionLayers();
    if (options.recordValues) {
        for (const auto& [owner, state] : m_owners) {
            for (std::uint64_t id : state.instanceIds) {
                const GraphInstance* instance = m_manager.instance(id);
                const GraphBatch* batch = instance != nullptr ? m_manager.batchOf(*instance) : nullptr;
                if (batch == nullptr) {
                    continue;
                }
                LogicFrameReport::InstanceValues v;
                v.id = id;
                v.owner = owner;
                v.mod = state.mod;
                v.graph = instance->initialState().primPath;
                const GraphTopology& t = batch->topology();
                for (std::size_t c = 0; c < t.componentSpecs.size(); ++c) {
                    const ComponentSpec& spec = *t.componentSpecs[c];
                    for (std::size_t p = 0; p < spec.properties.size() && p < t.propertyIndices[c].size(); ++p) {
                        const PropertySpec& prop = spec.properties[p];
                        if (prop.ioType != PropertyIOType::Output) {
                            continue;
                        }
                        v.outputs.emplace_back(lastSegment(t.nodePaths[c]) + "." + prop.name,
                                               formatPropertyValue(batch->value(t.propertyIndices[c][p], instance->batchIndex()),
                                                                   prop.type));
                    }
                }
                report.values.push_back(std::move(v));
            }
        }
    }
    return report;
}

std::string logicFrameJson(const LogicFrameReport& report, const std::string& pathBase) {
    std::string out;
    out += "\"enabled\":" + std::string(report.enabled ? "true" : "false");
    out += ",\"paused\":" + std::string(report.paused ? "true" : "false");
    out += ",\"graphs\":" + std::to_string(report.graphStates);
    out += ",\"owners\":" + std::to_string(report.owners);
    out += ",\"instances\":" + std::to_string(report.instances);
    out += ",\"batches\":" + std::to_string(report.batches);
    out += ",\"added\":" + std::to_string(report.added);
    out += ",\"removed\":" + std::to_string(report.removed);
    out += ",\"layers\":[";
    for (std::size_t i = 0; i < report.layers.size(); ++i) {
        const HeldOptionLayer& l = report.layers[i];
        out += i ? "," : "";
        out += "{\"config\":" + jsonString(relativeTo(l.configPath, pathBase)) + ",\"priority\":" + std::to_string(l.priority) +
               ",\"references\":" + std::to_string(l.references) + ",\"enabled\":" + (l.enabled ? "true" : "false") +
               ",\"strength\":" + jsonFloat(l.blendStrength) + ",\"threshold\":" + jsonFloat(l.blendThreshold) + "}";
    }
    out += "],\"values\":[";
    for (std::size_t i = 0; i < report.values.size(); ++i) {
        const LogicFrameReport::InstanceValues& v = report.values[i];
        out += i ? "," : "";
        out += "{\"id\":" + std::to_string(v.id) + ",\"owner\":" + std::to_string(v.owner) + ",\"mod\":" + jsonString(v.mod) +
               ",\"graph\":" + jsonString(v.graph) + ",\"outputs\":{";
        for (std::size_t k = 0; k < v.outputs.size(); ++k) {
            out += k ? "," : "";
            out += jsonString(v.outputs[k].first) + ":" + jsonString(v.outputs[k].second);
        }
        out += "}}";
    }
    out += "]";
    return out;
}

} // namespace fuse::relight::logic
