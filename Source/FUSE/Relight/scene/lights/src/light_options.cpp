// FUSE Relight RL-1.5: the legacy light translation options (see light_options.hpp).
#include <fuse/relight/scene/lights/light_options.hpp>

#include <fuse/relight/options/option_manager.hpp>

#include <variant>

namespace fuse::relight::scene {

float LightOptions::sceneScale() {
    if (const options::OptionBase* o = options::OptionManager::findOption("rtx.sceneScale")) {
        const options::OptionValue v = o->getResolvedValue();
        if (const float* f = std::get_if<float>(&v)) {
            return *f;
        }
    }
    return 1.0f; // RtxOptions::sceneScale default
}

} // namespace fuse::relight::scene
