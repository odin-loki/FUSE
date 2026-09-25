#pragma once

// Asset W0.7 x WP-1.5 layered-bin gates: the material rows shared by the CPU gates
// (test_rp_material_resolve_layered_cpu.cpp) and the Lavapipe gates (test_rp_material_resolve_layered.cpp).
//
// Rows 0 .. mr_test::kMatCount - 1: the WP-1.5 test materials (flat, textured, normal-mapped, LOD probe, emissive,
// AO / emissive / metallic textures, cloth box side). Rows kLayeredBase + i (i < kLayeredRows): layered rows
// (gpu_scene::kGpuMaterialLayered) over entry i of the layered library, i.e. the 8 W0.7 material-ball fixtures
// (tests/material_layers/ball_*.fusemat.json: plain, stochastic, triplanar, triplanar + stochastic, moss, snow,
// wet + detail, everything). Row kLayeredBase + 2 carries an emissive colour and row kLayeredBase + 6 the cloth
// shading model, so the G-buffer gates see the row's emissive / shading model pass through the layered bin.

#include "test_rp_material_resolve_scene.hpp"

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>

#include <vector>

namespace mrl_test {

using fuse::f32;
using fuse::u32;
using GpuMaterial = fuse::renderer::Material::GPUMaterial;

inline constexpr u32 kLayeredBase = mr_test::kMatCount;
inline constexpr u32 kLayeredRows = 8u;
inline constexpr u32 kRowCount = kLayeredBase + kLayeredRows;
inline constexpr u32 kEmissiveLayeredRow = kLayeredBase + 2u;
inline constexpr u32 kClothLayeredRow = kLayeredBase + 6u;

/// The WP-1.5 rows + the layered rows. `tex[i]`: shader handle of WP-1.5 test texture i.
inline std::vector<GpuMaterial> makeMaterials(const u32 tex[mr_test::kTexCount]) {
    std::vector<GpuMaterial> m = mr_test::makeMaterials(tex);
    for (u32 i = 0; i < kLayeredRows; ++i) {
        GpuMaterial g{};
        // Ignored by the layered bin (the layered record defines the surface); set to a visible colour so a
        // row that loses its flag is told apart.
        g.baseColor = {0.9f, 0.1f, 0.6f, 0.f};
        g.roughnessEmissive = {0.35f, 0.f, 0.f, 0.f};
        g.shadingModel = 0u;
        if (kLayeredBase + i == kEmissiveLayeredRow) {
            g.roughnessEmissive = {0.35f, 0.2f, 0.4f, 0.8f};
            g.emissiveIntensity = 1.5f;
            g.shadingModel = 2u;
        }
        if (kLayeredBase + i == kClothLayeredRow) {
            g.shadingModel = 5u;
            g.clothBlock = {0.9f, 0.8f, 0.7f, 0.f};
        }
        fuse::renderer::gpu_scene::set_gpu_material_layered(g, i);
        m.push_back(g);
    }
    return m;
}

/// `rows` with the layered flag cleared (the rows become flat materials): the "plain" twin of a frame.
inline std::vector<GpuMaterial> withoutLayered(std::vector<GpuMaterial> rows) {
    for (GpuMaterial& g : rows) {
        g.flags &= ~fuse::renderer::gpu_scene::kGpuMaterialLayered;
        g.padding = 0u;
    }
    return rows;
}

} // namespace mrl_test
