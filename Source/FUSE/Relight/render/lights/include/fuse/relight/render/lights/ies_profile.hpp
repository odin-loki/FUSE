#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::render::lights {

/// Type-C IESNA LM-63 candela table (TILT=NONE only). Angles outside the grid clamp to the edge.
struct IesProfile {
    std::vector<float> verticalAngles;
    std::vector<float> horizontalAngles;
    std::vector<float> candela;
    float multiplier = 1.f;

    [[nodiscard]] float sample(float verticalDeg, float horizontalDeg) const {
        if (!std::isfinite(verticalDeg) || !std::isfinite(horizontalDeg) || verticalAngles.empty() ||
            horizontalAngles.empty()) {
            return 0.f;
        }
        std::size_t v0 = 0;
        std::size_t v1 = 0;
        std::size_t h0 = 0;
        std::size_t h1 = 0;
        float tv = 0.f;
        float th = 0.f;
        axis(verticalAngles, verticalDeg, v0, v1, tv);
        axis(horizontalAngles, horizontalDeg, h0, h1, th);
        const auto at = [this](std::size_t h, std::size_t v) {
            return candela[h * verticalAngles.size() + v];
        };
        const float c = (1.f - th) * ((1.f - tv) * at(h0, v0) + tv * at(h0, v1)) +
                        th * ((1.f - tv) * at(h1, v0) + tv * at(h1, v1));
        return c * multiplier;
    }

private:
    static void axis(const std::vector<float>& angles, float x, std::size_t& i0, std::size_t& i1, float& t) {
        if (angles.size() == 1 || x <= angles.front()) {
            i0 = 0;
            i1 = 0;
            t = 0.f;
            return;
        }
        if (x >= angles.back()) {
            i0 = angles.size() - 1;
            i1 = i0;
            t = 0.f;
            return;
        }
        i0 = 0;
        while (i0 + 1 < angles.size() && angles[i0 + 1] < x) {
            ++i0;
        }
        i1 = i0 + 1;
        const float span = angles[i1] - angles[i0];
        t = span > 0.f ? (x - angles[i0]) / span : 0.f;
    }
};

[[nodiscard]] inline std::optional<IesProfile> parse_ies(std::string_view text) {
    const auto tilt = text.find("TILT=");
    if (tilt == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = text.substr(tilt + 5);
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) {
        rest.remove_prefix(1);
    }
    if (rest.size() < 4 || rest.substr(0, 4) != "NONE") {
        return std::nullopt;
    }
    rest.remove_prefix(4);
    if (!rest.empty() && !std::isspace(static_cast<unsigned char>(rest.front()))) {
        return std::nullopt;
    }

    std::vector<double> nums;
    {
        std::stringstream ss{std::string(rest)};
        double value = 0.0;
        while (ss >> value) {
            nums.push_back(value);
        }
    }
    if (nums.size() < 14) {
        return std::nullopt;
    }
    auto asCount = [](double x) -> int {
        if (!std::isfinite(x) || x < 1.0 || x > 64.0) {
            return -1;
        }
        const int n = static_cast<int>(std::lround(x));
        if (std::fabs(x - static_cast<double>(n)) > 1e-4) {
            return -1;
        }
        return n;
    };
    const int verticalCount = asCount(nums[3]);
    const int horizontalCount = asCount(nums[4]);
    if (verticalCount < 1 || horizontalCount < 1 || std::lround(nums[5]) != 1) {
        return std::nullopt;
    }
    const std::size_t need = 14u + static_cast<std::size_t>(verticalCount) +
                             static_cast<std::size_t>(horizontalCount) +
                             static_cast<std::size_t>(verticalCount) * static_cast<std::size_t>(horizontalCount);
    if (nums.size() < need || !std::isfinite(nums[2])) {
        return std::nullopt;
    }

    IesProfile profile;
    profile.multiplier = static_cast<float>(nums[2]);
    profile.verticalAngles.resize(static_cast<std::size_t>(verticalCount));
    profile.horizontalAngles.resize(static_cast<std::size_t>(horizontalCount));
    profile.candela.resize(static_cast<std::size_t>(verticalCount) * static_cast<std::size_t>(horizontalCount));
    std::size_t cursor = 14;
    for (int i = 0; i < verticalCount; ++i, ++cursor) {
        profile.verticalAngles[static_cast<std::size_t>(i)] = static_cast<float>(nums[cursor]);
        if (i > 0 && !(profile.verticalAngles[static_cast<std::size_t>(i)] >
                       profile.verticalAngles[static_cast<std::size_t>(i - 1)])) {
            return std::nullopt;
        }
    }
    for (int i = 0; i < horizontalCount; ++i, ++cursor) {
        profile.horizontalAngles[static_cast<std::size_t>(i)] = static_cast<float>(nums[cursor]);
        if (i > 0 && !(profile.horizontalAngles[static_cast<std::size_t>(i)] >
                       profile.horizontalAngles[static_cast<std::size_t>(i - 1)])) {
            return std::nullopt;
        }
    }
    for (std::size_t i = 0; i < profile.candela.size(); ++i, ++cursor) {
        profile.candela[i] = static_cast<float>(nums[cursor]);
    }
    return profile;
}

} // namespace fuse::relight::render::lights
