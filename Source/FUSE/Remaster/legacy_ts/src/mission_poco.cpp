// FUSE Remaster W1.4: T3D mission -> POCO Level (see mission_poco.hpp).

#include <fuse/remaster/legacy_ts/mission_poco.hpp>

#include <fuse/project/world_converter.hpp>
#include <fuse/remaster/legacy_ts/poco_cook.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace fuse::remaster::legacy_ts {

namespace fs = std::filesystem;

namespace {

bool fail(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

std::string sanitizeId(const std::string& s) {
    std::string out;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back(std::isalnum(u) != 0 || c == '_' || c == '-' ? static_cast<char>(std::tolower(u)) : '_');
    }
    return out.empty() ? std::string("unnamed") : out;
}

/// Column-major T * R * S.
std::array<float, 16> trs(const fuse::scene::SceneEntityTransform& t) {
    const f32 x = t.rotationX, y = t.rotationY, z = t.rotationZ, w = t.rotationW;
    const f32 r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                         {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                         {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    const f32 s[3] = {t.scaleX, t.scaleY, t.scaleZ};
    std::array<float, 16> m{};
    for (u32 c = 0; c < 3u; ++c) {
        for (u32 row = 0; row < 3u; ++row) {
            m[c * 4u + row] = r[row][c] * s[c];
        }
    }
    m[12] = t.positionX;
    m[13] = t.positionY;
    m[14] = t.positionZ;
    m[15] = 1.f;
    return m;
}

std::string hex64(u64 v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

} // namespace

bool convertT3DMissionToPoco(const fs::path& mission, store::BlobStore& store, db::Database& database,
                             const MissionPocoOptions& options, MissionPocoResult& out, std::string* error) {
    out = MissionPocoResult{};
    const project::T3DMissionInventory inventory = project::inventoryT3DMission(mission.string(), options.searchRoots);
    if (!inventory.ok) {
        return fail(error, mission.filename().string() + ": " + inventory.note);
    }

    // 1 + 2: every distinct TSStatic shape -> POCO (+ DB) -> cooked at the level's asset id.
    for (const project::T3DMissionObject& object : inventory.objects) {
        if (object.archetype != "static_mesh" || object.shapeCookedRelPath.empty()) {
            continue;
        }
        auto existing = std::find_if(out.shapes.begin(), out.shapes.end(), [&](const MissionShapeBinding& b) {
            return b.cookedRelPath == object.shapeCookedRelPath;
        });
        if (existing != out.shapes.end()) {
            existing->entities.push_back(object.name);
            continue;
        }
        if (object.shapeSourcePath.empty()) {
            return fail(error, "TSStatic '" + object.name + "': shape '" + object.shapeRef + "' not found");
        }
        MissionShapeBinding binding;
        binding.shapeRef = object.shapeRef;
        binding.sourcePath = object.shapeSourcePath;
        binding.cookedRelPath = object.shapeCookedRelPath;
        binding.cookedAssetId = object.shapeAssetId;
        binding.entities.push_back(object.name);
        std::string ext = fs::path(object.shapeSourcePath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".dts") {
            return fail(error, "TSStatic '" + object.name + "': only .dts shapes import to POCO here (got '" + ext + "')");
        }
        ImportOptions importOptions;
        importOptions.gameId = options.gameId;
        std::string stem = object.shapeCookedRelPath;
        stem.resize(stem.size() - fs::path(stem).extension().string().size());
        importOptions.idPrefix = sanitizeId(options.gameId) + "." + sanitizeId(stem);
        importOptions.origin = options.origin;
        importOptions.licenceId = options.licenceId;
        ImportedShape imported;
        if (!importTorqueShape(object.shapeSourcePath, {}, store, &database, importOptions, imported, error)) {
            return false;
        }
        binding.oaid = imported.oaid;
        binding.pocoMeshId = imported.lods.front().id;
        if (!options.cookedRoot.empty()) {
            PocoRecordSet records;
            for (const poco::Record* r : imported.all()) {
                records.records.push_back(*r);
            }
            const fs::path outPath = options.cookedRoot / binding.cookedRelPath;
            if (!cookPocoMesh(store, std::get<poco::Mesh>(imported.lods.front().payload), records, outPath, error)) {
                return false;
            }
        }
        out.shapes.push_back(std::move(binding));
    }

    // 3: the Level record.
    out.level.kind = poco::Kind::Level;
    out.level.id = sanitizeId(options.gameId) + ".level." + sanitizeId(mission.stem().string());
    out.level.name = inventory.sceneName.empty() ? mission.stem().string() : inventory.sceneName;
    out.level.units.up = "+Y";
    out.level.provenance.origin = options.origin;
    out.level.provenance.tool = "fuse_legacy_ts";
    out.level.provenance.human_authorship =
        options.origin == poco::Origin::Authored ? poco::HumanAuthorship::Substantial : poco::HumanAuthorship::None;
    out.level.licence_id = options.licenceId;
    out.level.tags = {"torque", "mis"};
    out.level.payload = poco::Level{};
    poco::Level& level = std::get<poco::Level>(out.level.payload);
    for (const project::T3DMissionObject& object : inventory.objects) {
        poco::Entity e;
        e.archetype = object.archetype;
        e.transform = trs(object.transform);
        e.legacyClass = object.className;
        e.legacyName = object.name;
        e.components.push_back(poco::EntityComponent{"parent", std::to_string(object.parent)});
        if (object.archetype.empty()) {
            ++out.unexpected;
            out.unhandledClasses.push_back(object.className);
        }
        if (object.archetype == "static_mesh") {
            for (const MissionShapeBinding& b : out.shapes) {
                if (b.cookedRelPath == object.shapeCookedRelPath) {
                    e.assetRefs = {"oaid:" + b.oaid, "poco:" + b.pocoMeshId};
                    e.components.push_back(poco::EntityComponent{
                        "mesh_asset", "{\"id\":\"" + hex64(b.cookedAssetId.value) + "\",\"path\":\"game:/cooked/" +
                                          b.cookedRelPath + "\"}"});
                }
            }
        }
        level.entities.push_back(std::move(e));
    }
    out.entities = static_cast<u32>(level.entities.size());
    out.level.distribution = poco::computeDistribution(out.level);
    if (!store.writeRecord(out.level, error)) {
        return false;
    }
    return true;
}

} // namespace fuse::remaster::legacy_ts
