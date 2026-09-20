#include <fuse/compat/ts_t3d.hpp>

namespace fuse::compat::ts_t3d {

ExecResult eval(std::string_view source, std::string_view chunkName) {
    return fuse::compat::eval(Dialect::T3d, source, chunkName);
}

ExecResult evalFile(const std::filesystem::path& path) {
    return fuse::compat::evalFile(Dialect::T3d, path);
}

} // namespace fuse::compat::ts_t3d
