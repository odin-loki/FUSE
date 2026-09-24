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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_graph_batch.h@0867d3c, rtx_graph_batch.cpp@0867d3c,
// rtx_graph_instance.h@0867d3c and rtx_graph_manager.h@0867d3c
//
// FUSE Relight RL-3.5: graph instances, batches and the manager.
//   GraphBatch     every instance of one topology (graph hash): the property vectors (one element per instance)
//                  and one ComponentBatch per node, updated in topological order. addInstance appends the
//                  instance's initial values and runs one update of the new instance (initialize callbacks first,
//                  per component, as upstream); removeInstance swaps the last instance into the hole.
//   GraphInstance  one live instance (per replacement instance that carries the graph). owner() is the caller's
//                  key for the prim table the instance's Prim properties resolve against.
//   GraphManager   instances by id, batches by graph hash (ordered: deterministic update order), rtx.graph.enable /
//                  pauseGraphUpdates semantics (the caller passes them; logic_options.hpp).
#pragma once

#include <fuse/relight/logic/graph_types.hpp>
#include <fuse/relight/logic/logic_context.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace fuse::relight::logic {

class GraphManager;

class GraphInstance {
public:
    GraphInstance(std::uint64_t graphHash, std::size_t batchIndex, std::uint64_t id, std::uint64_t owner,
                  std::shared_ptr<const GraphState> initialState)
        : m_graphHash(graphHash), m_batchIndex(batchIndex), m_id(id), m_owner(owner),
          m_initialState(std::move(initialState)) {}
    GraphInstance(const GraphInstance&) = delete;
    GraphInstance& operator=(const GraphInstance&) = delete;

    std::uint64_t graphHash() const { return m_graphHash; }
    std::size_t batchIndex() const { return m_batchIndex; }
    void setBatchIndex(std::size_t i) { m_batchIndex = i; }
    std::uint64_t id() const { return m_id; }
    std::uint64_t owner() const { return m_owner; }
    const GraphState& initialState() const { return *m_initialState; }

private:
    std::uint64_t m_graphHash;
    std::size_t m_batchIndex;
    std::uint64_t m_id;
    std::uint64_t m_owner;
    std::shared_ptr<const GraphState> m_initialState;
};

class GraphBatch {
public:
    GraphBatch() = default;
    GraphBatch(const GraphBatch&) = delete;
    GraphBatch& operator=(const GraphBatch&) = delete;

    /// Creates the property vectors and component batches. Components keep references into this object: it must
    /// not move afterwards (GraphManager holds batches by pointer).
    void initialize(std::shared_ptr<const GraphTopology> topology);
    bool addInstance(const LogicContext& ctx, const GraphState& state, GraphInstance* instance);
    void removeInstance(GraphInstance* instance);
    void removeAllInstances();
    void update(const LogicContext& ctx);
    void applySceneOverrides(const LogicContext& ctx);

    std::size_t numInstances() const { return m_instances.size(); }
    const std::vector<GraphInstance*>& instances() const { return m_instances; }
    bool isEmpty() const { return m_componentBatches.empty(); }
    const std::vector<std::unique_ptr<ComponentBatch>>& componentBatches() const { return m_componentBatches; }
    const std::vector<PropertyVector>& properties() const { return m_properties; }
    std::vector<PropertyVector>& properties() { return m_properties; }
    const GraphTopology& topology() const { return *m_topology; }

    /// The prim a component of instance `batchIndex` targets (nullptr when unresolved).
    const PrimSnapshot* resolvePrimTarget(const LogicContext& ctx, std::size_t batchIndex, const PrimTarget& target) const;

    /// Current value of property `propertyIndex` for instance `batchIndex`.
    PropertyValue value(std::size_t propertyIndex, std::size_t batchIndex) const;

private:
    std::shared_ptr<const GraphTopology> m_topology;
    std::vector<std::unique_ptr<ComponentBatch>> m_componentBatches;
    std::vector<std::size_t> m_batchesWithSceneOverrides;
    std::vector<PropertyVector> m_properties;
    std::vector<GraphInstance*> m_instances;
};

class GraphManager {
public:
    GraphManager() = default;
    GraphManager(const GraphManager&) = delete;
    GraphManager& operator=(const GraphManager&) = delete;

    /// nullptr when the state has no topology or its values do not match it.
    GraphInstance* addInstance(const LogicContext& ctx, std::shared_ptr<const GraphState> state, std::uint64_t owner);
    void removeInstance(std::uint64_t instanceId);
    void clear();
    /// Updates every batch (graph hash order). No-op while paused.
    void update(const LogicContext& ctx, bool paused = false);
    void applySceneOverrides(const LogicContext& ctx, bool paused = false);

    const std::map<std::uint64_t, std::unique_ptr<GraphInstance>>& instances() const { return m_instances; }
    const std::map<std::uint64_t, std::unique_ptr<GraphBatch>>& batches() const { return m_batches; }
    GraphInstance* instance(std::uint64_t id) const;
    const GraphBatch* batchOf(const GraphInstance& instance) const;

private:
    std::map<std::uint64_t, std::unique_ptr<GraphBatch>> m_batches;
    std::map<std::uint64_t, std::unique_ptr<GraphInstance>> m_instances;
    std::uint64_t m_nextInstanceId = 1;
};

} // namespace fuse::relight::logic
