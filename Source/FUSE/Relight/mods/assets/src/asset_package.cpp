// FUSE Relight RL-3.3: Remix asset package reader / writer. Port of dxvk-remix AssetPackage (MIT, NVIDIA;
// the notice is in asset_package.hpp), modified for FUSE.
#include <fuse/relight/mods/assets/asset_package.hpp>

#include <fuse/relight/mods/assets/gdeflate.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fuse::relight::mods::assets {

namespace {

std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint64_t get64(const std::uint8_t* p) { return std::uint64_t(get32(p)) | (std::uint64_t(get32(p + 4)) << 32); }

void put16(std::vector<std::uint8_t>& o, std::uint16_t v) {
    o.push_back(static_cast<std::uint8_t>(v));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    put16(o, static_cast<std::uint16_t>(v));
    put16(o, static_cast<std::uint16_t>(v >> 16));
}
void put64(std::vector<std::uint8_t>& o, std::uint64_t v) {
    put32(o, static_cast<std::uint32_t>(v));
    put32(o, static_cast<std::uint32_t>(v >> 32));
}

template <typename T>
std::optional<T> fail(std::string* error, std::string why) {
    if (error) {
        *error = std::move(why);
    }
    return std::nullopt;
}

bool isImage(PackageAssetType t) {
    return t == PackageAssetType::Image1D || t == PackageAssetType::Image2D || t == PackageAssetType::Image3D ||
           t == PackageAssetType::ImageCube;
}

const std::string kEmptyName;

} // namespace

std::optional<AssetPackage> AssetPackage::fromBytes(std::span<const std::uint8_t> bytes, std::string* error) {
    auto shared = std::make_shared<std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
    const std::uint64_t size = shared->size();
    ReadFn read = [shared](std::uint64_t offset, std::size_t n, std::uint8_t* dst) {
        if (offset > shared->size() || n > shared->size() - offset) {
            return false;
        }
        if (n) {
            std::memcpy(dst, shared->data() + offset, n);
        }
        return true;
    };
    return fromSource(std::move(read), size, error);
}

std::optional<AssetPackage> AssetPackage::open(const std::filesystem::path& path, std::string* error) {
    auto stream = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*stream) {
        return fail<AssetPackage>(error, "unable to open package file " + path.string());
    }
    stream->seekg(0, std::ios::end);
    const std::streamoff end = stream->tellg();
    if (end < 0) {
        return fail<AssetPackage>(error, "unable to size package file " + path.string());
    }
    const std::uint64_t size = static_cast<std::uint64_t>(end);
    ReadFn read = [stream, size](std::uint64_t offset, std::size_t n, std::uint8_t* dst) {
        if (offset > size || n > size - offset) {
            return false;
        }
        stream->clear();
        stream->seekg(static_cast<std::streamoff>(offset));
        stream->read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<std::size_t>(stream->gcount()) == n;
    };
    return fromSource(std::move(read), size, error);
}

std::optional<AssetPackage> AssetPackage::fromSource(ReadFn read, std::uint64_t size, std::string* error) {
    std::uint8_t header[kHeaderSize];
    if (size < kHeaderSize || !read(0, kHeaderSize, header)) {
        return fail<AssetPackage>(error, "malformed asset package (short header)");
    }
    if (get32(header) != kMagic) {
        return fail<AssetPackage>(error, "not an asset package");
    }
    if (get32(header + 4) != kVersion) {
        return fail<AssetPackage>(error, "asset package version mismatch: got " + std::to_string(get32(header + 4)) +
                                             ", expected " + std::to_string(kVersion));
    }
    AssetPackage pkg;
    pkg.dictOffset_ = get64(header + 8);
    if (pkg.dictOffset_ < kHeaderSize || pkg.dictOffset_ > size || size - pkg.dictOffset_ < 4) {
        return fail<AssetPackage>(error, "malformed asset package (dictionary offset)");
    }
    // Everything from the dictionary to EOF (dictionary + names) is small next to the blobs: read it once.
    const std::uint64_t tail = size - pkg.dictOffset_;
    constexpr std::uint64_t kMaxDictionary = 4 + 65535ull * (kAssetDescSize + kBlobDescSize) + (64ull << 20);
    if (tail > kMaxDictionary) {
        return fail<AssetPackage>(error, "malformed asset package (dictionary and names too large)");
    }
    std::vector<std::uint8_t> dict(static_cast<std::size_t>(tail));
    if (!read(pkg.dictOffset_, dict.size(), dict.data())) {
        return fail<AssetPackage>(error, "malformed asset package (dictionary read)");
    }
    const std::uint32_t assetCount = get16(dict.data()), blobCount = get16(dict.data() + 2);
    const std::uint64_t descBytes = std::uint64_t(assetCount) * kAssetDescSize + std::uint64_t(blobCount) * kBlobDescSize;
    if (4 + descBytes > dict.size()) {
        return fail<AssetPackage>(error, "malformed asset package (truncated dictionary)");
    }
    const std::uint8_t* p = dict.data() + 4;
    pkg.assets_.resize(assetCount);
    for (PackageAssetDesc& a : pkg.assets_) {
        a.nameIdx = get16(p);
        a.type = p[2] <= static_cast<std::uint8_t>(PackageAssetType::Buffer) ? static_cast<PackageAssetType>(p[2])
                                                                              : PackageAssetType::Unknown;
        a.format = p[3];
        a.size = get32(p + 4);
        a.width = get16(p + 4);
        a.height = get16(p + 6);
        a.depth = get16(p + 8);
        a.numMips = get16(p + 10);
        a.numTailMips = get16(p + 12);
        a.arraySize = get16(p + 14);
        a.baseBlobIdx = get16(p + 16);
        a.tailBlobIdx = get16(p + 18);
        p += kAssetDescSize;
    }
    pkg.blobs_.resize(blobCount);
    for (PackageBlobDesc& b : pkg.blobs_) {
        const std::uint64_t bits = get64(p);
        b.offset = bits & ((std::uint64_t(1) << 40) - 1);
        b.compression = static_cast<std::uint8_t>(bits >> 40);
        b.flags = static_cast<std::uint8_t>(bits >> 48);
        b.size = get32(p + 8);
        b.crc32 = get32(p + 12);
        p += kBlobDescSize;
    }
    // Names: assetCount NUL-terminated strings; upstream walks them with strlen, here every one must end
    // before EOF.
    const std::uint8_t* end = dict.data() + dict.size();
    pkg.names_.reserve(assetCount);
    for (std::uint32_t n = 0; n < assetCount; ++n) {
        const std::uint8_t* z = std::find(p, end, std::uint8_t(0));
        if (z == end) {
            return fail<AssetPackage>(error, "malformed asset package (name table)");
        }
        pkg.names_.emplace_back(reinterpret_cast<const char*>(p), static_cast<std::size_t>(z - p));
        pkg.nameIndex_.emplace(pkg.names_.back(), n);
        p = z + 1;
    }
    pkg.read_ = std::move(read);
    pkg.size_ = size;
    return pkg;
}

const std::string& AssetPackage::assetName(std::uint32_t idx) const { return idx < names_.size() ? names_[idx] : kEmptyName; }

std::uint32_t AssetPackage::findAsset(const std::string& name) const {
    const auto it = nameIndex_.find(name);
    return it == nameIndex_.end() ? kNoIndex : it->second;
}

std::optional<std::vector<std::uint8_t>> AssetPackage::readBlobRaw(std::uint32_t idx, std::string* error) const {
    const PackageBlobDesc* b = blob(idx);
    if (!b) {
        return fail<std::vector<std::uint8_t>>(error, "blob " + std::to_string(idx) + " does not exist");
    }
    if (b->offset > size_ || b->size > size_ - b->offset) {
        return fail<std::vector<std::uint8_t>>(error, "blob " + std::to_string(idx) + " lies outside the package");
    }
    std::vector<std::uint8_t> out(b->size);
    if (!read_(b->offset, out.size(), out.data())) {
        return fail<std::vector<std::uint8_t>>(error, "blob " + std::to_string(idx) + " read failed");
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> AssetPackage::readBlob(std::uint32_t idx, std::string* error,
                                                                std::uint64_t maxDecoded) const {
    auto raw = readBlobRaw(idx, error);
    if (!raw || blobs_[idx].compression == 0) {
        return raw;
    }
    std::string why;
    auto out = gdeflate::decompress(*raw, &why, maxDecoded);
    if (!out) {
        const char* kind = gdeflate::available() ? "" : " (no GDeflate codec in this build)";
        return fail<std::vector<std::uint8_t>>(error, "blob " + std::to_string(idx) + ": " + why + kind);
    }
    return out;
}

bool AssetPackage::blobCrcMatches(std::uint32_t idx) const {
    const auto raw = readBlobRaw(idx);
    return raw && gdeflate::crc32(*raw) == blobs_[idx].crc32;
}

std::uint32_t AssetPackage::blobIndex(std::uint32_t assetIdx, std::uint32_t layer, std::uint32_t face, std::uint32_t level) const {
    const PackageAssetDesc* a = asset(assetIdx);
    if (!a) {
        return kNoIndex;
    }
    if (a->type == PackageAssetType::Buffer) {
        return a->baseBlobIdx < blobs_.size() ? a->baseBlobIdx : kNoIndex;
    }
    if (!isImage(a->type) || a->numTailMips > a->numMips || level >= a->numMips || layer >= std::max<std::uint32_t>(1u, a->arraySize)) {
        return kNoIndex;
    }
    const std::uint32_t faces = a->type == PackageAssetType::ImageCube ? 6u : 1u;
    if (face >= faces) {
        return kNoIndex;
    }
    const std::uint64_t slot = std::uint64_t(layer) * faces + face;
    const std::uint32_t numLoose = std::uint32_t(a->numMips) - a->numTailMips;
    const std::uint64_t base = level >= numLoose ? a->tailBlobIdx : std::uint64_t(level) + a->baseBlobIdx;
    const std::uint64_t idx = base + slot * numLoose;
    return idx < blobs_.size() ? static_cast<std::uint32_t>(idx) : kNoIndex;
}

std::optional<TextureImage> AssetPackage::loadImage(std::uint32_t assetIdx, std::string* error) const {
    const PackageAssetDesc* a = asset(assetIdx);
    if (!a || !isImage(a->type)) {
        return fail<TextureImage>(error, "asset " + std::to_string(assetIdx) + " is not an image");
    }
    TextureImage img;
    img.format = static_cast<TexFormat>(a->format);
    if (!texFormatInfo(img.format)) {
        return fail<TextureImage>(error, "unsupported VkFormat " + std::to_string(a->format));
    }
    switch (a->type) {
    case PackageAssetType::Image1D:
        img.dimension = TexDimension::Tex1D;
        break;
    case PackageAssetType::Image3D:
        img.dimension = TexDimension::Tex3D;
        break;
    case PackageAssetType::ImageCube:
        img.dimension = TexDimension::Cube;
        break;
    default:
        img.dimension = TexDimension::Tex2D;
        break;
    }
    img.width = a->width;
    img.height = img.dimension == TexDimension::Tex1D ? 1u : a->height;
    img.depth = img.dimension == TexDimension::Tex3D ? std::max<std::uint32_t>(1u, a->depth) : 1u;
    img.mipLevels = a->numMips;
    img.arraySize = std::max<std::uint32_t>(1u, a->arraySize);
    img.faces = img.dimension == TexDimension::Cube ? 6u : 1u;
    if (img.width == 0 || img.height == 0 || img.mipLevels == 0 || img.mipLevels > fullMipCount(img.width, img.height, img.depth) ||
        a->numTailMips > a->numMips) {
        return fail<TextureImage>(error, "asset " + std::to_string(assetIdx) + " has a bad extent or mip count");
    }
    const std::uint64_t total = layoutSubresources(img);
    if (total == 0 || total > kMaxDecodedBytes) {
        return fail<TextureImage>(error, "asset " + std::to_string(assetIdx) + " is too large or has a bad layout");
    }
    // Every referenced blob must exist, and the stored bytes must be able to hold what is claimed (a
    // GDeflate stream is at least its header), before the output is allocated.
    const std::uint32_t numLoose = img.mipLevels - a->numTailMips;
    for (std::uint32_t l = 0; l < img.arraySize; ++l) {
        for (std::uint32_t f = 0; f < img.faces; ++f) {
            for (std::uint32_t m = 0; m < img.mipLevels; m = (m < numLoose ? m + 1 : img.mipLevels)) {
                if (blobIndex(assetIdx, l, f, m) == kNoIndex) {
                    return fail<TextureImage>(error, "asset " + std::to_string(assetIdx) + " references a missing blob");
                }
            }
        }
    }
    img.data.assign(static_cast<std::size_t>(total), 0);
    for (std::uint32_t l = 0; l < img.arraySize; ++l) {
        for (std::uint32_t f = 0; f < img.faces; ++f) {
            // Loose levels: one blob each.
            for (std::uint32_t m = 0; m < numLoose; ++m) {
                const Subresource* s = img.subresource(l, f, m);
                const std::uint32_t bi = blobIndex(assetIdx, l, f, m);
                auto bytes = readBlob(bi, error, s->size);
                if (!bytes) {
                    return std::nullopt;
                }
                if (bytes->size() != s->size) {
                    return fail<TextureImage>(error, "blob " + std::to_string(bi) + " holds " + std::to_string(bytes->size()) +
                                                         " bytes, level " + std::to_string(m) + " needs " + std::to_string(s->size));
                }
                std::memcpy(img.data.data() + s->offset, bytes->data(), bytes->size());
            }
            if (numLoose == img.mipLevels) {
                continue;
            }
            // Mip tail: the remaining levels back to back.
            std::uint64_t tailSize = 0;
            for (std::uint32_t m = numLoose; m < img.mipLevels; ++m) {
                tailSize += img.subresource(l, f, m)->size;
            }
            const std::uint32_t bi = blobIndex(assetIdx, l, f, numLoose);
            auto bytes = readBlob(bi, error, tailSize);
            if (!bytes) {
                return std::nullopt;
            }
            if (bytes->size() != tailSize) {
                return fail<TextureImage>(error, "mip-tail blob " + std::to_string(bi) + " holds " + std::to_string(bytes->size()) +
                                                     " bytes, the tail needs " + std::to_string(tailSize));
            }
            std::uint64_t at = 0;
            for (std::uint32_t m = numLoose; m < img.mipLevels; ++m) {
                const Subresource* s = img.subresource(l, f, m);
                std::memcpy(img.data.data() + s->offset, bytes->data() + at, static_cast<std::size_t>(s->size));
                at += s->size;
            }
        }
    }
    return img;
}

std::optional<std::vector<std::uint8_t>> AssetPackage::loadBuffer(std::uint32_t assetIdx, std::string* error) const {
    const PackageAssetDesc* a = asset(assetIdx);
    if (!a || a->type != PackageAssetType::Buffer) {
        return fail<std::vector<std::uint8_t>>(error, "asset " + std::to_string(assetIdx) + " is not a buffer");
    }
    const std::uint32_t bi = blobIndex(assetIdx, 0, 0, 0);
    if (bi == kNoIndex) {
        return fail<std::vector<std::uint8_t>>(error, "buffer asset references a missing blob");
    }
    auto bytes = readBlob(bi, error, a->size);
    if (bytes && bytes->size() != a->size) {
        return fail<std::vector<std::uint8_t>>(error, "buffer blob size does not match the asset size");
    }
    return bytes;
}

// ---- writer --------------------------------------------------------------------------------------------------

std::uint32_t AssetPackageWriter::addBlob(std::span<const std::uint8_t> bytes, std::uint32_t gdeflateLevel, std::string* error) {
    if (blobs_.size() >= 0xffff) {
        if (error) {
            *error = "too many blobs";
        }
        return AssetPackage::kNoIndex;
    }
    PackageBlobDesc b;
    std::vector<std::uint8_t> stored;
    if (gdeflateLevel > 0 && !bytes.empty()) {
        auto c = gdeflate::compress(bytes, gdeflateLevel, error);
        if (!c) {
            return AssetPackage::kNoIndex;
        }
        stored = std::move(*c);
        b.compression = 1;
    } else {
        stored.assign(bytes.begin(), bytes.end());
    }
    b.offset = AssetPackage::kHeaderSize + data_.size();
    b.size = static_cast<std::uint32_t>(stored.size());
    b.crc32 = gdeflate::crc32(stored);
    data_.insert(data_.end(), stored.begin(), stored.end());
    blobs_.push_back(b);
    return static_cast<std::uint32_t>(blobs_.size() - 1);
}

void AssetPackageWriter::addAsset(const std::string& name, const PackageAssetDesc& desc) {
    PackageAssetDesc d = desc;
    d.nameIdx = static_cast<std::uint16_t>(names_.size());
    assets_.push_back(d);
    names_.push_back(name);
}

bool AssetPackageWriter::addImage(const std::string& name, const TextureImage& image, std::uint32_t tailMips,
                                  std::uint32_t gdeflateLevel, std::string* error) {
    auto failed = [&](const char* why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    TextureImage layout = image;
    if (layoutSubresources(layout) != image.data.size() || static_cast<std::uint32_t>(image.format) > 0xff ||
        image.width > 0xffff || image.height > 0xffff || image.depth > 0xffff || tailMips > image.mipLevels) {
        return failed("image cannot be packaged (layout, format or extent)");
    }
    const std::uint32_t numLoose = image.mipLevels - tailMips;
    const std::uint32_t slots = image.arraySize * image.faces;
    if (tailMips > 0 && numLoose == 0 && slots > 1) {
        return failed("an all-tail image array cannot be addressed by the package layout");
    }
    PackageAssetDesc d;
    d.type = image.dimension == TexDimension::Tex1D   ? PackageAssetType::Image1D
             : image.dimension == TexDimension::Tex3D ? PackageAssetType::Image3D
             : image.dimension == TexDimension::Cube  ? PackageAssetType::ImageCube
                                                      : PackageAssetType::Image2D;
    d.format = static_cast<std::uint8_t>(image.format);
    d.width = static_cast<std::uint16_t>(image.width);
    d.height = static_cast<std::uint16_t>(image.height);
    d.depth = static_cast<std::uint16_t>(image.depth);
    d.numMips = static_cast<std::uint16_t>(image.mipLevels);
    d.numTailMips = static_cast<std::uint16_t>(tailMips);
    d.arraySize = static_cast<std::uint16_t>(image.arraySize);
    d.baseBlobIdx = static_cast<std::uint16_t>(blobs_.size());
    const std::span<const std::uint8_t> bytes(image.data);
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        for (std::uint32_t m = 0; m < numLoose; ++m) {
            const Subresource& s = layout.subresources[std::size_t(slot) * image.mipLevels + m];
            if (addBlob(bytes.subspan(static_cast<std::size_t>(s.offset), static_cast<std::size_t>(s.size)), gdeflateLevel, error) ==
                AssetPackage::kNoIndex) {
                return false;
            }
        }
    }
    if (tailMips > 0) {
        // Upstream addresses the tail of slot s as tailBlobIdx + s * numLoose; gaps are filled with empty blobs.
        d.tailBlobIdx = static_cast<std::uint16_t>(blobs_.size());
        for (std::uint32_t slot = 0; slot < slots; ++slot) {
            while (blobs_.size() < std::size_t(d.tailBlobIdx) + std::size_t(slot) * numLoose) {
                addBlob({}, 0, error);
            }
            const Subresource& first = layout.subresources[std::size_t(slot) * image.mipLevels + numLoose];
            const Subresource& last = layout.subresources[std::size_t(slot) * image.mipLevels + image.mipLevels - 1];
            if (addBlob(bytes.subspan(static_cast<std::size_t>(first.offset), static_cast<std::size_t>(last.offset + last.size - first.offset)),
                        gdeflateLevel, error) == AssetPackage::kNoIndex) {
                return false;
            }
        }
    }
    addAsset(name, d);
    return true;
}

std::vector<std::uint8_t> AssetPackageWriter::finish() const {
    std::vector<std::uint8_t> out;
    put32(out, AssetPackage::kMagic);
    put32(out, AssetPackage::kVersion);
    put64(out, AssetPackage::kHeaderSize + data_.size());
    out.insert(out.end(), data_.begin(), data_.end());
    put16(out, static_cast<std::uint16_t>(assets_.size()));
    put16(out, static_cast<std::uint16_t>(blobs_.size()));
    for (const PackageAssetDesc& a : assets_) {
        put16(out, a.nameIdx);
        out.push_back(static_cast<std::uint8_t>(a.type));
        out.push_back(a.format);
        if (a.type == PackageAssetType::Buffer) {
            put32(out, a.size);
        } else {
            put16(out, a.width);
            put16(out, a.height);
        }
        put16(out, a.depth);
        put16(out, a.numMips);
        put16(out, a.numTailMips);
        put16(out, a.arraySize);
        put16(out, a.baseBlobIdx);
        put16(out, a.tailBlobIdx);
    }
    for (const PackageBlobDesc& b : blobs_) {
        put64(out, (b.offset & ((std::uint64_t(1) << 40) - 1)) | (std::uint64_t(b.compression) << 40) | (std::uint64_t(b.flags) << 48));
        put32(out, b.size);
        put32(out, b.crc32);
    }
    for (const std::string& n : names_) {
        out.insert(out.end(), n.begin(), n.end());
        out.push_back(0);
    }
    return out;
}

} // namespace fuse::relight::mods::assets
