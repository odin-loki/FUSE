// Engine probe smoke for real core/color.cpp (StockColor + static constants).
#include <fuse/legacy/t3d/api.hpp>

#include "color_real_wrapper.h"

namespace fuse::legacy::t3d::engineProbe {

bool colorStaticConstSmoke() {
    return ColorI::RED.red == 255 && ColorI::RED.green == 0 && ColorI::RED.blue == 0 &&
           LinearColorF::WHITE.red > 0.99f && LinearColorF::WHITE.green > 0.99f;
}

bool stockColorSmoke() {
    StockColor::create();
    const bool hasWhite = StockColor::isColor("White");
    const ColorI& red = StockColor::colorI("Red");
    const bool countOk = StockColor::getCount() > 100;
    StockColor::destroy();
    return hasWhite && red.red == 255 && red.green == 0 && red.blue == 0 && countOk;
}

} // namespace fuse::legacy::t3d::engineProbe
