#include <fuse/project/t3d_datablock_resolve.hpp>

#include <fuse/scene/scene.hpp>
#include <fuse/scene/wire_stub.hpp>

namespace fuse::project {

namespace {

void appendBinding(T3DDatablockResolveResult& result, const std::string& owner,
                   const std::string& refName, const std::string& kind) {
    if (owner.empty() || refName.empty()) {
        return;
    }

    T3DResolvedBinding binding{};
    binding.owner = owner;
    binding.refName = refName;
    binding.kind = kind;
    binding.resolvedId = hashT3DLegacyRefName(refName);
    result.bindings.push_back(std::move(binding));
    ++result.ownerLinked;

    if (kind == "datablock") {
        ++result.datablockCount;
    } else if (kind == "material") {
        ++result.materialCount;
    }
}

} // namespace

u32 hashT3DLegacyRefName(const std::string& refName) {
    u64 hash = 14695981039346656037ull;
    for (unsigned char ch : refName) {
        hash ^= static_cast<u64>(ch);
        hash *= 1099511628211ull;
    }
    return static_cast<u32>(hash & 0xFFFFFFFFu);
}

T3DDatablockResolveResult resolveT3DMissionBindings(const T3DMissionExtract& extract) {
    T3DDatablockResolveResult result;

    for (const T3DSimObjectStub& object : extract.simObjects) {
        if (!object.datablockRef.empty()) {
            appendBinding(result, object.objectName, object.datablockRef, "datablock");
        }
        if (!object.materialAsset.empty()) {
            appendBinding(result, object.objectName, object.materialAsset, "material");
        }
    }

    return result;
}

T3DDatablockResolveResult resolveT3DBindingsFromScene(const fuse::scene::Scene& scene) {
    T3DDatablockResolveResult result;

    for (const fuse::scene::SceneEntity& entity : scene.entities()) {
        if (!fuse::scene::isWireStubEntityName(entity.name)) {
            continue;
        }

        const fuse::scene::WireStubRef wire = fuse::scene::parseWireStubEntityName(entity.name);
        if (!wire.valid) {
            continue;
        }

        if (wire.kind == "datablock" || wire.kind == "material") {
            appendBinding(result, wire.owner, wire.value, wire.kind);
        }
    }

    return result;
}

} // namespace fuse::project
