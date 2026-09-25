// FUSE Relight RL-1.8: writes a capture in both forms (plan §4.8):
//
//   <dir>/<stage>.usda, meshes/, materials/, lights/, skeletons/   the Remix-compatible capture (usda_writer.hpp)
//   <dir>/textures/<H>.dds                                      captured albedo textures (dds.hpp, BC passthrough)
//   <dir>/store/                                                the FUSE capture: POCO + hash_key rows (poco_store.hpp)
//
// The capture folder of a game is build/remaster-cache/<game>/capture/ (Remaster §0.1.2); the caller picks it.
#pragma once

#include <fuse/relight/capture/export/capture_model.hpp>
#include <fuse/relight/capture/export/poco_store.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fuse::relight::capture::exporter {

struct CaptureWriteReport {
    std::vector<std::string> files;     ///< every file written, relative to the capture directory, sorted
    std::vector<std::string> errors;    ///< non-fatal problems (a texture without DDS form, ...)
    std::vector<CaptureKey> keys;       ///< the hash_key rows written
    std::size_t textures = 0;           ///< DDS files written
    bool ok() const { return errors.empty(); }
};

/// Writes `capture` under `dir` (created). Returns false on an I/O error (details in report->errors).
bool writeCapture(const std::filesystem::path& dir, const CaptureData& capture, hash::HashRule assetRule,
                  CaptureWriteReport* report = nullptr);

} // namespace fuse::relight::capture::exporter
