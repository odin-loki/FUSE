// FUSE Relight RL-3.4: residency policy of replacement textures (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.6).
//
// The policy (CPU bookkeeping; the renderer's upload queue executes it) decides per frame which replacement
// textures are resident and from which mip level, within a byte budget. Its rules follow the texture manager of
// Remix (rtx_texture_manager, MIT: facts only): a global mip bias drops the top mips of every texture
// (relight.replace.textureMipBias, e.g. from upscaling); materials flagged preload_textures, or every texture with
// relight.replace.preloadTextures, are requested from the moment their mod loads; the others on first use.
//
// Per update(frame, used):
//   1. requested = used + preload; each at minMip = min(mipBias, mips - 1).
//   2. Under budget pressure (requested bytes > budget) the largest requested textures are demoted one mip at a
//      time (ties: sha256 order) until the request fits or every texture sits at its last mip.
//   3. Resident textures that are not requested are evicted least recently used first (ties: sha256 order) until
//      requested + kept <= budget.
//   4. Requested textures become resident at their mip; a change of mip counts as a load.
// Everything is deterministic (no clock), so the capture record can carry the result.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fuse::relight::replace {

struct ResidencyConfig {
    std::uint64_t budgetBytes = 1024ull << 20; ///< 0: unlimited
    std::uint32_t mipBias = 0;
    bool preloadAll = false;
};

struct TextureInfo {
    std::string sha256;
    std::string format; ///< RL-3.3 TexFormatInfo name ("" or unknown: sizes estimated from fileBytes)
    std::uint32_t width = 0, height = 0, mips = 1;
    std::uint64_t fileBytes = 0;
    bool preload = false;
};

/// Bytes of mips [firstMip, mips) of `t` (block-compressed and uncompressed formats; unknown formats: the DDS
/// payload scaled by 4^-firstMip).
std::uint64_t textureBytes(const TextureInfo& t, std::uint32_t firstMip);

struct ResidencyStats {
    std::uint64_t budgetBytes = 0;
    std::uint64_t residentBytes = 0;
    std::uint32_t resident = 0;
    std::uint32_t requested = 0;
    std::uint32_t loaded = 0;  ///< became resident or changed mip this frame
    std::uint32_t evicted = 0; ///< left this frame (evicted, or no longer in any mod)
    std::uint32_t demoted = 0; ///< requested textures sitting above their desired mip because of the budget
    bool overBudget = false;   ///< even fully demoted, the request exceeds the budget
};

class TextureResidency {
public:
    explicit TextureResidency(ResidencyConfig config = {}) : m_config(config) {}

    void setConfig(const ResidencyConfig& config) { m_config = config; }
    const ResidencyConfig& config() const { return m_config; }

    /// The textures the loaded mods can use (by sha256). Resident textures no longer listed are dropped (counted
    /// as evicted at the next update).
    void setCatalog(std::map<std::string, TextureInfo> catalog);

    ResidencyStats update(std::uint64_t frame, const std::set<std::string>& used);

    struct Entry {
        std::uint32_t minMip = 0;
        std::uint64_t bytes = 0;
        std::uint64_t lastUsed = 0;
    };
    const std::map<std::string, Entry>& resident() const { return m_resident; }
    const std::map<std::string, TextureInfo>& catalog() const { return m_catalog; }

private:
    ResidencyConfig m_config;
    std::map<std::string, TextureInfo> m_catalog;
    std::map<std::string, Entry> m_resident;
    std::uint32_t m_pendingEvictions = 0;
};

} // namespace fuse::relight::replace
