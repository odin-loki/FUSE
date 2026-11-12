// FUSE Remaster W0.2: canonical content hashing of textures, meshes and sound (see store.hpp).
#include <fuse/remaster/store.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace fuse::remaster::store {

namespace {

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
}

std::uint32_t canonicalF32Bits(std::uint32_t bits) {
    if (bits == 0x80000000u) {
        return 0; // -0.0 -> +0.0
    }
    if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0) {
        return 0x7FC00000u; // every NaN -> one quiet NaN
    }
    return bits;
}

bool isFloatFormat(poco::Format f) {
    return f == poco::Format::F32x2 || f == poco::Format::F32x3 || f == poco::Format::F32x4;
}

std::string fail(std::string* error, const std::string& msg) {
    if (error) {
        *error = msg;
    }
    return {};
}

std::uint32_t readLE(const std::uint8_t* p, int bytes) {
    std::uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= std::uint32_t(p[i]) << (8 * i);
    }
    return v;
}

} // namespace

std::string canonicalTextureSha256(std::span<const std::uint8_t> rgba8) { return sha256Hex(rgba8.data(), rgba8.size()); }

std::uint32_t formatSize(poco::Format f) {
    switch (f) {
    case poco::Format::F32x2: return 8;
    case poco::Format::F32x3: return 12;
    case poco::Format::F32x4: return 16;
    case poco::Format::Unorm8x4: return 4;
    case poco::Format::U16x4: return 8;
    case poco::Format::Unorm16x4: return 8;
    }
    return 0;
}

std::optional<MeshData> meshFromInterleaved(std::span<const std::uint8_t> vertices, std::uint32_t stride,
                                            std::span<const InterleavedAttribute> layout, std::span<const std::uint32_t> indices32,
                                            std::span<const std::uint16_t> indices16, std::string* error) {
    if (stride == 0 || vertices.size() % stride != 0) {
        fail(error, "interleaved: the buffer size is not a multiple of the stride");
        return std::nullopt;
    }
    MeshData m;
    m.vertexCount = static_cast<std::uint32_t>(vertices.size() / stride);
    for (const InterleavedAttribute& a : layout) {
        const std::uint32_t size = formatSize(a.format);
        if (size == 0 || std::uint64_t(a.offset) + size > stride) {
            fail(error, "interleaved: an attribute does not fit in the stride");
            return std::nullopt;
        }
        MeshAttribute out{a.semantic, a.format, {}};
        out.data.resize(std::size_t(m.vertexCount) * size);
        for (std::uint32_t v = 0; v < m.vertexCount; ++v) {
            std::memcpy(out.data.data() + std::size_t(v) * size, vertices.data() + std::size_t(v) * stride + a.offset, size);
        }
        m.attributes.push_back(std::move(out));
    }
    if (!indices32.empty() && !indices16.empty()) {
        fail(error, "interleaved: pass either 32-bit or 16-bit indices");
        return std::nullopt;
    }
    m.indices.assign(indices32.begin(), indices32.end());
    for (const std::uint16_t i : indices16) {
        m.indices.push_back(i);
    }
    return m;
}

std::optional<std::vector<std::uint8_t>> canonicalMeshStream(const MeshData& mesh, std::string* error) {
    std::vector<const MeshAttribute*> attrs;
    const MeshAttribute* position = nullptr;
    for (const MeshAttribute& a : mesh.attributes) {
        for (const MeshAttribute* b : attrs) {
            if (b->semantic == a.semantic) {
                fail(error, std::string("mesh: duplicate ") + poco::enumName(a.semantic) + " attribute");
                return std::nullopt;
            }
        }
        if (std::uint64_t(formatSize(a.format)) * mesh.vertexCount != a.data.size()) {
            fail(error, std::string("mesh: ") + poco::enumName(a.semantic) + " holds " + std::to_string(a.data.size()) +
                            " bytes, expected vertexCount * element size");
            return std::nullopt;
        }
        if (a.semantic == poco::Semantic::Position) {
            if (a.format != poco::Format::F32x3) {
                fail(error, "mesh: Position must be F32x3");
                return std::nullopt;
            }
            position = &a;
        }
        attrs.push_back(&a);
    }
    if (!position) {
        fail(error, "mesh: no Position attribute");
        return std::nullopt;
    }
    for (const std::uint32_t i : mesh.indices) {
        if (i >= mesh.vertexCount) {
            fail(error, "mesh: index " + std::to_string(i) + " out of range");
            return std::nullopt;
        }
    }
    std::sort(attrs.begin(), attrs.end(), [](const MeshAttribute* a, const MeshAttribute* b) { return a->semantic < b->semantic; });

    std::vector<std::uint8_t> out;
    out.reserve(8 + position->data.size() + mesh.indices.size() * 4);
    putU32(out, mesh.vertexCount);
    putU32(out, static_cast<std::uint32_t>(mesh.indices.size()));
    auto appendData = [&](const MeshAttribute& a) {
        if (isFloatFormat(a.format)) {
            for (std::size_t o = 0; o + 4 <= a.data.size(); o += 4) {
                putU32(out, canonicalF32Bits(readLE(a.data.data() + o, 4)));
            }
        } else {
            out.insert(out.end(), a.data.begin(), a.data.end());
        }
    };
    appendData(*position);
    for (const std::uint32_t i : mesh.indices) {
        putU32(out, i);
    }
    for (const MeshAttribute* a : attrs) {
        if (a == position) {
            continue;
        }
        out.push_back(static_cast<std::uint8_t>(a->semantic));
        out.push_back(static_cast<std::uint8_t>(a->format));
        appendData(*a);
    }
    return out;
}

std::string canonicalMeshSha256(const MeshData& mesh, std::string* error) {
    const auto s = canonicalMeshStream(mesh, error);
    return s ? sha256Hex(s->data(), s->size()) : std::string();
}

std::optional<PcmAudio> decodeWav(std::span<const std::uint8_t> b, std::string* error) {
    auto bad = [&](const char* msg) -> std::optional<PcmAudio> {
        fail(error, std::string("wav: ") + msg);
        return std::nullopt;
    };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        return bad("not a RIFF/WAVE file");
    }
    std::uint16_t formatTag = 0, channels = 0, bits = 0, blockAlign = 0;
    std::uint32_t rate = 0;
    bool haveFmt = false;
    std::span<const std::uint8_t> data;
    bool haveData = false;
    std::size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const std::uint32_t size = readLE(b.data() + pos + 4, 4);
        const std::size_t body = pos + 8;
        if (size > b.size() - body) {
            return bad("truncated chunk");
        }
        if (std::memcmp(b.data() + pos, "fmt ", 4) == 0) {
            if (size < 16) {
                return bad("short fmt chunk");
            }
            formatTag = static_cast<std::uint16_t>(readLE(b.data() + body, 2));
            channels = static_cast<std::uint16_t>(readLE(b.data() + body + 2, 2));
            rate = readLE(b.data() + body + 4, 4);
            blockAlign = static_cast<std::uint16_t>(readLE(b.data() + body + 12, 2));
            bits = static_cast<std::uint16_t>(readLE(b.data() + body + 14, 2));
            if (formatTag == 0xFFFE) { // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the format tag
                if (size < 40) {
                    return bad("short extensible fmt chunk");
                }
                formatTag = static_cast<std::uint16_t>(readLE(b.data() + body + 24, 2));
            }
            haveFmt = true;
        } else if (std::memcmp(b.data() + pos, "data", 4) == 0) {
            data = b.subspan(body, size);
            haveData = true;
        }
        pos = body + size + (size & 1); // chunks are word-aligned
    }
    if (!haveFmt || !haveData) {
        return bad("missing fmt or data chunk");
    }
    if (channels == 0 || rate == 0) {
        return bad("zero channels or sample rate");
    }
    const bool isFloat = formatTag == 3;
    if (!(formatTag == 1 || isFloat)) {
        return bad("unsupported format tag (PCM and IEEE float only)");
    }
    if ((isFloat && bits != 32 && bits != 64) || (!isFloat && bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        return bad("unsupported bit depth");
    }
    const std::uint32_t bytesPer = bits / 8;
    if (blockAlign != bytesPer * channels) {
        return bad("block align disagrees with channels and bits");
    }
    const std::size_t frames = data.size() / blockAlign;
    PcmAudio pcm;
    pcm.sampleRate = rate;
    pcm.channels = channels;
    pcm.samples.reserve(frames * channels);
    for (std::size_t s = 0; s < frames * channels; ++s) {
        const std::uint8_t* p = data.data() + s * bytesPer;
        float v = 0.f;
        if (isFloat && bits == 32) {
            const std::uint32_t u = readLE(p, 4);
            std::memcpy(&v, &u, 4);
        } else if (isFloat) {
            std::uint64_t u = std::uint64_t(readLE(p, 4)) | (std::uint64_t(readLE(p + 4, 4)) << 32);
            double d = 0.0;
            std::memcpy(&d, &u, 8);
            v = static_cast<float>(d);
        } else if (bits == 8) {
            v = static_cast<float>(int(p[0]) - 128) / 128.f;
        } else {
            const std::uint32_t u = readLE(p, int(bytesPer));
            const std::uint32_t shift = 32 - bits;
            const auto sv = static_cast<std::int32_t>(u << shift) >> shift; // sign-extend
            v = static_cast<float>(static_cast<double>(sv) / static_cast<double>(1u << (bits - 1)));
        }
        pcm.samples.push_back(v);
    }
    return pcm;
}

std::string canonicalPcmSha256(const PcmAudio& pcm) {
    std::vector<std::uint8_t> out;
    out.reserve(12 + pcm.samples.size() * 4);
    putU32(out, pcm.sampleRate);
    putU32(out, pcm.channels);
    putU32(out, pcm.channels ? static_cast<std::uint32_t>(pcm.samples.size() / pcm.channels) : 0);
    for (const float f : pcm.samples) {
        std::uint32_t u = 0;
        std::memcpy(&u, &f, 4);
        putU32(out, canonicalF32Bits(u));
    }
    return sha256Hex(out.data(), out.size());
}

} // namespace fuse::remaster::store
