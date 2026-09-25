// FUSE Relight RL-6.4: file versions of optional plugin runtimes, read without loading them.
// PE / resource layout per the Microsoft PE/COFF specification (IMAGE_RESOURCE_DIRECTORY, RT_VERSION = 16,
// VS_FIXEDFILEINFO signature 0xFEEF04BD). FUSE's own code (MIT).
#include <fuse/relight/package/plugin_discovery.hpp>

#include <cctype>
#include <fstream>

namespace fuse::relight::package {

namespace {

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept : m_data(data), m_size(size) {}

    bool u16(std::size_t off, std::uint32_t& out) const noexcept {
        if (off > m_size || m_size - off < 2) {
            return false;
        }
        out = std::uint32_t(m_data[off]) | (std::uint32_t(m_data[off + 1]) << 8);
        return true;
    }
    bool u32(std::size_t off, std::uint32_t& out) const noexcept {
        if (off > m_size || m_size - off < 4) {
            return false;
        }
        out = std::uint32_t(m_data[off]) | (std::uint32_t(m_data[off + 1]) << 8) |
              (std::uint32_t(m_data[off + 2]) << 16) | (std::uint32_t(m_data[off + 3]) << 24);
        return true;
    }
    std::size_t size() const noexcept { return m_size; }

private:
    const std::uint8_t* m_data;
    std::size_t m_size;
};

struct Section {
    std::uint32_t va = 0;
    std::uint32_t vsize = 0;
    std::uint32_t rawSize = 0;
    std::uint32_t rawPtr = 0;
};

constexpr std::uint32_t kRtVersion = 16;
constexpr std::uint32_t kFixedSignature = 0xFEEF04BDu;
constexpr std::uint32_t kSubdirFlag = 0x80000000u;
constexpr std::size_t kMaxSections = 96;

} // namespace

bool parsePeFileVersion(const std::uint8_t* data, std::size_t size, FileVersion& out) noexcept {
    if (data == nullptr) {
        return false;
    }
    const Reader r(data, size);
    std::uint32_t mz = 0, peOff = 0, sig = 0;
    if (!r.u16(0, mz) || mz != 0x5A4Du || !r.u32(0x3C, peOff) || !r.u32(peOff, sig) || sig != 0x00004550u) {
        return false;
    }
    const std::size_t coff = std::size_t(peOff) + 4;
    std::uint32_t nsec = 0, optSize = 0, magic = 0;
    if (!r.u16(coff + 2, nsec) || !r.u16(coff + 16, optSize) || nsec == 0 || nsec > kMaxSections) {
        return false;
    }
    const std::size_t opt = coff + 20;
    if (!r.u16(opt, magic)) {
        return false;
    }
    std::size_t numRvaOff = 0;
    if (magic == 0x10Bu) {
        numRvaOff = opt + 92;
    } else if (magic == 0x20Bu) {
        numRvaOff = opt + 108;
    } else {
        return false;
    }
    std::uint32_t numRva = 0, resRva = 0, resSize = 0;
    if (!r.u32(numRvaOff, numRva) || numRva < 3 || !r.u32(numRvaOff + 4 + 2 * 8, resRva) ||
        !r.u32(numRvaOff + 4 + 2 * 8 + 4, resSize) || resRva == 0 || resSize == 0) {
        return false;
    }
    // Every data directory entry must fit inside the optional header.
    if (numRvaOff + 4 + std::size_t(numRva) * 8 > opt + optSize) {
        return false;
    }

    Section sections[kMaxSections];
    const std::size_t secTable = opt + optSize;
    for (std::uint32_t i = 0; i < nsec; ++i) {
        const std::size_t s = secTable + std::size_t(i) * 40;
        Section& sec = sections[i];
        if (!r.u32(s + 8, sec.vsize) || !r.u32(s + 12, sec.va) || !r.u32(s + 16, sec.rawSize) ||
            !r.u32(s + 20, sec.rawPtr)) {
            return false;
        }
    }
    // RVA -> file offset, with at least `need` readable bytes behind it.
    auto toOffset = [&](std::uint32_t rva, std::size_t need, std::size_t& off) noexcept {
        for (std::uint32_t i = 0; i < nsec; ++i) {
            const Section& sec = sections[i];
            const std::uint64_t span = sec.vsize > sec.rawSize ? sec.rawSize : (sec.vsize ? sec.vsize : sec.rawSize);
            if (rva >= sec.va && std::uint64_t(rva) - sec.va < span) {
                const std::uint64_t o = std::uint64_t(sec.rawPtr) + (rva - sec.va);
                const std::uint64_t inSection = span - (rva - sec.va);
                if (o + need > r.size() || need > inSection) {
                    return false;
                }
                off = std::size_t(o);
                return true;
            }
        }
        return false;
    };

    std::size_t resBase = 0;
    if (!toOffset(resRva, 16, resBase)) {
        return false;
    }
    const std::size_t resLimit = resBase + resSize; // offsets inside the resource tree stay below this
    // Returns the first entry's target (level > 1) or the RT_VERSION entry's target (level 1).
    auto pickEntry = [&](std::size_t dirOff, bool wantVersionType, std::uint32_t& target) noexcept {
        std::uint32_t named = 0, ids = 0;
        if (!r.u16(dirOff + 12, named) || !r.u16(dirOff + 14, ids)) {
            return false;
        }
        const std::uint32_t count = named + ids;
        for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
            const std::size_t e = dirOff + 16 + std::size_t(i) * 8;
            std::uint32_t name = 0, to = 0;
            if (e + 8 > resLimit || !r.u32(e, name) || !r.u32(e + 4, to)) {
                return false;
            }
            if (wantVersionType && (i < named || name != kRtVersion)) {
                continue;
            }
            target = to;
            return true;
        }
        return false;
    };
    std::uint32_t t = 0;
    if (!pickEntry(resBase, true, t) || (t & kSubdirFlag) == 0) {
        return false;
    }
    const std::size_t lvl2 = resBase + (t & ~kSubdirFlag);
    if (lvl2 + 16 > resLimit || !pickEntry(lvl2, false, t) || (t & kSubdirFlag) == 0) {
        return false;
    }
    const std::size_t lvl3 = resBase + (t & ~kSubdirFlag);
    if (lvl3 + 16 > resLimit || !pickEntry(lvl3, false, t) || (t & kSubdirFlag) != 0) {
        return false;
    }
    const std::size_t dataEntry = resBase + t;
    std::uint32_t dataRva = 0, dataSize = 0;
    if (dataEntry + 16 > resLimit || !r.u32(dataEntry, dataRva) || !r.u32(dataEntry + 4, dataSize)) {
        return false;
    }
    std::size_t blob = 0;
    if (dataSize < 52 || !toOffset(dataRva, dataSize, blob)) {
        return false;
    }
    // VS_VERSIONINFO: header, "VS_VERSION_INFO" (UTF-16), padding, then VS_FIXEDFILEINFO. Search the
    // 4-aligned positions of the block for the signature instead of re-deriving the padding.
    for (std::size_t o = 0; o + 52 <= dataSize; o += 4) {
        std::uint32_t s = 0;
        if (!r.u32(blob + o, s) || s != kFixedSignature) {
            continue;
        }
        std::uint32_t ms = 0, ls = 0;
        if (!r.u32(blob + o + 8, ms) || !r.u32(blob + o + 12, ls)) {
            return false;
        }
        out.major = ms >> 16;
        out.minor = ms & 0xFFFFu;
        out.patch = ls >> 16;
        out.build = ls & 0xFFFFu;
        return true;
    }
    return false;
}

bool readPeFileVersion(const std::string& path, FileVersion& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    const std::streamoff len = f.tellg();
    if (len <= 0 || len > (std::streamoff(256) << 20)) {
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(len));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), len)) {
        return false;
    }
    return parsePeFileVersion(bytes.data(), bytes.size(), out);
}

bool parseSonameVersion(std::string_view name, FileVersion& out) noexcept {
    const std::size_t so = name.rfind(".so.");
    if (so == std::string_view::npos) {
        return false;
    }
    std::string_view rest = name.substr(so + 4);
    std::uint32_t parts[4] = {0, 0, 0, 0};
    int n = 0;
    while (!rest.empty() && n < 4) {
        std::uint64_t v = 0;
        std::size_t i = 0;
        while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i])) != 0) {
            v = v * 10 + std::uint64_t(rest[i] - '0');
            if (v > 0xFFFFFFFFull) {
                return false;
            }
            ++i;
        }
        if (i == 0) {
            return false;
        }
        parts[n++] = std::uint32_t(v);
        rest.remove_prefix(i);
        if (rest.empty()) {
            break;
        }
        if (rest.front() != '.') {
            return false;
        }
        rest.remove_prefix(1);
    }
    if (n == 0 || !rest.empty()) {
        return false;
    }
    out = FileVersion{parts[0], parts[1], parts[2], parts[3]};
    return true;
}

} // namespace fuse::relight::package
