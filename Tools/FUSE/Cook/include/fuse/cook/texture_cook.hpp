#pragma once

#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

/// Cooked BC7 texture (`FUSETEX_BC7` text header, `DATA\n`, then mode-6 blocks of every mip level
/// from largest to 1×1). The header never records paths or times, so identical sources cook to
/// identical bytes.
struct CookedTexture {
    struct Level {
        u32 width = 0;
        u32 height = 0;
        std::vector<u8> blocks;
    };

    std::string compression;
    std::vector<Level> levels;
};

/// Decode an image (PNG / TGA / BMP / JPEG via stb_image), build mips when requested, BC7-encode and
/// write. Fails without writing when the source cannot be decoded.
CookStubWriteResult cook_texture_bc7_file(const std::string& input_path, const std::string& output_path,
                                          bool mipmaps);

/// Write an in-memory RGBA8 image as a cooked BC7 texture (same format as `cook_texture_bc7_file`).
CookStubWriteResult write_texture_bc7_rgba(const u8* rgba, u32 width, u32 height, const std::string& output_path,
                                           bool mipmaps, const char* hook = "bc7_mode6");

/// Parse and validate a cooked BC7 texture file.
bool load_cooked_texture(const std::string& path, CookedTexture& out, std::string* error = nullptr);

} // namespace fuse::cook
