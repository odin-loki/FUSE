// FUSE Relight RL-1.8: both capture forms in one directory (see capture_writer.hpp).
#include <fuse/relight/capture/export/capture_writer.hpp>

#include <fuse/relight/capture/export/dds.hpp>
#include <fuse/relight/capture/export/usda_writer.hpp>
#include <fuse/relight/hash/hash_string.hpp>

#include <algorithm>

namespace fuse::relight::capture::exporter {

namespace fs = std::filesystem;

bool writeCapture(const fs::path& dir, const CaptureData& capture, hash::HashRule assetRule, CaptureWriteReport* report) {
    CaptureWriteReport rep;
    bool ok = true;
    std::error_code ec;
    fs::create_directories(dir, ec);

    // Textures first: the store references their DDS bytes.
    std::map<Hash64, std::vector<std::uint8_t>> dds;
    for (const auto& [h, tex] : capture.textures) {
        if (tex.mip0.empty()) {
            continue; // no bytes captured (a render target): keyed, but no DDS file
        }
        DdsImage img;
        img.format = static_cast<hash::D3DFormat>(tex.d3dFormat);
        img.width = tex.width;
        img.height = tex.height;
        img.mips.push_back(tex.mip0);
        std::string err;
        std::vector<std::uint8_t> bytes = writeDds(img, &err);
        if (bytes.empty()) {
            rep.errors.push_back("texture " + hash::hashToString(h) + ": " + err);
            continue;
        }
        const std::string rel = captureTexturePath(h);
        if (!writeFile(dir / rel, bytes, &err)) {
            rep.errors.push_back(err);
            ok = false;
            continue;
        }
        rep.files.push_back(rel);
        ++rep.textures;
        dds.emplace(h, std::move(bytes));
    }
    for (const auto& [rel, text] : writeRemixUsda(capture)) {
        std::string err;
        if (!writeFile(dir / rel, text, &err)) {
            rep.errors.push_back(err);
            ok = false;
            continue;
        }
        rep.files.push_back(rel);
    }
    StoreReport store;
    std::string err;
    if (!writePocoStore(dir / "store", capture, assetRule, dds, &store, &err)) {
        rep.errors.push_back(err);
        ok = false;
    }
    for (const std::string& r : store.records) {
        rep.files.push_back("store/" + r);
    }
    rep.files.push_back("store/db/remaster_db.json");
    rep.keys = store.keys;
    std::sort(rep.files.begin(), rep.files.end());
    if (report) {
        *report = std::move(rep);
    }
    return ok;
}

} // namespace fuse::relight::capture::exporter
