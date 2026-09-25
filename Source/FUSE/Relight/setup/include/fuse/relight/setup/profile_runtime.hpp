// FUSE Relight RL-6.3: the one entry point the runtime (tap/src/tap_config.cpp) calls. Kept free of the
// profile types so the C++17 tap sources can include it.
#pragma once

#include <fuse/relight/options/option_manager.hpp>

#include <string>

namespace fuse::relight::setup {

/// See profile_discovery.hpp: OptionSystemDesc with the per-game profile as the app-config layer.
options::OptionSystemDesc runtimeOptionSystemDesc(const std::string& baseDirectory);

} // namespace fuse::relight::setup
