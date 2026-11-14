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
// FUSE_REMASTER_PLAN §2.6 (W0.4) adds, with the §0.1.0 allow / deny lists as data in Content/models/licence-policy.json
// (schema fuse.licence-policy/1):
//   8. no LicenseRef-Original-* / origin original|derived-original / recipe-only record is tracked (a Content file),
//      marked for a pack, named by a cook-manifest output, or in the derivation chain of a packed record;
//   9. every `ai` record has a `recipe` block with model id, model_version or date, numeric seed and output_sha256;
//  10. a record with an original in its derivation chain and `cloud_uploads` needs an `opt_in` record
//      (cloud_upload "opt-in", confirmed_by, confirmed date) and the paid tier;
//  11. forbidden model licences fail: a deny-listed (or unlisted) code / weights licence on the model or on any
//      `submodels` entry, a §3.4.2 non-FOSS model id, GPL code not run as a separate process, a `foss` flag that
//      disagrees with the computed one, and research-only / third-party-footage training data without review;
//  12. a provider that is not a policy default (gemini, mock) running a model off the §3.4.1 FOSS list is
//      non-default: it must say `non_default: true`, and a shippable output from it fails.
// Rule-1 exceptions: tooling data that is licensed with the repository, not content: schemas/**/*.schema.json and
// models/licence-policy.json (the policy itself is validated whenever present).
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
/// `policyFile` (optional): the real Content/models/licence-policy.json; the Remaster checks 8-12 are then also
/// run with it and must give the same findings as with the self-test's embedded policy.
bool selfTestAssetLicences(const std::filesystem::path& scratch, const Sha256Fn& sha256,
                           const std::filesystem::path& policyFile = {});

} // namespace fuse::tools::lint
