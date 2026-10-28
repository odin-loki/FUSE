#pragma once

namespace fuse::renderer {

[[nodiscard]] inline bool cluster_tiles_ok(int tilesX, int tilesY) {
    const auto axisOk = [](int tiles) { return tiles >= 1 && tiles <= 64; };
    return axisOk(tilesX) && axisOk(tilesY);
}

} // namespace fuse::renderer
