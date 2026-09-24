/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/lssusd/remix_category_names.h@0867d3c (attribute names) and
// src/dxvk/rtx_render/rtx_game_capturer.cpp@0867d3c (prepExportInstances: instance names).
// FUSE Relight RL-1.8: capture model helpers and the key set (see capture_model.hpp).
#include <fuse/relight/capture/export/capture_model.hpp>

#include <fuse/relight/capture/export/dds.hpp>
#include <fuse/relight/capture/export/digest.hpp>
#include <fuse/relight/hash/hash_string.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::relight::capture::exporter {

Mat4d identity4d() {
    Mat4d m{};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    return m;
}

std::string CaptureInstance::primName() const {
    return std::string(isSky ? "sky_" : "inst_") + hash::hashToString(mesh) + "_" + std::to_string(meshInstNum);
}

const char* remixCategoryAttribute(scene::InstanceCategories category) {
    static const char* const kNames[] = {
        "remix_category:world_ui",
        "remix_category:world_matte",
        "remix_category:sky",
        "remix_category:ignore",
        "remix_category:ignore_lights",
        "remix_category:ignore_anti_culling",
        "remix_category:ignore_motion_blur",
        "remix_category:ignore_opacity_micromap",
        "remix_category:ignore_alpha_channel",
        "remix_category:hidden",
        "remix_category:particle",
        "remix_category:beam",
        "remix_category:decal_Static",
        "remix_category:decal_dynamic",
        "remix_category:decal_single_offset",
        "remix_category:decal_no_offset",
        "remix_category:alpha_blend_to_cutout",
        "remix_category:terrain",
        "remix_category:animated_water",
        "remix_category:third_person_player_model",
        "remix_category:third_person_player_body",
        "remix_category:ignore_baked_lighting",
        "remix_category:particle_emitter",
        "remix_category:smooth_normals",
        "remix_category:hair_cards",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == scene::kInstanceCategoryCount);
    const auto i = static_cast<std::uint32_t>(category);
    return i < scene::kInstanceCategoryCount ? kNames[i] : "";
}

namespace {
void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
}
void putF32(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t u = 0;
    static_assert(sizeof u == sizeof f);
    std::memcpy(&u, &f, 4);
    putU32(out, u);
}
} // namespace

std::vector<std::uint8_t> canonicalMeshStream(const CaptureMesh& mesh) {
    std::vector<std::uint8_t> out;
    out.reserve(8 + mesh.points.size() * 12 + mesh.indices.size() * 4);
    putU32(out, static_cast<std::uint32_t>(mesh.points.size()));
    putU32(out, static_cast<std::uint32_t>(mesh.indices.size()));
    for (const Vec3f& p : mesh.points) {
        putF32(out, p[0]);
        putF32(out, p[1]);
        putF32(out, p[2]);
    }
    for (const std::int32_t i : mesh.indices) {
        putU32(out, static_cast<std::uint32_t>(i));
    }
    return out;
}

std::string canonicalMeshSha256(const CaptureMesh& mesh) {
    const std::vector<std::uint8_t> s = canonicalMeshStream(mesh);
    return sha256Hex(s.data(), s.size());
}

std::string canonicalTextureSha256(const CaptureTexture& texture) {
    if (texture.mip0.empty()) {
        return {};
    }
    const auto rgba = decodeRgba8(static_cast<hash::D3DFormat>(texture.d3dFormat), texture.width, texture.height, texture.mip0);
    return rgba ? sha256Hex(rgba->data(), rgba->size()) : std::string();
}

std::vector<CaptureKey> captureKeys(const CaptureData& capture, hash::HashRule assetRule) {
    std::vector<CaptureKey> keys;
    const std::string ruleId = hash::hashToString(hash::hashRuleId(assetRule));
    for (const auto& [h, mesh] : capture.meshes) {
        keys.push_back({key_algo::kGeomAsset, hash::hashToString(h), ruleId, "mesh"});
        if (mesh.legacy0 != 0) {
            keys.push_back({key_algo::kGeomLegacy0, hash::hashToString(mesh.legacy0),
                            hash::hashToString(hash::hashRuleId(hash::rules::kLegacyAsset0)), "mesh"});
        }
        if (mesh.legacy1 != 0) {
            keys.push_back({key_algo::kGeomLegacy1, hash::hashToString(mesh.legacy1),
                            hash::hashToString(hash::hashRuleId(hash::rules::kLegacyAsset1)), "mesh"});
        }
        keys.push_back({key_algo::kCaptureSha256, canonicalMeshSha256(mesh), "", "mesh"});
    }
    for (const auto& [h, tex] : capture.textures) {
        keys.push_back({tex.obsoleteHash ? key_algo::kTextureObsolete : key_algo::kTexture, hash::hashToString(h), "", "texture"});
        if (tex.descriptorHash != 0) {
            keys.push_back({key_algo::kRtDescriptor, hash::hashToString(tex.descriptorHash), "", "texture"});
        }
        const std::string sha = canonicalTextureSha256(tex);
        if (!sha.empty()) {
            keys.push_back({key_algo::kCaptureSha256, sha, "", "texture"});
        }
    }
    for (const auto& [h, light] : capture.sphereLights) {
        (void)light;
        keys.push_back({key_algo::kLight, hash::hashToString(h), "", "light"});
    }
    for (const auto& [h, light] : capture.distantLights) {
        (void)light;
        keys.push_back({key_algo::kLight, hash::hashToString(h), "", "light"});
    }
    std::sort(keys.begin(), keys.end());
    // Identical content under two Remix keys (two meshes with the same vertices, or the same pixels
    // uploaded twice) shares one canonical key; the first row wins (the DB maps a key to one asset).
    keys.erase(std::unique(keys.begin(), keys.end(),
                           [](const CaptureKey& a, const CaptureKey& b) { return a.algo == b.algo && a.value == b.value; }),
               keys.end());
    return keys;
}

} // namespace fuse::relight::capture::exporter
