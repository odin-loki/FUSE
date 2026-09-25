// fuse_lint asset-licences — FUSE_ASSET_PLAN §2.4 licence manifest gate (Wave 0 task W0.5).
//
// Content/licences.lock.json (schema 1) holds one record per asset. The gate checks:
//   1. every file under Content/ (except the lock, CREDITS.md and sources.lock.json) and every cooked
//      output named by an optional cook manifest resolves to a record;
//   2. file sha256s match (raw bytes, or the bytes with CRLF folded to LF for text checkouts);
//   3. the licence is on the allow-list, or it is review-class and review.required + review.by are set;
//   4. licence propagation over derived_from: forbidden inputs never flow; share-alike / database / GPL-art
//      inputs cannot flow into a record declared under a permissive licence or packed as "permissive";
//   5. forbidden identifiers (CC-BY-NC*, CC-BY-ND*, editorial / royalty-free / unknown provenance,
//      research-only datasets such as Bandai Namco, AMASS, Mixamo) fail;
//   6. Content/CREDITS.md equals the rendering of the lock file (regenerate: fuse_lint asset-credits);
//   7. generated records name a generator path@git-revision and a seed.
// Unknown provenance is recorded explicitly (origin "unknown" or licence "NOASSERTION") and is always
// reported, never silently accepted.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fuse::tools::lint {

using Sha256Fn = std::function<std::string(const std::string&)>;

/// Runs the gate over `content` (the Content/ directory). `manifest` may be empty (no cook manifest);
/// `cache` may be empty (cache-located files are then not hashed).
std::vector<std::string> checkAssetLicences(const std::filesystem::path& content, const std::filesystem::path& manifest,
                                            const std::filesystem::path& cache, const Sha256Fn& sha256);

/// Renders CREDITS.md from the lock file. Returns false (with `error`) when the lock does not parse.
bool renderAssetCredits(const std::filesystem::path& lockFile, std::string& out, std::string& error);

/// Self-test: seeds good / bad Content trees under `scratch` and checks the gate classifies them.
/// Prints failures to stderr; returns true when every expectation holds.
bool selfTestAssetLicences(const std::filesystem::path& scratch, const Sha256Fn& sha256);

} // namespace fuse::tools::lint
