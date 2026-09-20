#include <fuse/scene/mission_load.hpp>

#include <fuse/project/importer_extract.hpp>

namespace fuse::scene {

MissionLoadResult loadMissionToHandleMap(const std::string& path) {
    MissionLoadResult result;

    const std::string text = fuse::project::readTextFile(path, result.error);
    if (!result.error.empty()) {
        return result;
    }

    const fuse::project::T3DMissionExtract extract =
        fuse::project::extractT3DMissionFields(text);
    result.worldName = extract.missionName;
    result.handles.reserve(extract.simObjects.size());

    for (const fuse::project::T3DSimObjectStub& stub : extract.simObjects) {
        MissionObject object;
        object.className = stub.className;
        object.objectName = stub.objectName;
        object.position = stub.position;
        object.scale = stub.scale;

        const fuse::Handle<MissionObject> handle = result.objects.insert(std::move(object));
        if (MissionObject* stored = result.objects.get(handle)) {
            stored->handle = handle;
        }
        result.handles.push_back(handle);
    }

    result.ok = true;
    return result;
}

} // namespace fuse::scene
