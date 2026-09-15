#include <fuse/animation/skinning.hpp>

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
    if (vertexCount == 0 || input.weights.size() != vertexCount) {
        return false;
    }

    output.positions.resize(vertexCount);
    output.normals.resize(vertexCount);

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
            const vec3& restNormal = input.rest_normals[vertex];

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
        output.normals[vertex] = skinnedNormal;
    }

    return true;
}

} // namespace fuse::animation
