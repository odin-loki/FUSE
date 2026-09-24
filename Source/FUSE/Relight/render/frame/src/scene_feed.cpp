// FUSE Relight RL-4.1: the per-frame scene feed (see scene_feed.hpp).
#include <fuse/relight/render/frame/scene_feed.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace fuse::relight::render::frame {

namespace gs = fuse::renderer::gpu_scene;
namespace inst = fuse::relight::scene::instances;

namespace {

std::uint64_t mixKey(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t v[2] = {a, b};
    return hash::xxh3_64(v, sizeof(v));
}

std::uint64_t stringKey(const std::string& s, std::uint64_t salt) { return hash::xxh3_64(s.data(), s.size(), salt); }

constexpr std::uint64_t kSaltLight = 0x6c697465ull; // "lite"

} // namespace

AdapterDraw adapterDraw(const inst::SceneDrawResult& result, const inst::SceneDrawInput& input,
                        const scene::LegacyMaterialRecord& material, tap::ResourceId colorTexture) {
    AdapterDraw d;
    d.instanceId = result.instanceId;
    d.blasId = result.blasId;
    d.created = result.created;
    d.objectToWorld = result.objectToWorld;
    d.bounds = input.boundingBox;
    d.materialHash = material.hash();
    d.colorTexture = colorTexture;
    d.diffuse = material.d3dMaterial.diffuse;
    d.transparent = material.blendMode.enableBlending;
    return d;
}

std::vector<AdapterLight> adapterLights(const std::vector<scene::LightRecord>& lights) {
    std::vector<AdapterLight> out;
    out.reserve(lights.size());
    for (const scene::LightRecord& l : lights) {
        if (l.isOff()) {
            continue;
        }
        AdapterLight a;
        a.key = mixKey(l.hash, kSaltLight);
        gs::GpuLight& g = a.light;
        const bool distant = l.type == hash::LightType::Distant;
        std::memcpy(g.position, l.position.data(), sizeof(g.position));
        const float maxc = std::max({l.radiance[0], l.radiance[1], l.radiance[2]});
        for (int c = 0; c < 3; ++c) {
            g.color[c] = maxc > 0.f ? l.radiance[static_cast<std::size_t>(c)] / maxc : 0.f;
        }
        if (distant) {
            // Distant: the radiance is the illuminance the light delivers at normal incidence.
            g.type = static_cast<std::uint32_t>(gs::GpuLightType::Directional);
            std::memcpy(g.direction, l.direction.data(), sizeof(g.direction));
            g.intensity = maxc;
            g.range = 0.f;
        } else {
            // Sphere of radius r and radiance L seen from d >> r: E = L pi r^2 / d^2, i.e. a point light of
            // intensity L pi r^2. Range: where that falls to kNewLightEndValue / 16 (the windowed inverse-square
            // falloff of the clustered lighting then only trims what is already below the legacy end value).
            const float r = std::max(l.radius, 1e-3f);
            g.intensity = maxc * scene::kLightPi * r * r;
            g.range = std::sqrt(g.intensity / (scene::kNewLightEndValue / 16.f));
            if (l.shaping.enabled) {
                g.type = static_cast<std::uint32_t>(gs::GpuLightType::Spot);
                std::memcpy(g.direction, l.shaping.direction.data(), sizeof(g.direction));
                g.cosOuter = l.shaping.cosConeAngle;
                g.cosInner = std::min(1.f, l.shaping.cosConeAngle + l.shaping.coneSoftness);
            } else {
                g.type = static_cast<std::uint32_t>(gs::GpuLightType::Point);
            }
        }
        out.push_back(a);
    }
    return out;
}

std::vector<AdapterLight> adapterLights(const std::vector<replace::ReplacedLight>& lights) {
    std::vector<AdapterLight> out;
    out.reserve(lights.size());
    for (const replace::ReplacedLight& l : lights) {
        AdapterLight a;
        a.key = l.origin == replace::ReplacedLight::Origin::Game
                    ? mixKey(l.gameHash, kSaltLight)
                    : mixKey(stringKey(l.mod + "/" + l.recordId, kSaltLight), l.instanceId);
        gs::GpuLight& g = a.light;
        const bool distant = l.type == "distant";
        g.type = static_cast<std::uint32_t>(distant ? gs::GpuLightType::Directional : gs::GpuLightType::Point);
        for (int c = 0; c < 3; ++c) {
            const auto i = static_cast<std::size_t>(c);
            g.position[c] = static_cast<float>(l.position[i]);
            g.direction[c] = static_cast<float>(l.direction[i]);
            g.color[c] = static_cast<float>(l.color[i]);
        }
        g.intensity = static_cast<float>(l.intensity);
        out.push_back(a);
    }
    return out;
}

} // namespace fuse::relight::render::frame
