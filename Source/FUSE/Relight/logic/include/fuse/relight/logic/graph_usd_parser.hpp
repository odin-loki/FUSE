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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_graph_usd_parser.h@0867d3c and rtx_graph_usd_parser.cpp@0867d3c,
// and the graph discovery of src/dxvk/rtx_render/rtx_mod_usd.cpp@0867d3c (processReplacement / processGraph).
//
// FUSE Relight RL-3.5: OmniGraph prims (as Remix mods author them) -> GraphState, on the RL-3.1 composed stage
// (mods/usd, the Remix profile) instead of a pxr::UsdStage.
//
// The authored form the parser reads:
//   def OmniGraph "<graph>"                       anywhere below a mesh replacement (mesh_<H>), not the root
//   {                                             itself (upstream processReplacementRecursive)
//       def OmniGraphNode "<node>"
//       {
//           custom token node:type = "lightspeed.trex.logic.<Component>"
//           custom int node:typeVersion = 1        required; newer than the runtime's -> the node is skipped
//           custom <type> inputs:<name> = <value>  typed values or tokens ("(1, 2, 3)", "0x1A2B", "true", enum names)
//           custom <type> inputs:<name>.connect = </.../<other node>.outputs:<name>>   a connection
//           custom rel inputs:<prim property> = </.../prim>                           a Prim target
//           custom <type> outputs:<name>
//       }
//   }
// Semantics kept from upstream: nodes sorted topologically (ties: by component type hash, then prim path, and the
// whole order reversed, exactly as getDAGSortedNodes); a connected input shares the source output's property
// index (types must match, else the connection is ignored); the last connection whose prim exists wins; renamed
// properties (oldUsdNames: the current name when authored, else the first authored old name) and renamed
// components (oldNames); flexible types resolved from the connection source, else the attribute's USD type, else
// the token text; Prim relationships resolve through the owner's prim table; cycles drop the nodes on them.
// Differences: numeric USD types convert to the property type (upstream requires the exact VtValue type and falls
// back to the default); an `asset`-typed AssetPath value is accepted (resolved by RL-3.1 against its layer).
#pragma once

#include <fuse/relight/logic/graph_types.hpp>
#include <fuse/relight/logic/logic_log.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::logic {

/// Prim path -> index in the owning replacement's prim table (upstream pathToOffsetMap).
using PrimTable = std::map<std::string, std::uint32_t>;

struct GraphDiagnostic {
    LogSeverity severity = LogSeverity::Error;
    std::string path; ///< prim or property path
    std::string message;
};

/// Parses one OmniGraph prim. Returns the state (never null; an empty topology when nothing loaded).
std::shared_ptr<const GraphState> parseGraph(const mods::usd::ComposedStage& stage, std::string_view graphPath,
                                             const PrimTable& primTable, std::vector<GraphDiagnostic>* diagnostics = nullptr);

/// Parser internals exposed for the unit tests (upstream GraphUsdParserTestApp).
namespace parser_detail {
struct DagNode {
    std::string path;
    const ComponentSpec* spec = nullptr;
};
std::vector<DagNode> dagSortedNodes(const mods::usd::ComposedStage& stage, std::string_view graphPath,
                                    std::vector<GraphDiagnostic>* diagnostics);
const ComponentSpec* componentSpecForPrim(const mods::usd::Prim& node, std::vector<GraphDiagnostic>* diagnostics);
bool versionCheck(const mods::usd::Prim& node, const ComponentSpec& spec, std::vector<GraphDiagnostic>* diagnostics);
std::string resolvePropertyName(const mods::usd::Prim& node, const PropertySpec& property);
PropertyType inferTypeFromTokenString(const std::string& token);
} // namespace parser_detail

/// The prims of one mesh replacement (mesh_<H>) that graphs can target, and its graphs.
struct ReplacementGraphs {
    std::string rootPath;   ///< /RootNode/meshes/mesh_<H>
    std::uint64_t hash = 0; ///< <H>
    bool preserveOriginalDrawCall = false;
    enum class PrimKind : std::uint8_t { OriginalMesh, Mesh, Light, Graph };
    struct PrimEntry {
        std::string path;
        PrimKind kind = PrimKind::Mesh;
    };
    std::vector<PrimEntry> prims; ///< the prim table in index order
    PrimTable primTable;          ///< path -> index into `prims`
    std::vector<std::shared_ptr<const GraphState>> graphs; ///< one per OmniGraph prim (composed order)
};

/// Every graph of a mod stage, by replacement root. Replacements without graphs are omitted.
struct ModGraphs {
    std::string mod;
    std::map<std::uint64_t, ReplacementGraphs> meshes; ///< by mesh hash
    std::vector<GraphDiagnostic> diagnostics;
    std::size_t graphCount() const;
};

/// Scans every /RootNode/meshes/mesh_<H> for OmniGraph prims (active prims only; not the replacement root itself;
/// not below point instancers, which upstream does not support) and parses them. Upstream attaches graphs to mesh
/// replacements only (light replacements are not replacement hierarchies).
/// The prim table follows processReplacement: index 0 is "<root>/mesh" (the original draw) when the root says
/// preserveOriginalDrawCall, then meshes, lights and graphs in depth-first composed order.
ModGraphs loadModGraphs(const mods::usd::ComposedStage& stage, std::string modName);

} // namespace fuse::relight::logic
