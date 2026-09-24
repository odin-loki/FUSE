// FUSE Relight RL-1.3: the rtx.* options owned by the geometry capture (rtx.conf compatible; each
// also answers to its relight.* twin through the RL-0.6 alias rule). Names, defaults and meaning
// as dxvk-remix @0867d3c src/dxvk/rtx_render/rtx_options.h and src/d3d9/d3d9_rtx.h.
#include <fuse/relight/capture/geometry/geometry_capture.hpp>

#include <fuse/relight/options/options.hpp>

#include <string>

namespace fuse::relight::capture::geometry {

namespace {

struct GeometryHashOptions {
    FUSE_RELIGHT_OPTION("rtx", std::string, geometryGenerationHashRuleString,
                        "positions,indices,texcoords,geometrydescriptor,vertexlayout,vertexshader",
                        "Defines which hashes we need to generate via the geometry processing engine.");
    FUSE_RELIGHT_OPTION("rtx", std::string, geometryAssetHashRuleString, "positions,indices,geometrydescriptor",
                        "Defines which asset hashes we need to generate via the geometry processing engine.");
    FUSE_RELIGHT_OPTION("rtx", bool, enableIndexBufferMemoization, true,
                        "CPU performance optimization: cache the index rebasing per index buffer range and reuse it "
                        "until the range is written.");
};

} // namespace

GeometryCaptureConfig GeometryCaptureConfig::fromOptions() {
    GeometryCaptureConfig config;
    config.generationRule = hash::parseHashRule(GeometryHashOptions::geometryGenerationHashRuleString());
    config.assetRule = hash::parseHashRule(GeometryHashOptions::geometryAssetHashRuleString());
    config.indexBufferMemoization = GeometryHashOptions::enableIndexBufferMemoization();
    return config;
}

} // namespace fuse::relight::capture::geometry
