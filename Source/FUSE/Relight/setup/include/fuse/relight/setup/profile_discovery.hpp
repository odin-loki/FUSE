// FUSE Relight RL-6.3: profile discovery and loading into the RL-0.6 option system.
//
// Discovery (runtimeOptionSystemDesc, called once when d3d9.dll / d3d8.dll resolve their runtime config):
//
//   FUSE_RELIGHT_PROFILE        "0" / "off" / "none": no profile. A file path: that profile, whatever its
//                               match section says (explicit choice).
//   FUSE_RELIGHT_PROFILE_PATH   directories to scan, separated by ';' (and ',' ); replaces the defaults:
//                               <exe dir>/relight/profiles, then <working dir>/relight/profiles.
//
// Every *.json file of the search directories (sorted by name, not recursive) is a candidate. A profile
// matches the running executable by XXH3-64 of its file (score 2) or by file name, case-insensitively
// (score 1; not when match.require_hash is set). The best score wins; on a tie the earlier directory, then
// the earlier file name. The executable is hashed only when some candidate lists hashes.
//
// Loading: the winner's profileToConfig() becomes OptionSystemDesc::appConfig, the "Hardcoded EXE Config"
// system layer (priority 2): above dxvk.conf and defaults, below rtx.conf, the environment, user.conf and
// every dynamic layer (Remix's per-application defaults). applyProfileLayer() instead loads a profile as a
// dynamic layer at a chosen priority (the assistant's live preview and tests).
#pragma once

#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/setup/profile.hpp>
#include <fuse/relight/setup/profile_runtime.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fuse::relight::setup {

inline constexpr const char* kProfileEnvVar = "FUSE_RELIGHT_PROFILE";
inline constexpr const char* kProfilePathEnvVar = "FUSE_RELIGHT_PROFILE_PATH";

/// XXH3-64 of a file's bytes (nullopt when it cannot be read).
std::optional<std::uint64_t> hashExecutableFile(const std::string& path);

struct ExecutableIdentity {
    std::string path; ///< full path; hashed on demand
    std::string name; ///< file name; derived from path when empty
    std::optional<std::uint64_t> hash; ///< preset (tests); computed from path when needed
};

struct DiscoveryRequest {
    ExecutableIdentity exe;
    std::vector<std::string> searchDirs;
    std::string explicitProfile; ///< a file: loaded without matching
};

struct DiscoveryResult {
    std::optional<GameProfile> profile;
    int score = 0;               ///< 3 explicit, 2 hash, 1 name
    std::vector<std::string> log; ///< one line per candidate / decision
};

/// 0 no match, 1 by name, 2 by hash. `exeHash` is filled lazily (only when the profile lists hashes).
int matchScore(const ProfileMatch& match, ExecutableIdentity& exe);

DiscoveryResult discoverProfile(DiscoveryRequest request);

/// Default search directories: FUSE_RELIGHT_PROFILE_PATH, else <exe dir>/relight/profiles and
/// <baseDirectory or working dir>/relight/profiles.
std::vector<std::string> defaultProfileSearchDirs(const std::string& exePath, const std::string& baseDirectory = {});

// runtimeOptionSystemDesc(baseDirectory) (profile_runtime.hpp): the OptionSystemDesc for this process, exe name
// and the discovered profile as appConfig; logs the decision to stderr ("fuse-relight: profile ...").
/// The profile runtimeOptionSystemDesc() loaded in this process (empty name when none).
const std::string& runtimeProfileName();

/// Load a profile as a dynamic option layer (priority clamped to the dynamic range by acquireLayer).
options::OptionLayerHandle applyProfileLayer(const GameProfile& profile, std::uint32_t priority);

} // namespace fuse::relight::setup
