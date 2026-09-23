// FUSE Relight RL-0.5 tests: one entry per hashed function. Each entry generates random inputs
// (as case-line key/values) and computes the outputs with fuse_relight_hash. The same lines are
// checked by the Python reference and compared across native and MinGW/Wine builds.
#pragma once

#include "case_io.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::hash::test {

struct CaseFunction {
    std::string_view name;
    std::vector<std::string_view> outputKeys;
    KeyValues (*generate)(Rng& rng);
    /// Computes the outputs from the inputs; throws std::runtime_error on malformed inputs.
    KeyValues (*compute)(const KeyValues& inputs);
};

std::span<const CaseFunction> caseFunctions();
const CaseFunction* findCaseFunction(std::string_view name);

/// Inputs of `kv` (every key that is not an output key of `fn`).
KeyValues caseInputs(const CaseFunction& fn, const KeyValues& kv);

/// "" when the line's outputs match what fuse_relight_hash computes, else a diagnostic.
std::string verifyCase(const CaseFunction& fn, const KeyValues& kv);

/// Emits `count` random cases of `fn` (seeded) through `sink`, one formatted line each.
void emitRandomCases(const CaseFunction& fn, std::uint64_t seed, std::size_t count,
                     const std::function<void(const std::string&)>& sink);

} // namespace fuse::relight::hash::test
