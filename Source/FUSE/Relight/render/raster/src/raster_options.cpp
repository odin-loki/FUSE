// FUSE Relight RL-4.2: raster remaster options (see raster_options.hpp).
#include <fuse/relight/render/raster/raster_options.hpp>

#include <algorithm>
#include <cctype>

namespace fuse::relight::render::raster {

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &RasterOptions::tier,          &RasterOptions::ambient,           &RasterOptions::exposure,           &RasterOptions::fog,
    &RasterOptions::decals,        &RasterOptions::shadowMapSize,     &RasterOptions::fallbackLightMode,
    &RasterOptions::fallbackLightRadiance, &RasterOptions::fallbackLightDirection,
};

} // namespace

void registerRasterOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
}

bool parseRasterTier(const std::string& text, int& out) {
    std::string s;
    for (char c : text) {
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (s.empty() || s == "auto" || s == "-1") {
        out = -1;
        return true;
    }
    if (s.size() == 2 && s[0] == 't') {
        s.erase(0, 1);
    }
    if (s.size() == 1 && s[0] >= '0' && s[0] <= '2') {
        out = s[0] - '0';
        return true;
    }
    return false;
}

RasterConfig RasterConfig::fromOptions() {
    registerRasterOptions();
    RasterConfig c;
    if (!parseRasterTier(RasterOptions::tier(), c.tier)) {
        c.tier = -1;
    }
    const options::Vec3f a = RasterOptions::ambient();
    const options::Vec3f r = RasterOptions::fallbackLightRadiance();
    const options::Vec3f d = RasterOptions::fallbackLightDirection();
    for (std::size_t i = 0; i < 3; ++i) {
        c.ambient[i] = std::max(0.f, a[i]);
        c.fallbackRadiance[i] = std::max(0.f, r[i]);
        c.fallbackDirection[i] = d[i];
    }
    c.exposure = std::max(0.f, RasterOptions::exposure());
    c.fog = RasterOptions::fog();
    c.decals = RasterOptions::decals();
    c.shadowMapSize = static_cast<std::uint32_t>(std::clamp(RasterOptions::shadowMapSize(), 64, 4096));
    c.fallbackLightMode = static_cast<std::uint32_t>(std::clamp(RasterOptions::fallbackLightMode(), 0, 2));
    return c;
}

} // namespace fuse::relight::render::raster
