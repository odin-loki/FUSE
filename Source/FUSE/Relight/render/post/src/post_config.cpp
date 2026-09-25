// FUSE Relight RL-5.7: post configuration (see post_config.hpp).
#include <fuse/relight/render/post/post_config.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace fuse::relight::render::post {

namespace {

const options::OptionBase* const kRegisteredOptions[] = {
    &PostOptions::enable,          &PostOptions::upscaler,         &PostOptions::resolutionScale,
    &PostOptions::sharpness,       &PostOptions::useAccumulation,
    &RemixPostOptions::upscalerType,    &RemixPostOptions::qualityDLSS,   &RemixPostOptions::taauPreset,
    &RemixPostOptions::nisPreset,       &RemixPostOptions::resolutionScale, &RemixPostOptions::nativeMipBias,
    &RemixPostOptions::upscalingMipBias, &RemixPostOptions::tonemappingMode, &LocalToneMapOptions::mip,      &LocalToneMapOptions::shadows,
    &LocalToneMapOptions::highlights, &LocalToneMapOptions::exposurePreferenceSigma,
    &LocalToneMapOptions::exposurePreferenceOffset,
};

std::string lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

void registerPostOptions() {
    for (const options::OptionBase* o : kRegisteredOptions) {
        (void)o;
    }
}

const char* upscalerNameForRemixType(int type) {
    switch (type) {
    case 0: return "none";
    case 1: return "dlss";
    case 2: return "nis";
    case 3: return "native_taau";
    case 4: return "xess";
    default: return "native_taau";
    }
}

float dlssPresetScale(int preset, u32 outputHeight) {
    if (preset >= 5) { // Auto
        preset = outputHeight <= 1080u ? 3 : (outputHeight <= 1440u ? 2 : 1);
    }
    static const float kScales[] = {0.33f, 0.5f, 0.58f, 0.667f, 1.f};
    return kScales[std::clamp(preset, 0, 4)];
}

float taauPresetScale(int preset, float customScale) {
    if (preset >= 5) {
        return std::clamp(customScale, 0.1f, 1.f);
    }
    static const float kScales[] = {0.33f, 0.5f, 0.66f, 0.75f, 1.f};
    return kScales[std::clamp(preset, 0, 4)];
}

float nisPresetScale(int preset) {
    static const float kScales[] = {0.5f, 0.66f, 0.75f, 1.f};
    return kScales[std::clamp(preset, 0, 3)];
}

void renderExtentForScale(u32 outputWidth, u32 outputHeight, float scale, u32& width, u32& height) {
    const double s = std::clamp(double(scale), 0.01, 1.0);
    width = std::clamp(static_cast<u32>(std::lround(double(outputWidth) * s)), 1u, std::max(outputWidth, 1u));
    height = std::clamp(static_cast<u32>(std::lround(double(outputHeight) * s)), 1u, std::max(outputHeight, 1u));
}

float textureMipBias(u32 renderWidth, u32 outputWidth, float nativeMipBias, float upscalingMipBias) {
    if (renderWidth == 0u || outputWidth == 0u || renderWidth == outputWidth) {
        return nativeMipBias;
    }
    return static_cast<float>(std::log2(double(renderWidth) / double(outputWidth))) + upscalingMipBias;
}

PostConfig PostConfig::fromOptions(u32 outputHeight) {
    PostConfig c;
    const std::string over = lower(PostOptions::upscaler());
    const int type = RemixPostOptions::upscalerType();
    c.upscaler = over.empty() ? upscalerNameForRemixType(type) : over;
    if (c.upscaler == "dlss" || c.upscaler == "dlss_rr") {
        c.resolutionScale = dlssPresetScale(RemixPostOptions::qualityDLSS(), outputHeight);
    } else if (c.upscaler == "nis") {
        c.resolutionScale = nisPresetScale(RemixPostOptions::nisPreset());
    } else if (c.upscaler == "none" || c.upscaler == "cas") {
        c.resolutionScale = 1.f;
    } else {
        c.resolutionScale = taauPresetScale(RemixPostOptions::taauPreset(), RemixPostOptions::resolutionScale());
    }
    const float forced = PostOptions::resolutionScale();
    if (forced > 0.f && c.upscaler != "none" && c.upscaler != "cas") {
        c.resolutionScale = std::clamp(forced, 0.1f, 1.f);
    }
    c.nativeMipBias = RemixPostOptions::nativeMipBias();
    c.upscalingMipBias = RemixPostOptions::upscalingMipBias();
    c.sharpness = std::clamp(PostOptions::sharpness(), 0.f, 1.f);
    c.useAccumulation = PostOptions::useAccumulation();
    c.toneMapping = RemixPostOptions::tonemappingMode() == 0 ? ToneMappingMode::Global : ToneMappingMode::Local;
    c.localToneMap.mip = static_cast<u32>(std::clamp(LocalToneMapOptions::mip(), 0, 8));
    c.localToneMap.shadows = std::max(LocalToneMapOptions::shadows(), 1e-3f);
    c.localToneMap.highlights = std::max(LocalToneMapOptions::highlights(), 1e-3f);
    c.localToneMap.exposurePreferenceSigma = std::max(LocalToneMapOptions::exposurePreferenceSigma(), 1e-3f);
    c.localToneMap.exposurePreferenceOffset = LocalToneMapOptions::exposurePreferenceOffset();
    return c;
}

} // namespace fuse::relight::render::post
