// Relight options: process-wide state shared by the option, layer and manager translation units.
// Every accessor returns a function-local static, so options constructed during static
// initialization (the declaration macros) always find it constructed.
#pragma once

#include <fuse/relight/options/option_config.hpp>

#include <atomic>
#include <map>
#include <string>

namespace fuse::relight::options {

class OptionLayer;

namespace detail {

struct SystemLayerState {
    const OptionLayer* defaultLayer = nullptr;
    OptionLayer* rtxConfLayer = nullptr;
    const OptionLayer* environmentLayer = nullptr;
    const OptionLayer* qualityLayer = nullptr;
    const OptionLayer* derivedLayer = nullptr;
    OptionLayer* userLayer = nullptr;
    OptionConfig mergedConfig;
    std::string exeName;
    bool initialized = false;
};

SystemLayerState& systemLayerState();
std::map<std::string, std::string, std::less<>>& aliasTable();
std::atomic<bool>& graphicsPresetIsCustom();
bool& drawcallTranslationInvalid();

} // namespace detail
} // namespace fuse::relight::options
