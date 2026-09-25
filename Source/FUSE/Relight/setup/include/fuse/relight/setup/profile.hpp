// FUSE Relight RL-6.3: per-game profiles (docs/plans/FUSE_REMIX_PORT_PLAN.md, RL-6.3).
//
// A profile is a small JSON file (Content/relight/profiles/*.json, or any directory on the profile search
// path) that carries, for one game:
//
//   * how to recognise the game: executable file names (case-insensitive) and/or XXH3-64 hashes of the
//     executable file (profile_discovery.hpp);
//   * an rtx.conf layer: `key = value` option text exactly as rtx.conf spells it;
//   * texture category lists: short category names ("sky", "ui", "decal", "ignore", ...) mapped to the
//     Remix rtx.*Textures hash-set options (textureCategories()), with Remix's `-hash` removal syntax;
//   * the setup assistant's suggestions (setup_assistant.hpp), applied or pending review, and notes.
//
// No game data: hashes and option values only.
//
//   {
//     "schema": "fuse.relight.profile/1",
//     "name": "Example Game",
//     "match": {"exe": ["game.exe"], "xxh3": ["0x0123456789ABCDEF"], "require_hash": false},
//     "options": {"rtx.preTransformedVerticesIsUI": "True"},
//     "textures": {"sky": ["0xC0BD341CA38B60FC"], "ui": ["0x...", "-0x..."]},
//     "suggestions": [{"kind": "texture", "category": "ui", "hash": "0x...", "confidence": 0.4,
//                      "applied": false, "reason": "..."}],
//     "notes": ["..."]
//   }
//
// At runtime the profile becomes the "Hardcoded EXE Config" system layer (priority 2, Remix's per-app
// defaults): stronger than dxvk.conf and the declared defaults, weaker than rtx.conf, the environment,
// user.conf and every dynamic layer, so a user's rtx.conf always wins and can remove profile hashes with
// `-hash`. writeProfile() is canonical (sorted keys and hashes), so a profile reads and writes back byte for
// byte; exportRtxConf() / importRtxConf() convert to and from Remix rtx.conf text.
#pragma once

#include <fuse/relight/options/hash_set_layer.hpp>
#include <fuse/relight/options/option_config.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::setup {

inline constexpr const char* kProfileSchema = "fuse.relight.profile/1";

/// A texture category of the profile format and the Remix hash-set option it feeds.
struct TextureCategory {
    const char* name;      ///< profile key ("sky")
    const char* optionKey; ///< rtx.conf key ("rtx.skyBoxTextures")
};

/// Every category, in a fixed order (the order writeProfile() uses).
const std::vector<TextureCategory>& textureCategories();
/// The category with this short name or option key (nullptr when unknown).
const TextureCategory* findTextureCategory(std::string_view nameOrOptionKey);

struct ProfileMatch {
    std::vector<std::string> exeNames;       ///< file names, compared case-insensitively
    std::vector<std::uint64_t> exeHashes;    ///< XXH3-64 of the executable file
    bool requireHash = false;                ///< a name match alone is not enough
    bool empty() const { return exeNames.empty() && exeHashes.empty(); }
};

/// One assistant proposal. Applied proposals are also in GameProfile::textures / options.
struct ProfileSuggestion {
    enum class Kind : std::uint8_t { Texture, Option };
    Kind kind = Kind::Texture;
    std::string category;   ///< Texture: short category name
    std::uint64_t hash = 0; ///< Texture: texture hash (a render-target category uses the descriptor hash)
    std::string key;        ///< Option: rtx.conf key
    std::string value;      ///< Option: rtx.conf value text
    double confidence = 0.0;
    bool applied = false;
    std::string reason;
    bool operator==(const ProfileSuggestion& o) const;
};

struct GameProfile {
    std::string name;
    std::string description;
    ProfileMatch match;
    std::map<std::string, std::string> options;                   ///< rtx.conf key -> value text
    std::map<std::string, options::HashSetLayer> textures;        ///< short category -> hashes
    std::vector<ProfileSuggestion> suggestions;
    std::vector<std::string> notes;
    std::string sourcePath; ///< file it was loaded from (not serialised)

    /// Add a hash to a category (removes a `-hash` opinion of the same category).
    bool addTexture(std::string_view category, std::uint64_t hash);
    /// Mark the matching suggestions applied and copy them into textures / options. `selector` is a hash
    /// ("0x..."), an option key, a category name, or "all". Returns how many were applied.
    std::size_t acceptSuggestions(std::string_view selector);
};

/// Parse a profile document. Unknown top-level members are ignored; unknown texture categories, bad hashes
/// and non-string option values are errors.
std::optional<GameProfile> parseProfile(std::string_view json, std::string* error = nullptr);
/// Canonical JSON text (two-space indent, sorted options and hashes, '\n' line ends).
std::string writeProfile(const GameProfile& profile);

std::optional<GameProfile> loadProfileFile(const std::string& path, std::string* error = nullptr);
bool saveProfileFile(const GameProfile& profile, const std::string& path);

/// The rtx.conf layer of a profile: its options plus one rtx.*Textures entry per non-empty category.
/// A texture category wins over the same key given as a raw option.
options::OptionConfig profileToConfig(const GameProfile& profile);
/// rtx.conf text of profileToConfig (sorted `key = value` lines, byte-stable).
std::string exportRtxConf(const GameProfile& profile);
/// A profile from rtx.conf text: rtx.*Textures keys of a known category become texture lists, every other
/// entry an option. `[exe]` sections apply as OptionConfig::parse applies them.
GameProfile importRtxConf(std::string_view confText, const options::ConfigParseOptions& parseOptions = {},
                          std::vector<options::ConfigDiagnostic>* diagnostics = nullptr);

/// "0x0123456789ABCDEF" (Remix's hash spelling).
std::string formatHash(std::uint64_t hash);
/// Accepts "0x..." or bare hex, any case.
bool parseHash(std::string_view text, std::uint64_t& out);

} // namespace fuse::relight::setup
