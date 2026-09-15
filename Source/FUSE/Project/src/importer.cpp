#include <fuse/project/importer.hpp>

#include <fuse/log/logger.hpp>

#include <cctype>
#include <fstream>
#include <sstream>

namespace fuse::project {

namespace {

std::string readFileToString(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string extractT3DMissionName(const std::string& text) {
    const std::string markers[] = {"new Scene(", "new SimGroup("};
    for (const std::string& marker : markers) {
        const std::size_t pos = text.find(marker);
        if (pos == std::string::npos) {
            continue;
        }

        std::size_t cursor = pos + marker.size();
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }

        std::size_t end = cursor;
        while (end < text.size()) {
            const char ch = text[end];
            if (ch == ')' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '{') {
                break;
            }
            ++end;
        }

        if (end > cursor) {
            return text.substr(cursor, end - cursor);
        }
    }
    return "UnnamedMission";
}

std::string extractT2DModuleName(const std::string& text, const std::string& fallbackPath) {
    const std::string markers[] = {"module \"", "module @"};
    for (const std::string& marker : markers) {
        const std::size_t pos = text.find(marker);
        if (pos == std::string::npos) {
            continue;
        }

        std::size_t cursor = pos + marker.size();
        if (marker == "module @") {
            while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
                ++cursor;
            }
        }

        std::size_t end = cursor;
        while (end < text.size()) {
            const char ch = text[end];
            if (ch == '"' || ch == ';' || ch == '\n' || ch == '\r') {
                break;
            }
            ++end;
        }

        if (end > cursor) {
            return text.substr(cursor, end - cursor);
        }
    }

    const std::size_t slash = fallbackPath.find_last_of("/\\");
    if (slash != std::string::npos) {
        std::string leaf = fallbackPath.substr(slash + 1);
        const std::size_t dot = leaf.find('.');
        if (dot != std::string::npos) {
            leaf = leaf.substr(0, dot);
        }
        if (!leaf.empty()) {
            return leaf;
        }
    }

    return "UnnamedModule";
}

dimension::WorldHandle makeWorldHandle(u32 index) {
    return dimension::WorldHandle(index, 1u);
}

ImportRecord makeFuselevelPlaceholder(const std::string& fuselevelPath,
                                    u32 worldIndex,
                                    const char* dimensionNote) {
    ImportRecord record;
    record.sourcePath = fuselevelPath;

    const std::size_t slash = fuselevelPath.find_last_of("/\\");
    std::string leaf = slash != std::string::npos ? fuselevelPath.substr(slash + 1) : fuselevelPath;
    const std::size_t dot = leaf.find('.');
    if (dot != std::string::npos) {
        leaf = leaf.substr(0, dot);
    }
    record.worldName = leaf.empty() ? "FuselevelWorld" : leaf;
    record.worldHandle = makeWorldHandle(worldIndex);
    record.ok = true;
    record.note = dimensionNote;
    return record;
}

} // namespace

const char* importSourceKindName(ImportSourceKind kind) {
    switch (kind) {
    case ImportSourceKind::T3DMission:
        return "t3d_mission";
    case ImportSourceKind::T2DModule:
        return "t2d_module";
    }
    return "unknown";
}

ImportRecord importT3DMission(const std::string& missionPath, u32 worldIndex) {
    ImportRecord record;
    record.kind = ImportSourceKind::T3DMission;
    record.sourcePath = missionPath;

    const std::string text = readFileToString(missionPath);
    if (text.empty()) {
        record.note = "unable to read mission file";
        return record;
    }

    record.worldName = extractT3DMissionName(text);
    record.worldHandle = makeWorldHandle(worldIndex);
    record.ok = true;
    record.note = "stub World3D placeholder registered";
    log::info("importT3DMission: %s -> world '%s' handle=%u",
              missionPath.c_str(),
              record.worldName.c_str(),
              record.worldHandle.index());
    return record;
}

ImportRecord importT2DModule(const std::string& modulePath, u32 worldIndex) {
    ImportRecord record;
    record.kind = ImportSourceKind::T2DModule;
    record.sourcePath = modulePath;

    const std::string text = readFileToString(modulePath);
    if (text.empty()) {
        record.note = "unable to read module file";
        return record;
    }

    record.worldName = extractT2DModuleName(text, modulePath);
    record.worldHandle = makeWorldHandle(worldIndex);
    record.ok = true;
    record.note = "stub World2D placeholder registered";
    log::info("importT2DModule: %s -> world '%s' handle=%u",
              modulePath.c_str(),
              record.worldName.c_str(),
              record.worldHandle.index());
    return record;
}

ImportDryRunResult importDryRun(const ProjectManifest& project, const std::string& sourcePath) {
    ImportDryRunResult result;
    u32 nextIndex = 1u;

    if (endsWith(sourcePath, ".mis")) {
        ImportRecord record = importT3DMission(sourcePath, nextIndex++);
        result.worlds.push_back(record);
        result.ok = record.ok;
        result.summary = record.ok ? "t3d mission import dry-run ok" : record.note;
        return result;
    }

    if (endsWith(sourcePath, ".cs") || endsWith(sourcePath, "main.cs")) {
        ImportRecord record = importT2DModule(sourcePath, nextIndex++);
        result.worlds.push_back(record);
        result.ok = record.ok;
        result.summary = record.ok ? "t2d module import dry-run ok" : record.note;
        return result;
    }

    if (project.dimensions.enable3D && !project.defaultWorld3D.empty()) {
        std::string worldPath = project.projectRoot;
        if (!worldPath.empty() && worldPath.back() != '/' && worldPath.back() != '\\') {
            worldPath.push_back('/');
        }
        worldPath += project.defaultWorld3D;

        ImportRecord record;
        if (endsWith(worldPath, ".mis")) {
            record = importT3DMission(worldPath, nextIndex++);
            record.kind = ImportSourceKind::T3DMission;
        } else if (endsWith(worldPath, ".fuselevel")) {
            record = makeFuselevelPlaceholder(worldPath, nextIndex++, "stub World3D .fuselevel placeholder");
            record.kind = ImportSourceKind::T3DMission;
        } else {
            record = importT3DMission(worldPath, nextIndex++);
            record.kind = ImportSourceKind::T3DMission;
        }
        result.worlds.push_back(record);
    }

    if (project.dimensions.enable2D && !project.defaultWorld2D.empty()) {
        std::string worldPath = project.projectRoot;
        if (!worldPath.empty() && worldPath.back() != '/' && worldPath.back() != '\\') {
            worldPath.push_back('/');
        }
        worldPath += project.defaultWorld2D;

        ImportRecord record;
        if (endsWith(worldPath, ".cs")) {
            record = importT2DModule(worldPath, nextIndex++);
            record.kind = ImportSourceKind::T2DModule;
        } else if (endsWith(worldPath, ".fuselevel")) {
            record = makeFuselevelPlaceholder(worldPath, nextIndex++, "stub World2D .fuselevel placeholder");
            record.kind = ImportSourceKind::T2DModule;
        } else {
            record = importT2DModule(worldPath, nextIndex++);
            record.kind = ImportSourceKind::T2DModule;
        }
        result.worlds.push_back(record);
    }

    result.ok = true;
    for (const ImportRecord& record : result.worlds) {
        if (!record.ok) {
            result.ok = false;
            break;
        }
    }

    if (result.worlds.empty()) {
        result.ok = false;
        result.summary = "no import targets resolved";
    } else if (result.ok) {
        result.summary = "project import dry-run ok (" + std::to_string(result.worlds.size()) + " worlds)";
    } else {
        result.summary = "project import dry-run failed";
    }

    return result;
}

} // namespace fuse::project
