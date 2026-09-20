#include <fuse/project/mission_load.hpp>

#include <fuse/project/importer_extract.hpp>
#include <fuse/types.hpp>

#include <string_view>
#include <vector>

namespace fuse::project {

MissionLoadResult loadMissionFile(const std::string& path) {
    MissionLoadResult result;

    const std::string text = readTextFile(path, result.error);
    if (!result.error.empty()) {
        return result;
    }

    const T3DMissionExtract extract = extractT3DMissionFields(text);
    result.worldName = extract.missionName;
    result.handles.reserve(extract.simObjects.size());

    for (const T3DSimObjectStub& stub : extract.simObjects) {
        MissionObject object;
        object.className = stub.className;
        object.name = stub.objectName;
        object.position = stub.position;
        object.scale = stub.scale;
        object.datablockRef = stub.datablockRef;
        result.handles.push_back(result.objects.insert(std::move(object)));
    }

    s32 depth = 0;
    std::size_t objectIndex = 0;
    fuse::Handle<MissionObject> lastInserted = fuse::Handle<MissionObject>::invalid();
    std::vector<fuse::Handle<MissionObject>> parentAtDepth(1, fuse::Handle<MissionObject>::invalid());

    std::size_t lineBegin = 0;
    while (lineBegin < text.size()) {
        std::size_t lineEnd = text.find('\n', lineBegin);
        if (lineEnd == std::string::npos) {
            lineEnd = text.size();
        }

        const std::string_view line(text.data() + lineBegin, lineEnd - lineBegin);
        lineBegin = lineEnd + 1;

        if (line.find("new ") != std::string_view::npos && objectIndex < result.handles.size()) {
            const fuse::Handle<MissionObject> handle = result.handles[objectIndex++];
            MissionObject* object = result.objects.get(handle);
            if (object != nullptr && depth > 0 &&
                static_cast<std::size_t>(depth) < parentAtDepth.size()) {
                object->parent = parentAtDepth[static_cast<std::size_t>(depth)];
            }
            if (depth <= 0) {
                result.roots.push_back(handle);
            }
            lastInserted = handle;
        }

        for (char ch : line) {
            if (ch == '{') {
                ++depth;
                if (parentAtDepth.size() <= static_cast<std::size_t>(depth)) {
                    parentAtDepth.resize(static_cast<std::size_t>(depth) + 1);
                }
                parentAtDepth[static_cast<std::size_t>(depth)] = lastInserted;
            } else if (ch == '}') {
                --depth;
                if (depth < 0) {
                    depth = 0;
                }
            }
        }
    }

    if (result.roots.empty() && !result.handles.empty()) {
        result.roots.push_back(result.handles.front());
    }

    result.ok = true;
    return result;
}

} // namespace fuse::project
