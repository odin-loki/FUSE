// FUSE Relight RL-3.4: residency policy of replacement textures (see texture_residency.hpp).
#include <fuse/relight/replace/texture_residency.hpp>

#include <fuse/relight/mods/assets/texture_format.hpp>

#include <algorithm>
#include <optional>
#include <tuple>

namespace fuse::relight::replace {

namespace {

namespace as = mods::assets;

std::optional<as::TexFormat> formatByName(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (std::uint32_t v = 1; v < 256; ++v) {
        const auto f = static_cast<as::TexFormat>(v);
        if (const as::TexFormatInfo* i = as::texFormatInfo(f); i && name == i->name) {
            return f;
        }
    }
    return std::nullopt;
}

std::uint32_t lastMip(const TextureInfo& t) { return t.mips > 0 ? t.mips - 1 : 0; }

} // namespace

std::uint64_t textureBytes(const TextureInfo& t, std::uint32_t firstMip) {
    const std::uint32_t mips = std::max<std::uint32_t>(1, t.mips);
    firstMip = std::min(firstMip, mips - 1);
    if (const auto f = formatByName(t.format); f && t.width > 0 && t.height > 0) {
        std::uint64_t bytes = 0;
        for (std::uint32_t m = firstMip; m < mips; ++m) {
            bytes += as::texLevelSize(*f, as::mipExtent(t.width, m), as::mipExtent(t.height, m), 1);
        }
        if (bytes > 0) {
            return bytes;
        }
    }
    const std::uint64_t payload = t.fileBytes > 128 ? t.fileBytes - 128 : t.fileBytes; // DDS header
    return std::max<std::uint64_t>(1, payload >> (2 * std::min<std::uint32_t>(firstMip, 31)));
}

void TextureResidency::setCatalog(std::map<std::string, TextureInfo> catalog) {
    m_catalog = std::move(catalog);
    for (auto it = m_resident.begin(); it != m_resident.end();) {
        if (!m_catalog.count(it->first)) {
            it = m_resident.erase(it);
            ++m_pendingEvictions;
        } else {
            ++it;
        }
    }
}

ResidencyStats TextureResidency::update(std::uint64_t frame, const std::set<std::string>& used) {
    ResidencyStats st;
    st.budgetBytes = m_config.budgetBytes;
    st.evicted = m_pendingEvictions;
    m_pendingEvictions = 0;

    // 1. The request.
    struct Req {
        const TextureInfo* info = nullptr;
        std::uint32_t desiredMip = 0;
        std::uint32_t mip = 0;
        std::uint64_t bytes = 0;
    };
    std::map<std::string, Req> req;
    for (const auto& [sha, info] : m_catalog) {
        if (!used.count(sha) && !(info.preload || m_config.preloadAll)) {
            continue;
        }
        Req r;
        r.info = &info;
        r.desiredMip = std::min(m_config.mipBias, lastMip(info));
        r.mip = r.desiredMip;
        r.bytes = textureBytes(info, r.mip);
        req.emplace(sha, r);
    }
    st.requested = std::uint32_t(req.size());
    auto requestBytes = [&] {
        std::uint64_t b = 0;
        for (const auto& [sha, r] : req) {
            b += r.bytes;
        }
        return b;
    };

    // 2. Demote under pressure: the largest requested texture that can still drop a mip, one mip at a time.
    const std::uint64_t budget = m_config.budgetBytes;
    if (budget > 0) {
        while (requestBytes() > budget) {
            Req* largest = nullptr;
            for (auto& [sha, r] : req) {
                if (r.mip >= lastMip(*r.info)) {
                    continue;
                }
                if (!largest || r.bytes > largest->bytes) { // ties keep the first (sha256 order)
                    largest = &r;
                }
            }
            if (!largest) {
                st.overBudget = true;
                break;
            }
            ++largest->mip;
            largest->bytes = textureBytes(*largest->info, largest->mip);
        }
    }
    for (const auto& [sha, r] : req) {
        st.demoted += r.mip > r.desiredMip ? 1 : 0;
    }

    // 3. Evict unrequested resident textures, least recently used first.
    const std::uint64_t needed = requestBytes();
    std::vector<std::tuple<std::uint64_t, std::string>> candidates;
    std::uint64_t kept = 0;
    for (const auto& [sha, e] : m_resident) {
        if (!req.count(sha)) {
            candidates.emplace_back(e.lastUsed, sha);
            kept += e.bytes;
        }
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& [lastUsed, sha] : candidates) {
        if (budget == 0 || needed + kept <= budget) {
            break;
        }
        kept -= m_resident[sha].bytes;
        m_resident.erase(sha);
        ++st.evicted;
    }

    // 4. Make the request resident.
    for (const auto& [sha, r] : req) {
        auto it = m_resident.find(sha);
        if (it == m_resident.end() || it->second.minMip != r.mip) {
            ++st.loaded;
        }
        Entry& e = m_resident[sha];
        e.minMip = r.mip;
        e.bytes = r.bytes;
        if (used.count(sha)) {
            e.lastUsed = frame;
        }
    }
    for (const auto& [sha, e] : m_resident) {
        st.residentBytes += e.bytes;
    }
    st.resident = std::uint32_t(m_resident.size());
    if (budget > 0 && st.residentBytes > budget) {
        st.overBudget = true;
    }
    return st;
}

} // namespace fuse::relight::replace
