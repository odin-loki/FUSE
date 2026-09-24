// FUSE Relight RL-3.4: the replacement engine's options (see replace_options.hpp).
#include <fuse/relight/replace/replace_options.hpp>

namespace fuse::relight::replace {

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &ReplaceOptions::enable,
    &ReplaceOptions::modPaths,
    &ReplaceOptions::mods,
    &ReplaceOptions::gameId,
    &ReplaceOptions::hotReload,
    &ReplaceOptions::watchBackend,
    &ReplaceOptions::pollIntervalMs,
    &ReplaceOptions::textureBudgetMiB,
    &ReplaceOptions::preloadTextures,
    &ReplaceOptions::textureMipBias,
    &ReplaceOptions::debugReloadWaitFrame,
    &ReplaceOptions::debugReloadMarker,
    &ReplaceOptions::debugReloadWaitMs,
    &ReplaceOptions::modOrder,
    &ReplaceOptions::enableReplacementAssets,
    &ReplaceOptions::enableReplacementLights,
    &ReplaceOptions::enableReplacementMeshes,
    &ReplaceOptions::enableReplacementMaterials,
};

} // namespace

void registerReplaceOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
}

} // namespace fuse::relight::replace
