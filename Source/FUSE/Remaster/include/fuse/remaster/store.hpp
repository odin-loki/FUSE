// FUSE Remaster W0.2: fuse_remaster_store, the content-addressed blob store and canonical hashing
// (docs/plans/FUSE_REMASTER_PLAN.md §1.3, §2.1).
//
//   <store>/blobs/sha256/<ab>/<sha256>          immutable payloads, addressed by the sha256 of their bytes
//   <store>/poco/<kind>/<poco_id>.poco.json     POCO records (fuse_poco), written canonically
//
// Blob addresses hash the *encoded* bytes (a PNG and a TGA of one image are two blobs). The `hash_key` rows of
// the replacement DB use *canonical* hashes instead, which hash the decoded content, so the same image as PNG or
// TGA, or the same mesh with its vertex attributes in another order or layout, gives the same key (§1.3 rule 1):
//
//   texture  sha256 of the decoded RGBA8 mip 0 pixels, rows top-down, no header (the definition the Relight
//            capture store already used for its "fuse.capture.sha256" texture keys, so those keys are canonical);
//   mesh     sha256 of: u32 vertex count, u32 index count, Position as f32x3, indices as u32 (exactly the Relight
//            capture stream when a mesh has positions only), then each further attribute in Semantic order as
//            u8 semantic, u8 format, raw little-endian data. Interleaved buffers are de-interleaved first, 16-bit
//            indices widen to 32 bits, -0.0 becomes +0.0 and every NaN one quiet NaN;
//   sound    sha256 of: u32 sample rate, u32 channels, u32 frames, then the samples as f32 little-endian
//            interleaved (integer PCM scaled by 2^-(bits-1); exact up to 24 bits), from WAV (PCM 8/16/24/32-bit
//            integer, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE; other chunks ignored).
#pragma once

#include <fuse/remaster/poco.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fuse::remaster::store {

// ---- canonical content --------------------------------------------------------------------------------------

struct ImageRGBA8 {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> pixels; // width * height * 4, rows top-down
};

/// Decodes PNG, TGA, BMP, JPEG, GIF (first frame), PSD and PNM (stb_image) to RGBA8. 16-bit images are reduced
/// to 8 bits. Never throws; nullopt + error on bad input.
std::optional<ImageRGBA8> decodeImage(std::span<const std::uint8_t> encoded, std::string* error = nullptr);

/// sha256 of the RGBA8 pixels (see the file comment).
std::string canonicalTextureSha256(std::span<const std::uint8_t> rgba8);
inline std::string canonicalTextureSha256(const ImageRGBA8& img) { return canonicalTextureSha256(img.pixels); }

/// One de-interleaved vertex attribute: `data` holds vertexCount elements of `format`, tightly packed, LE.
struct MeshAttribute {
    poco::Semantic semantic = poco::Semantic::Position;
    poco::Format format = poco::Format::F32x3;
    std::vector<std::uint8_t> data;
};
struct MeshData {
    std::uint32_t vertexCount = 0;
    std::vector<MeshAttribute> attributes; // any order; exactly one Position (F32x3)
    std::vector<std::uint32_t> indices;
};

/// Bytes per element of a stream format.
std::uint32_t formatSize(poco::Format f);

/// An attribute inside an interleaved vertex buffer.
struct InterleavedAttribute {
    poco::Semantic semantic = poco::Semantic::Position;
    poco::Format format = poco::Format::F32x3;
    std::uint32_t offset = 0;
};
/// De-interleaves `vertices` (`stride` bytes per vertex) into MeshData. 16-bit index buffers pass `indices16`.
std::optional<MeshData> meshFromInterleaved(std::span<const std::uint8_t> vertices, std::uint32_t stride,
                                            std::span<const InterleavedAttribute> layout,
                                            std::span<const std::uint32_t> indices32,
                                            std::span<const std::uint16_t> indices16 = {}, std::string* error = nullptr);

/// The canonical stream (see the file comment); nullopt when the mesh is malformed (no / duplicate Position,
/// a Position that is not F32x3, duplicate semantics, attribute sizes that disagree with vertexCount, an index
/// out of range).
std::optional<std::vector<std::uint8_t>> canonicalMeshStream(const MeshData& mesh, std::string* error = nullptr);
/// sha256 of canonicalMeshStream; empty when malformed.
std::string canonicalMeshSha256(const MeshData& mesh, std::string* error = nullptr);

struct PcmAudio {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::vector<float> samples; // interleaved, frames * channels
};
/// Decodes a RIFF/WAVE file (see the file comment).
std::optional<PcmAudio> decodeWav(std::span<const std::uint8_t> encoded, std::string* error = nullptr);
std::string canonicalPcmSha256(const PcmAudio& pcm);

// ---- the store ----------------------------------------------------------------------------------------------

class BlobStore {
public:
    explicit BlobStore(std::filesystem::path root) : m_root(std::move(root)) {}

    const std::filesystem::path& root() const { return m_root; }
    /// <root>/blobs/sha256/<ab>/<sha256>
    std::filesystem::path blobPath(const std::string& sha256) const;

    /// Stores `bytes` (a no-op when the blob exists) and returns its reference. Writes go to a temporary file
    /// renamed into place, so a crash never leaves a half-written blob under its final name.
    std::optional<poco::BlobRef> put(std::span<const std::uint8_t> bytes, const std::string& media, std::string* error = nullptr);
    bool has(const std::string& sha256) const;
    std::optional<std::vector<std::uint8_t>> get(const std::string& sha256) const;
    /// The blob exists, has the reference's size and hashes back to its address.
    bool verify(const poco::BlobRef& ref, std::string* error = nullptr) const;
    /// Every blob address in the store, sorted.
    std::vector<std::string> list() const;

    /// Writes a record to poco/<kind>/<id>.poco.json (validated first; byte-identical for equal records).
    bool writeRecord(const poco::Record& record, std::string* error = nullptr) const;
    std::optional<poco::Record> readRecord(const std::string& kind, const std::string& id, std::string* error = nullptr) const;
    /// Every record path (store-relative, generic separators), sorted.
    std::vector<std::string> listRecords() const;

private:
    std::filesystem::path m_root;
};

/// Whole-file helpers (binary). writeFileAtomic writes <path>.tmp and renames it over <path>.
bool writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string* error = nullptr);
bool writeFileAtomic(const std::filesystem::path& path, std::string_view text, std::string* error = nullptr);
std::optional<std::vector<std::uint8_t>> readFileBytes(const std::filesystem::path& path);
std::optional<std::string> readFileText(const std::filesystem::path& path);

} // namespace fuse::remaster::store
