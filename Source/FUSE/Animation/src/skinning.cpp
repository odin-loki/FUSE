#include <fuse/animation/skinning.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::animation {

SkinningBackend skinning_backend() {
#if defined(FUSE_HAS_CUDA)
    return SkinningBackend::Cuda;
#else
    return SkinningBackend::CpuReference;
#endif
}

bool skin_vertices(const SkinningInput& input, SkinningOutput& output) {
    const u32 vertexCount = static_cast<u32>(input.rest_positions.size());
    const bool hasNormals = !input.rest_normals.empty();
    if (vertexCount == 0 || input.weights.size() != vertexCount ||
        (hasNormals && input.rest_normals.size() != vertexCount)) {
        return false;
    }

    output.positions.resize(vertexCount);
    output.normals.resize(hasNormals ? vertexCount : 0);

    for (u32 vertex = 0; vertex < vertexCount; ++vertex) {
        vec3 skinnedPos = {};
        vec3 skinnedNormal = {};

        for (u32 influence = 0; influence < 4; ++influence) {
            const f32 weight = input.weights[vertex].bone_weights[influence];
            if (weight <= 0.f) {
                continue;
            }

            const u32 boneIndex = input.weights[vertex].bone_indices[influence];
            if (boneIndex >= input.bone_transforms.size()) {
                continue;
            }

            const mat4& bone = input.bone_transforms[boneIndex];
            const vec3& restPos = input.rest_positions[vertex];
            const vec3 restNormal = hasNormals ? input.rest_normals[vertex] : vec3{};

            const vec3 transformed = {
                bone.data[0] * restPos.x + bone.data[4] * restPos.y + bone.data[8] * restPos.z + bone.data[12],
                bone.data[1] * restPos.x + bone.data[5] * restPos.y + bone.data[9] * restPos.z + bone.data[13],
                bone.data[2] * restPos.x + bone.data[6] * restPos.y + bone.data[10] * restPos.z + bone.data[14],
                0.f,
            };

            const vec3 transformedNormal = {
                bone.data[0] * restNormal.x + bone.data[4] * restNormal.y + bone.data[8] * restNormal.z,
                bone.data[1] * restNormal.x + bone.data[5] * restNormal.y + bone.data[9] * restNormal.z,
                bone.data[2] * restNormal.x + bone.data[6] * restNormal.y + bone.data[10] * restNormal.z,
                0.f,
            };

            skinnedPos.x += transformed.x * weight;
            skinnedPos.y += transformed.y * weight;
            skinnedPos.z += transformed.z * weight;
            skinnedNormal.x += transformedNormal.x * weight;
            skinnedNormal.y += transformedNormal.y * weight;
            skinnedNormal.z += transformedNormal.z * weight;
        }

        output.positions[vertex] = skinnedPos;
        if (hasNormals) {
            const f32 len = std::sqrt(skinnedNormal.x * skinnedNormal.x + skinnedNormal.y * skinnedNormal.y +
                                      skinnedNormal.z * skinnedNormal.z);
            if (len > 1e-8f) {
                skinnedNormal = {skinnedNormal.x / len, skinnedNormal.y / len, skinnedNormal.z / len, 0.f};
            }
            output.normals[vertex] = skinnedNormal;
        }
    }

    return true;
}

void compute_skinning_palette(const Skeleton& skel, const Pose& pose, std::vector<mat4>& out) {
    const u32 count = std::min(static_cast<u32>(skel.bones.size()),
                               static_cast<u32>(pose.bone_world_transforms.size()));
    out.resize(count);
    for (u32 i = 0; i < count; ++i) {
        out[i] = mat4_multiply(pose.bone_world_transforms[i], skel.bones[i].inverse_bind);
    }
}

} // namespace fuse::animation
