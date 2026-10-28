#pragma once

// Material id: 0 through 65535 inclusive.

namespace fuse::renderer {

[[nodiscard]] inline bool material_id_ok(int id) {
    return id >= 0 && id <= 65535;
}

} // namespace fuse::renderer
