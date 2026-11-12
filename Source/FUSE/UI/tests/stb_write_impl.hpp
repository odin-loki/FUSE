// stb_image_write implementation for a gate's translation unit: include this once per executable instead of
// defining STB_IMAGE_WRITE_IMPLEMENTATION by hand. The vendored header trips -Wmissing-field-initializers (and
// -Wunused-function when a gate uses only some writers), which /WX / -Werror builds turn into errors; the warnings
// are switched off around the include only. clang-cl defines __clang__ but not __GNUC__, so both are checked
// (clang understands `#pragma GCC diagnostic`).
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
