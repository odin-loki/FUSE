#include <fuse/renderer/volumetric/volumetric_fog.hpp>

#include <fuse/renderer/command_buffer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

f32 clamp01(f32 value) {
    return std::clamp(value, 0.f, 1.f);
}

f32 froxelDensityAt(const FroxelDensityGrid& grid, const FroxelGridDesc& desc, u32 tileX, u32 tileY, u32 sliceZ) {
    if (grid.density.empty() || desc.tilesX == 0u || desc.tilesY == 0u || desc.slicesZ == 0u) {
        return 0.f;
    }

    const u32 clampedTileX = std::min(tileX, desc.tilesX - 1u);
    const u32 clampedTileY = std::min(tileY, desc.tilesY - 1u);
    const u32 clampedSliceZ = std::min(sliceZ, desc.slicesZ - 1u);
    const u32 index = FroxelGridLayout::froxelIndex(clampedTileX, clampedTileY, clampedSliceZ, desc);
    if (index >= grid.density.size()) {
        return 0.f;
    }
    return grid.density[index];
}

} // namespace

f32 FroxelSliceLayout::computeSliceNearZ(u32 sliceZ, const FroxelGridDesc& desc, const FroxelCameraDesc& camera) {
    if (sliceZ >= desc.slicesZ) {
        return camera.farPlane;
    }

    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 sliceT0 = static_cast<f32>(sliceZ) / static_cast<f32>(desc.slicesZ);
    return camera.nearPlane * std::pow(depthRatio, sliceT0);
}

f32 FroxelSliceLayout::computeSliceFarZ(u32 sliceZ, const FroxelGridDesc& desc, const FroxelCameraDesc& camera) {
    if (sliceZ >= desc.slicesZ) {
        return camera.farPlane;
    }

    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 sliceT1 = static_cast<f32>(sliceZ + 1u) / static_cast<f32>(desc.slicesZ);
    return camera.nearPlane * std::pow(depthRatio, sliceT1);
}

u32 FroxelGridLayout::froxelIndex(u32 tileX, u32 tileY, u32 sliceZ, const FroxelGridDesc& desc) {
    return (tileY * desc.tilesX + tileX) * desc.slicesZ + sliceZ;
}

void FroxelGridLayout::decodeFroxelIndex(u32 index, const FroxelGridDesc& desc, u32& tileX, u32& tileY, u32& sliceZ) {
    if (desc.slicesZ == 0u || desc.tilesX == 0u) {
        tileX = 0u;
        tileY = 0u;
        sliceZ = 0u;
        return;
    }

    sliceZ = index % desc.slicesZ;
    const u32 tileSlice = index / desc.slicesZ;
    tileX = tileSlice % desc.tilesX;
    tileY = tileSlice / desc.tilesX;
}

bool FroxelGridLayout::mapScreenDepthToSampleCoords(f32 screenX,
                                                    f32 screenY,
                                                    f32 viewDepth,
                                                    const FroxelGridDesc& desc,
                                                    const FroxelCameraDesc& camera,
                                                    FroxelSampleCoords& outCoords) {
    if (desc.tilesX == 0u || desc.tilesY == 0u || desc.slicesZ == 0u) {
        return false;
    }
    if (viewDepth < camera.nearPlane || viewDepth > camera.farPlane) {
        return false;
    }

    const f32 continuousTileX = clamp01(screenX) * static_cast<f32>(desc.tilesX);
    const f32 continuousTileY = clamp01(screenY) * static_cast<f32>(desc.tilesY);
    outCoords.tileX0 = static_cast<u32>(continuousTileX);
    outCoords.tileY0 = static_cast<u32>(continuousTileY);
    outCoords.tileX1 = std::min(outCoords.tileX0 + 1u, desc.tilesX - 1u);
    outCoords.tileY1 = std::min(outCoords.tileY0 + 1u, desc.tilesY - 1u);
    outCoords.tx = continuousTileX - static_cast<f32>(outCoords.tileX0);
    outCoords.ty = continuousTileY - static_cast<f32>(outCoords.tileY0);

    const f32 depthRatio = camera.farPlane / camera.nearPlane;
    const f32 logDepth = std::log(viewDepth / camera.nearPlane) / std::log(depthRatio);
    const f32 continuousSlice = logDepth * static_cast<f32>(desc.slicesZ);
    outCoords.sliceZ0 = static_cast<u32>(continuousSlice);
    outCoords.sliceZ1 = std::min(outCoords.sliceZ0 + 1u, desc.slicesZ - 1u);
    outCoords.tz = continuousSlice - static_cast<f32>(outCoords.sliceZ0);
    return true;
}

namespace froxel_util {

f32 lerpDensity(f32 a, f32 b, f32 t) {
    return a + (b - a) * clamp01(t);
}

f32 sampleDensityBilinear(const FroxelDensityGrid& grid,
                          const FroxelGridDesc& desc,
                          const FroxelSampleCoords& coords) {
    const f32 d00 = froxelDensityAt(grid, desc, coords.tileX0, coords.tileY0, coords.sliceZ0);
    const f32 d10 = froxelDensityAt(grid, desc, coords.tileX1, coords.tileY0, coords.sliceZ0);
    const f32 d01 = froxelDensityAt(grid, desc, coords.tileX0, coords.tileY1, coords.sliceZ0);
    const f32 d11 = froxelDensityAt(grid, desc, coords.tileX1, coords.tileY1, coords.sliceZ0);

    const f32 d0 = lerpDensity(d00, d10, coords.tx);
    const f32 d1 = lerpDensity(d01, d11, coords.tx);
    return lerpDensity(d0, d1, coords.ty);
}

f32 sampleDensityTrilinear(const FroxelDensityGrid& grid,
                           const FroxelGridDesc& desc,
                           const FroxelSampleCoords& coords) {
    FroxelSampleCoords slice0 = coords;
    slice0.sliceZ1 = slice0.sliceZ0;
    FroxelSampleCoords slice1 = coords;
    slice1.sliceZ0 = slice1.sliceZ1;

    const f32 nearSlice = sampleDensityBilinear(grid, desc, slice0);
    const f32 farSlice = sampleDensityBilinear(grid, desc, slice1);
    return lerpDensity(nearSlice, farSlice, coords.tz);
}

void populateFromAnalyticFog(FroxelDensityGrid& grid,
                             const FroxelGridDesc& desc,
                             const FroxelCameraDesc& camera,
                             const VolumetricFogParams& params) {
    grid.density.assign(desc.froxelCount(), 0.f);
    if (desc.froxelCount() == 0u) {
        return;
    }

    for (u32 sliceZ = 0; sliceZ < desc.slicesZ; ++sliceZ) {
        const f32 sliceNear = FroxelSliceLayout::computeSliceNearZ(sliceZ, desc, camera);
        const f32 sliceFar = FroxelSliceLayout::computeSliceFarZ(sliceZ, desc, camera);
        const f32 viewDepth = 0.5f * (sliceNear + sliceFar);

        for (u32 tileY = 0; tileY < desc.tilesY; ++tileY) {
            for (u32 tileX = 0; tileX < desc.tilesX; ++tileX) {
                const u32 index = FroxelGridLayout::froxelIndex(tileX, tileY, sliceZ, desc);
                const math::Vec3 world_pos{camera.position.x,
                                           params.base_height + viewDepth * 0.01f,
                                           camera.position.z};
                grid.density[index] = sample_volumetric_fog_density(params, world_pos);
            }
        }
    }
}

} // namespace froxel_util

namespace {

struct VolumetricFogPassUserData {
    VolumetricFogParams params{};
    VolumetricFogPassStats stats{};
};

void executeVolumetricFogPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    auto* pass = static_cast<VolumetricFogPassUserData*>(userData);
    if (recorder == nullptr || pass == nullptr) {
        return;
    }

    recorder->beginPass("volumetric_fog");
    record_volumetric_fog_pass(pass->params, pass->stats);
    recorder->endPass();
}

VolumetricFogPassUserData g_volumetricFogPasses[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_volumetricFogAccesses[RenderGraph::kMaxPassesPerFrame * 4]{};
u32 g_volumetricFogPassCount = 0;

} // namespace

f32 sample_volumetric_fog_density(const VolumetricFogParams& params, const math::Vec3& world_pos) {
    const f32 height_delta = world_pos.y - params.base_height;
    const f32 height_factor = std::exp(-params.height_falloff * std::max(0.f, height_delta));
    return params.density * height_factor;
}

bool record_volumetric_fog_pass(const VolumetricFogParams& params, VolumetricFogPassStats& stats) {
    if (params.march_steps == 0u) {
        stats.ready = false;
        return false;
    }

    stats.ready = true;
    stats.lastDensity = sample_volumetric_fog_density(params, {0.f, 0.f, 0.f});
    ++stats.framesRecorded;
    return true;
}

void resetVolumetricFogPassGraphStorage() {
    g_volumetricFogPassCount = 0;
}

void addVolumetricFogPassToGraph(RenderGraph& graph, const RGTextureAccess* depth_read, u32 access_count) {
    if (g_volumetricFogPassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const u32 passIndex = g_volumetricFogPassCount++;
    VolumetricFogPassUserData& userData = g_volumetricFogPasses[passIndex];
    userData.params = VolumetricFogParams{};
    userData.stats = {};

    const RGTextureAccess* accesses = nullptr;
    u32 resolvedCount = 0u;
    if (depth_read != nullptr && access_count > 0u) {
        const u32 base = passIndex * 4u;
        for (u32 i = 0; i < access_count; ++i) {
            g_volumetricFogAccesses[base + i] = depth_read[i];
        }
        accesses = &g_volumetricFogAccesses[base];
        resolvedCount = access_count;
    }

    RGPassDesc pass{};
    pass.name = "volumetric_fog";
    pass.execute = executeVolumetricFogPass;
    pass.userData = &userData;
    pass.textureAccesses = accesses;
    pass.textureAccessCount = resolvedCount;
    pass.isCuda = true;
    graph.addPass(pass);
}

} // namespace fuse::renderer
