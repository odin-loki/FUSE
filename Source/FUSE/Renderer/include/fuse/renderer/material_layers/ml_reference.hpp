#pragma once

// Asset plan W0.7: the CPU side of layered materials: the material library (texture pool + resolved .fusemat
// records + decode LUT, what MaterialLayers uploads), the built-in procedural test textures, the golden material-
// ball scene, the CPU reference renders and a small deterministic PNG codec (stored deflate) for the golden image.

#include <fuse/asset/cooked_texture.hpp>
#include <fuse/renderer/material_layers/fusemat.hpp>
#include <fuse/renderer/material_layers/ml_kernel.hpp>
#include <fuse/renderer/material_layers/ml_types.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::renderer::material_layers {

/// The decode LUT: [0, 256) sRGB -> linear (IEC 61966-2-1, computed in f64), [256, 512) v / 255.
void ml_build_lut(f32 (&lut)[kMlLutEntries]);

/// Materials + textures + balls in the form the kernels read.
class MlLibrary {
public:
    MlLibrary();

    /// Adds a texture (RGBA8 texels, x fastest, width * height of them); returns its index. The decoded mean is
    /// computed here (stochastic tiling).
    u32 addTexture(std::string_view id, u32 width, u32 height, u32 flags, const std::vector<u32>& texels);
    /// E06 (AP-RT-COOKED): adds a cooked `.fusetex` (BC1 / BC4 / BC5 / BC6H / BC7, 2D; the first layer of an array)
    /// under `id`. The texel pool gets level 0 decoded to RGBA8 (cooked_assets/bcn_decode.hpp; sRGB flag from the
    /// texture), and the texture keeps its cooked source: ml_build_mips takes the cooked mip chain (decoded) instead of
    /// box-filtering, and MaterialLayers uploads the blocks as a VK_FORMAT_BC* image (CPU-decoded without
    /// textureCompressionBC). BC5 textures get kMlTexRg (B / A read as 1). Returns the index, kMlNoTexture when the
    /// texture is empty or cannot be decoded.
    u32 addCookedTexture(std::string_view id, std::shared_ptr<const asset::CookedTexture> texture);
    /// Cooked source of texture `t` (null for RGBA8 / built-in textures).
    const asset::CookedTexture* cookedTexture(u32 t) const {
        return t < m_cooked.size() ? m_cooked[t].get() : nullptr;
    }
    /// Index of a texture id, kMlNoTexture if absent.
    u32 findTexture(std::string_view id) const;
    /// Adds one of the built-in procedural test textures by id (see ml_builtin_texture_ids); false if unknown.
    bool addBuiltinTexture(std::string_view id, u32 size = 128u);
    /// Resolves and appends a .fusemat (built-in textures it names are generated on demand).
    FuseMatResult addMaterial(const FuseMat& m, u32* index = nullptr);
    u32 addMaterial(const MlMaterial& m);

    MlView view() const;
    const std::vector<MlMaterial>& materials() const { return m_materials; }
    std::vector<MlMaterial>& materials() { return m_materials; }
    const std::vector<MlTexture>& textures() const { return m_textures; }
    const std::vector<u32>& texels() const { return m_texels; }
    const f32* lut() const { return m_lut; }
    std::vector<MlBall>& balls() { return m_balls; }
    const std::vector<MlBall>& balls() const { return m_balls; }

private:
    std::vector<MlMaterial> m_materials;
    std::vector<MlTexture> m_textures;
    std::vector<std::string> m_textureIds;
    std::vector<std::shared_ptr<const asset::CookedTexture>> m_cooked; ///< per texture (null: not cooked)
    std::vector<u32> m_texels;
    std::vector<MlBall> m_balls;
    f32 m_lut[kMlLutEntries] = {};
};

/// Built-in procedural texture ids: "<kind>_albedo" / "<kind>_normal" for kind in stone, moss, snow, grain
/// (detail), checker. Deterministic (integer hashes + f64 math), tileable.
const std::vector<std::string>& ml_builtin_texture_ids();
bool ml_make_builtin_texture(std::string_view id, u32 size, std::vector<u32>& texels, u32& flags);

/// The golden scene: 4 x 2 balls, camera, sun. `materials` = the ball materials in order (8).
struct MlBallScene {
    MlParams params{};           ///< camera / light / sizes (addresses left 0)
    std::vector<MlBall> balls;
};
MlBallScene ml_ball_scene(u32 width, u32 height, u32 materialCount);

/// CPU reference render of the ball scene (f32x4 per pixel).
void ml_render_balls(const MlParams& params, const MlView& view, const MlBall* balls, std::vector<MlF4>& out);

/// Linear f32x4 -> sRGB RGBA8 (IEC 61966-2-1 encode, round to nearest; alpha linear).
void ml_to_srgb8(const std::vector<MlF4>& image, std::vector<u8>& rgba);

/// PNG (RGBA8, filter 0, zlib stored blocks, CRC-32 / Adler-32): deterministic bytes.
std::vector<u8> ml_png_encode(u32 width, u32 height, const std::vector<u8>& rgba);
/// Decodes PNGs written by ml_png_encode (RGBA8, stored deflate, filter 0 rows); false otherwise.
bool ml_png_decode(const std::vector<u8>& png, u32& width, u32& height, std::vector<u8>& rgba);

/// Normalised autocorrelation of a scalar image (w x h) at a horizontal and a vertical lag (average of the two).
f64 ml_autocorrelation(const std::vector<f32>& image, u32 w, u32 h, u32 lag);

} // namespace fuse::renderer::material_layers
