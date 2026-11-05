// Deterministic JSON dumps of the TorqueScript AST, per-file analysis, project call graph and datablock tree
// (plan §1.2 Track B, W1.6). Same input -> byte-identical output; keys are emitted in a fixed order, 2-space
// indentation, '\n' line endings.
#pragma once

#include "fuse/remaster/tscript/analysis.hpp"

#include <string>

namespace fuse::remaster::tscript {

[[nodiscard]] std::string ast_to_json(const Node& root);
[[nodiscard]] std::string diagnostics_to_json(const std::vector<Diagnostic>& diags);
[[nodiscard]] std::string analysis_to_json(const FileAnalysis& fa);
[[nodiscard]] std::string call_graph_to_json(const CallGraph& g);
[[nodiscard]] std::string datablock_tree_to_json(const DatablockTree& t);

} // namespace fuse::remaster::tscript
