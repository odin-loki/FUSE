#pragma once

#include <fuse/cook/cook_stub_writer.hpp>

#include <string>

namespace fuse::cook {

/// Optional ispc_texcomp BC7/BC5 hook — returns `ok=false` when header is absent.
CookStubWriteResult tryCookTextureIspc(const std::string& input_path, const std::string& output_path,
                                       const char* compression, bool mipmaps);

} // namespace fuse::cook
