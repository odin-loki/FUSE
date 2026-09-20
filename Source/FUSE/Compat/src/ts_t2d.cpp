#include <fuse/compat/ts_t2d.hpp>

namespace fuse::compat::ts_t2d {

ExecResult eval(std::string_view source, std::string_view chunkName) {
    return fuse::compat::eval(Dialect::T2d, source, chunkName);
}

ExecResult evalFile(const std::filesystem::path& path) {
    return fuse::compat::evalFile(Dialect::T2d, path);
}

} // namespace fuse::compat::ts_t2d
