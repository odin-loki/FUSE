/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_graph_batch.cpp@0867d3c and rtx_graph_manager.h@0867d3c
#include <fuse/relight/logic/graph_batch.hpp>
#include <fuse/relight/logic/component_list.hpp>
#include <fuse/relight/logic/logic_log.hpp>

#include <string>

namespace fuse::relight::logic {

namespace {

template <typename T>
bool swapAndRemove(std::vector<T>& vec, std::size_t index) {
    if (index >= vec.size()) {
        return false;
    }
    if (index != vec.size() - 1) {
        vec[index] = std::move(vec.back());
    }
    vec.pop_back();
    return true;
}

bool swapAndRemove(PropertyVector& propVec, std::size_t index) {
    return std::visit([index](auto& vec) { return swapAndRemove(vec, index); }, propVec);
}

} // namespace

void GraphBatch::initialize(std::shared_ptr<const GraphTopology> topology) {
    m_topology = std::move(topology);
    const GraphTopology& t = *m_topology;
    m_properties.reserve(t.propertyTypes.size());
    for (PropertyType type : t.propertyTypes) {
        m_properties.push_back(propertyVectorFromType(type));
    }
    // Components keep references into m_properties: it is complete (and never resized) before they are created.
    for (std::size_t i = 0; i < t.componentSpecs.size(); i++) {
        if (t.componentSpecs[i] == nullptr) {
            logMessage(LogSeverity::Error, "Component spec at index " + std::to_string(i) + " is null");
            continue;
        }
        m_componentBatches.push_back(t.componentSpecs[i]->createComponentBatch(*this, m_properties, t.propertyIndices[i]));
        if (t.componentSpecs[i]->applySceneOverrides != nullptr) {
            m_batchesWithSceneOverrides.push_back(m_componentBatches.size() - 1);
        }
    }
}

bool GraphBatch::addInstance(const LogicContext& ctx, const GraphState& state, GraphInstance* instance) {
    if (instance == nullptr) {
        logMessage(LogSeverity::Error, "Cannot add null GraphInstance");
        return false;
    }
    if (state.values.size() != m_properties.size()) {
        logMessage(LogSeverity::Error, "GraphState had the wrong number of values. Expected: " + std::to_string(m_properties.size()) +
                                           " got: " + std::to_string(state.values.size()));
        return false;
    }
    // Validate every value before touching the vectors (upstream catches bad_variant_access half way through).
    for (std::size_t i = 0; i < m_properties.size(); i++) {
        const bool ok = std::visit(
            [&](const auto& vec) {
                using V = typename std::decay_t<decltype(vec)>::value_type;
                return std::holds_alternative<V>(state.values[i]);
            },
            m_properties[i]);
        if (!ok) {
            logMessage(LogSeverity::Error, "Graph " + state.primPath + " had a type mismatch when adding instance to property " +
                                               std::to_string(i));
            return false;
        }
    }
    instance->setBatchIndex(m_instances.size());
    m_instances.push_back(instance);
    for (std::size_t i = 0; i < m_properties.size(); i++) {
        std::visit(
            [&](auto& vec) {
                using V = typename std::decay_t<decltype(vec)>::value_type;
                vec.push_back(std::get<V>(state.values[i]));
            },
            m_properties[i]);
    }
    // Update the new instance once to fill in the initial values; components with an initialize callback are
    // initialized after the earlier components ran (their inputs are current), before their own first update.
    const std::size_t newIndex = m_instances.size() - 1;
    for (auto& batch : m_componentBatches) {
        const ComponentSpec* spec = batch->getSpec();
        if (spec != nullptr && spec->initialize != nullptr) {
            spec->initialize(ctx, *batch, newIndex);
        }
        batch->updateRange(ctx, newIndex, newIndex + 1);
    }
    return true;
}

void GraphBatch::removeInstance(GraphInstance* instance) {
    if (instance == nullptr) {
        logMessage(LogSeverity::Error, "Cannot remove null GraphInstance");
        return;
    }
    const std::size_t index = instance->batchIndex();
    if (index >= m_instances.size() || m_instances[index] != instance) {
        logMessage(LogSeverity::Error, "GraphInstance to remove has the wrong index.  Instance: " + std::to_string(instance->id()));
        return;
    }
    for (auto& batch : m_componentBatches) {
        const ComponentSpec* spec = batch->getSpec();
        if (spec != nullptr && spec->cleanup != nullptr) {
            spec->cleanup(*batch, index);
        }
    }
    for (PropertyVector& p : m_properties) {
        swapAndRemove(p, index);
    }
    swapAndRemove(m_instances, index);
    if (index < m_instances.size()) {
        m_instances[index]->setBatchIndex(index);
    }
}

void GraphBatch::removeAllInstances() {
    for (std::size_t index = m_instances.size(); index-- > 0;) {
        removeInstance(m_instances[index]);
    }
}

void GraphBatch::update(const LogicContext& ctx) {
    const std::size_t end = m_instances.size();
    for (auto& batch : m_componentBatches) {
        batch->updateRange(ctx, 0, end);
    }
}

void GraphBatch::applySceneOverrides(const LogicContext& ctx) {
    for (std::size_t batchIndex : m_batchesWithSceneOverrides) {
        m_componentBatches[batchIndex]->getSpec()->applySceneOverrides(ctx, *m_componentBatches[batchIndex], 0, m_instances.size());
    }
}

const PrimSnapshot* GraphBatch::resolvePrimTarget(const LogicContext& ctx, std::size_t batchIndex, const PrimTarget& target) const {
    if (batchIndex >= m_instances.size() || m_instances[batchIndex] == nullptr) {
        return nullptr;
    }
    return ctx.resolvePrim(m_instances[batchIndex]->owner(), target);
}

PropertyValue GraphBatch::value(std::size_t propertyIndex, std::size_t batchIndex) const {
    return std::visit([batchIndex](const auto& vec) -> PropertyValue { return vec.at(batchIndex); }, m_properties.at(propertyIndex));
}

// ---- GraphManager ---------------------------------------------------------------------------------------------------

GraphInstance* GraphManager::addInstance(const LogicContext& ctx, std::shared_ptr<const GraphState> state, std::uint64_t owner) {
    registerAllComponents();
    if (!state || state->topology == nullptr) {
        logMessage(LogSeverity::Error, "GraphState has no topology. Prim path: " + (state ? state->primPath : std::string("(null)")));
        return nullptr;
    }
    const std::uint64_t graphHash = state->topology->graphHash;
    auto it = m_batches.find(graphHash);
    if (it == m_batches.end()) {
        auto batch = std::make_unique<GraphBatch>();
        batch->initialize(state->topology);
        it = m_batches.emplace(graphHash, std::move(batch)).first;
    }
    const std::uint64_t id = m_nextInstanceId++;
    auto instance = std::make_unique<GraphInstance>(graphHash, 0, id, owner, state);
    GraphInstance* raw = instance.get();
    if (!it->second->addInstance(ctx, *state, raw)) {
        logMessage(LogSeverity::Error, "Failed to add GraphInstance to GraphBatch. Instance: " + std::to_string(id) +
                                           " Prim path: " + state->primPath);
        if (it->second->numInstances() == 0) {
            m_batches.erase(it);
        }
        return nullptr;
    }
    m_instances.emplace(id, std::move(instance));
    return raw;
}

void GraphManager::removeInstance(std::uint64_t instanceId) {
    const auto it = m_instances.find(instanceId);
    if (it == m_instances.end()) {
        logMessage(LogSeverity::Error, "GraphInstance to remove not found. Instance: " + std::to_string(instanceId));
        return;
    }
    const auto batchIt = m_batches.find(it->second->graphHash());
    if (batchIt != m_batches.end()) {
        batchIt->second->removeInstance(it->second.get());
        if (batchIt->second->numInstances() == 0) {
            m_batches.erase(batchIt);
        }
    }
    m_instances.erase(it);
}

void GraphManager::clear() {
    // Cleanup callbacks run (option layers are released), unlike upstream's clear(), which drops the batches and
    // leaves the layer references behind until process exit.
    for (auto& [hash, batch] : m_batches) {
        batch->removeAllInstances();
    }
    m_batches.clear();
    m_instances.clear();
}

void GraphManager::update(const LogicContext& ctx, bool paused) {
    if (paused) {
        return;
    }
    for (auto& [hash, batch] : m_batches) {
        batch->update(ctx);
    }
}

void GraphManager::applySceneOverrides(const LogicContext& ctx, bool paused) {
    if (paused) {
        return;
    }
    for (auto& [hash, batch] : m_batches) {
        batch->applySceneOverrides(ctx);
    }
}

GraphInstance* GraphManager::instance(std::uint64_t id) const {
    const auto it = m_instances.find(id);
    return it == m_instances.end() ? nullptr : it->second.get();
}

const GraphBatch* GraphManager::batchOf(const GraphInstance& instance) const {
    const auto it = m_batches.find(instance.graphHash());
    return it == m_batches.end() ? nullptr : it->second.get();
}

} // namespace fuse::relight::logic
