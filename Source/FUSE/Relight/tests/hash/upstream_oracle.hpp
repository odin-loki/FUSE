// FUSE Relight RL-0.5 tests: the dxvk-remix hashing code as an oracle (see upstream_oracle.cpp).
#pragma once

#include "case_io.hpp"

#include <optional>
#include <string_view>

namespace fuse::relight::hash::test {

/// True when every oracle function can run (x86-64 with SSE4.1: the legacy position hash uses
/// _mm_round_ps exactly as upstream does).
bool oracleAvailable();

/// The outputs the upstream code gives for a case's inputs, or nullopt when the oracle does not
/// cover that function (texlayout: the D3DFORMAT table is checked by the Python reference) or
/// needs SSE4.1 that is not available.
std::optional<KeyValues> oracleCompute(std::string_view function, const KeyValues& inputs);

/// The draw-level oracle (index rebasing, computeHash, getHash, getHashForRuleLegacy).
std::optional<KeyValues> oracleDraw(const KeyValues& inputs);

} // namespace fuse::relight::hash::test
