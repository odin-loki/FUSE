#pragma once

#include <fuse/ai/behavior_tree.hpp>

#include <string>

namespace fuse::ai {

/// Text load path for BT assets (Qt/editor and UAISK template bridge).
///
/// Format (one node per line, `#` comments allowed):
///   typeId [threshold=N] [flag=N] [loops=N] [scalar=N] [success=N] [fail=N] [abort=1] [require_board=1] [require_allies=1] [require_agent=1] [require_radius=1] [children=a,b] [hook=scriptName]
/// Final line may be: root=N
///
/// Ore alignment: BadBehaviour editor tree serialization (SimGroup children) → flat indices here.
bool loadTreeFromText(const std::string& text, BehaviorTree& outTree, std::string* errorOut = nullptr);

} // namespace fuse::ai
