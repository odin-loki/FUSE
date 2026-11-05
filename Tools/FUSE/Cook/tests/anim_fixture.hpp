#pragma once

// 4b test fixture: a tiny glTF 2.0 clip written at test time (no committed binary). Node tree
// Armature -> root -> arm; clip "walk", 1 s, LINEAR keys at 0 / 0.5 / 1 s:
//   root translation (0,0,0) -> (1.5,0.2,0.75) -> (3,0,1.5)
//   root rotation    yaw about +Y 0 -> 45 -> 90 degrees
//   arm  rotation    about +X 0 -> 30 -> 60 degrees
//   arm  scale       1 -> 2 -> 1 (uniform)
//   arm  translation (0,1,0) constant (one key)
// The analytic expectation (anim_fixture_expected) is the piecewise-linear / slerp curve through those keys.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace anim_fixture {

constexpr double kPi = 3.14159265358979323846;

inline void push_quat_axis(std::vector<float>& out, int axis, double degrees) {
    const double h = degrees * kPi / 360.0;
    float q[4] = {0.f, 0.f, 0.f, static_cast<float>(std::cos(h))};
    q[axis] = static_cast<float>(std::sin(h));
    out.insert(out.end(), q, q + 4);
}

/// Writes `<dir>/<stem>.gltf` + `<dir>/<stem>.bin`; returns the .gltf path. `yaw_end_deg` lets a test change the clip.
inline std::string write_gltf(const std::string& dir, const std::string& stem, double yaw_end_deg = 90.0) {
    std::vector<float> data;
    const float times[3] = {0.f, 0.5f, 1.f};
    data.insert(data.end(), times, times + 3);           // 0: times (12 B)
    data.push_back(0.f);                                 // 12: single key time (4 B)
    const float rootT[9] = {0.f, 0.f, 0.f, 1.5f, 0.2f, 0.75f, 3.f, 0.f, 1.5f};
    data.insert(data.end(), rootT, rootT + 9);           // 16: root T (36 B)
    push_quat_axis(data, 1, 0.0);                        // 52: root R (48 B)
    push_quat_axis(data, 1, yaw_end_deg * 0.5);
    push_quat_axis(data, 1, yaw_end_deg);
    push_quat_axis(data, 0, 0.0);                        // 100: arm R (48 B)
    push_quat_axis(data, 0, 30.0);
    push_quat_axis(data, 0, 60.0);
    const float armS[9] = {1.f, 1.f, 1.f, 2.f, 2.f, 2.f, 1.f, 1.f, 1.f};
    data.insert(data.end(), armS, armS + 9);             // 148: arm S (36 B)
    const float armT[3] = {0.f, 1.f, 0.f};
    data.insert(data.end(), armT, armT + 3);             // 184: arm T (12 B) -> 196 B

    {
        std::ofstream bin(dir + "/" + stem + ".bin", std::ios::binary | std::ios::trunc);
        bin.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * 4u));
    }
    const std::string json = R"({
  "asset": {"version": "2.0", "generator": "fuse anim_fixture"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Armature", "children": [1]},
    {"name": "root", "children": [2]},
    {"name": "arm", "translation": [0, 1, 0]}
  ],
  "buffers": [{"uri": ")" + stem + R"(.bin", "byteLength": 196}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 12},
    {"buffer": 0, "byteOffset": 12, "byteLength": 4},
    {"buffer": 0, "byteOffset": 16, "byteLength": 36},
    {"buffer": 0, "byteOffset": 52, "byteLength": 48},
    {"buffer": 0, "byteOffset": 100, "byteLength": 48},
    {"buffer": 0, "byteOffset": 148, "byteLength": 36},
    {"buffer": 0, "byteOffset": 184, "byteLength": 12}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0], "max": [1]},
    {"bufferView": 1, "componentType": 5126, "count": 1, "type": "SCALAR", "min": [0], "max": [0]},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 5, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 6, "componentType": 5126, "count": 1, "type": "VEC3"}
  ],
  "animations": [{
    "name": "walk",
    "samplers": [
      {"input": 0, "output": 2, "interpolation": "LINEAR"},
      {"input": 0, "output": 3, "interpolation": "LINEAR"},
      {"input": 0, "output": 4, "interpolation": "LINEAR"},
      {"input": 0, "output": 5, "interpolation": "LINEAR"},
      {"input": 1, "output": 6, "interpolation": "LINEAR"}
    ],
    "channels": [
      {"sampler": 0, "target": {"node": 1, "path": "translation"}},
      {"sampler": 1, "target": {"node": 1, "path": "rotation"}},
      {"sampler": 2, "target": {"node": 2, "path": "rotation"}},
      {"sampler": 3, "target": {"node": 2, "path": "scale"}},
      {"sampler": 4, "target": {"node": 2, "path": "translation"}}
    ]
  }]
}
)";
    const std::string path = dir + "/" + stem + ".gltf";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << json;
    return path;
}

struct Expected {
    double rootT[3];
    double rootYawDeg;
    double armXDeg;
    double armScale;
};

inline double piecewise(double t, double a, double b, double c) {
    t = std::fmin(std::fmax(t, 0.0), 1.0);
    return t <= 0.5 ? a + (b - a) * (t / 0.5) : b + (c - b) * ((t - 0.5) / 0.5);
}

/// The fixture's analytic local TRS at time t (seconds, clamped to [0, 1]).
inline Expected expected(double t, double yaw_end_deg = 90.0) {
    Expected e{};
    e.rootT[0] = piecewise(t, 0.0, 1.5, 3.0);
    e.rootT[1] = piecewise(t, 0.0, 0.2, 0.0);
    e.rootT[2] = piecewise(t, 0.0, 0.75, 1.5);
    e.rootYawDeg = piecewise(t, 0.0, yaw_end_deg * 0.5, yaw_end_deg);
    e.armXDeg = piecewise(t, 0.0, 30.0, 60.0);
    e.armScale = piecewise(t, 1.0, 2.0, 1.0);
    return e;
}

} // namespace anim_fixture
