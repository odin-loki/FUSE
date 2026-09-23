// FUSE Relight options (RL-0.6): umbrella header.
//
// Typed options compatible with RTX Remix's rtx.conf / user.conf / dxvk.conf, resolved through
// sparse priority layers. See option.hpp (declaring and using options), option_layer.hpp (layers
// and files), option_manager.hpp (frame lifecycle and system setup) and option_config.hpp (.conf
// syntax).
#pragma once

#include <fuse/relight/options/hash_set_layer.hpp>
#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_layer.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/options/option_types.hpp>
#include <fuse/relight/options/option_value.hpp>
