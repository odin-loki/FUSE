// FUSE Relight RL-1.4: texture-hash options (see texture_options.hpp).
#include <fuse/relight/capture/texture/texture_options.hpp>

#include <fuse/relight/options/options.hpp>

namespace fuse::relight::capture::texture {

namespace {

struct TextureHashOptions {
    FUSE_RELIGHT_OPTION("rtx", bool, useObsoleteHashOnTextureUpload, false,
                        "Whether or not to use slower XXH64 hash on texture upload.\n"
                        "New projects should not enable this option as this solely exists for compatibility with "
                        "older hashing schemes.");
    FUSE_RELIGHT_OPTION("rtx", bool, recomputeTextureHashOnWrite, false,
                        "When true, the hash of a texture is recomputed when the game writes to its mip 0. Some games "
                        "manage their own pool of textures and shuffle data around those resources, resulting in "
                        "incorrect textures being displayed; recomputing the hash on write can resolve this, but can "
                        "have unintended side effects when replacing animated game textures. Hashes listed in "
                        "rtx.terrainTextures, rtx.lightmapTextures, rtx.ignoreTextures or "
                        "rtx.ignoreBakedLightingTextures are kept.");
};

} // namespace

bool isHashKeptOnWrite(hash::Hash64 hash) {
    for (std::string_view name : kKeepHashOnWriteLists) {
        options::OptionBase* base = options::OptionManager::findOption(name);
        if (base && base->getType() == options::OptionType::HashSet &&
            static_cast<options::Option<options::HashSet>*>(base)->containsHash(hash)) {
            return true;
        }
    }
    return false;
}

TextureTrackerConfig textureTrackerConfigFromOptions() {
    TextureTrackerConfig config;
    config.useObsoleteHashOnTextureUpload = TextureHashOptions::useObsoleteHashOnTextureUpload();
    config.recomputeTextureHashOnWrite = TextureHashOptions::recomputeTextureHashOnWrite();
    config.keepHashOnWrite = isHashKeptOnWrite;
    return config;
}

} // namespace fuse::relight::capture::texture
