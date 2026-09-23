#include <fuse/editor/material_library_io.hpp>

#include <fuse/editor/command_queue.hpp>

#if defined(FUSE_EDITOR_HAS_RHI)
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material/material_system.hpp>
#endif

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace fuse::editor {

namespace {

[[maybe_unused]] constexpr const char* kMagic = "FUSEMATLIB";
[[maybe_unused]] constexpr u32 kVersion = 1u;

#if defined(FUSE_EDITOR_HAS_RHI)

std::string f(f32 value) {
    return formatPropertyFloat(value);
}

void writeMaterial(std::ostream& out, u32 id, const renderer::Material& m) {
    out << "material " << id << "\n";
    out << "baseColor " << f(m.baseColor.x) << ' ' << f(m.baseColor.y) << ' ' << f(m.baseColor.z) << "\n";
    out << "roughness " << f(m.roughness) << "\n";
    out << "metallic " << f(m.metallic) << "\n";
    out << "normalStrength " << f(m.normalStrength) << "\n";
    out << "emissiveColor " << f(m.emissiveColor.x) << ' ' << f(m.emissiveColor.y) << ' ' << f(m.emissiveColor.z)
        << "\n";
    out << "emissiveIntensity " << f(m.emissiveIntensity) << "\n";
    out << "shadingModel " << static_cast<u32>(m.shadingModel) << "\n";
    const renderer::MaterialParameterBlock& p = m.parameters;
    out << "subsurface " << f(p.subsurface.scatterColor.x) << ' ' << f(p.subsurface.scatterColor.y) << ' '
        << f(p.subsurface.scatterColor.z) << ' ' << f(p.subsurface.scatterRadius) << "\n";
    out << "clearCoat " << f(p.clearCoat.clearCoat) << ' ' << f(p.clearCoat.clearCoatRoughness) << "\n";
    out << "cloth " << f(p.cloth.sheenColor.x) << ' ' << f(p.cloth.sheenColor.y) << ' ' << f(p.cloth.sheenColor.z)
        << ' ' << f(p.cloth.sheenRoughness) << "\n";
    out << "procedural " << (m.isProcedural ? 1 : 0) << ' ' << m.proceduralFnId << ' ' << m.proceduralSeed << "\n";
    out << "end\n";
}

bool readFloats(std::istringstream& in, f32* out, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        std::string token;
        if (!(in >> token)) {
            return false;
        }
        char* end = nullptr;
        out[i] = std::strtof(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0') {
            return false;
        }
    }
    return true;
}

#endif

} // namespace

MaterialLibraryIoResult saveMaterialLibrary(const renderer::MaterialSystem& materials, const std::string& path) {
    MaterialLibraryIoResult result{};
#if defined(FUSE_EDITOR_HAS_RHI)
    std::ostringstream out;
    out << kMagic << ' ' << kVersion << "\n";
    out << "count " << materials.materialCount() << "\n";
    for (u32 id = 0; id < materials.materialCount(); ++id) {
        writeMaterial(out, id, materials.get(id));
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        result.error = "cannot open " + path + " for writing";
        return result;
    }
    const std::string text = out.str();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) {
        result.error = "write failed: " + path;
        return result;
    }
    result.ok = true;
    result.materialCount = materials.materialCount();
#else
    (void)materials;
    (void)path;
    result.error = "material system unavailable (built without fuse_rhi)";
#endif
    return result;
}

MaterialLibraryIoResult loadMaterialLibrary(const std::string& path, renderer::MaterialSystem& materials) {
    MaterialLibraryIoResult result{};
#if defined(FUSE_EDITOR_HAS_RHI)
    if (!materials.isReady()) {
        result.error = "material system not initialised";
        return result;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "cannot open " + path;
        return result;
    }

    std::string line;
    std::string magic;
    u32 version = 0;
    if (!std::getline(file, line) || !(std::istringstream(line) >> magic >> version) || magic != kMagic ||
        version != kVersion) {
        result.error = "not a FUSEMATLIB v1 file";
        return result;
    }
    u32 count = 0;
    {
        std::string key;
        if (!std::getline(file, line) || !(std::istringstream(line) >> key >> count) || key != "count") {
            result.error = "missing material count";
            return result;
        }
    }

    // Parse everything first so a malformed file leaves the material system untouched.
    std::vector<renderer::Material> parsed;
    parsed.reserve(count);
    renderer::Material current{};
    bool inMaterial = false;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream in(line);
        std::string key;
        in >> key;
        if (key == "material") {
            u32 id = 0;
            if (inMaterial || !(in >> id) || id != parsed.size()) {
                result.error = "material ids must be sequential";
                return result;
            }
            current = renderer::Material{};
            inMaterial = true;
            continue;
        }
        if (!inMaterial) {
            result.error = "field outside a material block: " + key;
            return result;
        }
        bool ok = true;
        f32 v[4]{};
        if (key == "end") {
            parsed.push_back(current);
            inMaterial = false;
        } else if (key == "baseColor" && (ok = readFloats(in, v, 3))) {
            current.baseColor = {v[0], v[1], v[2]};
        } else if (key == "roughness" && (ok = readFloats(in, v, 1))) {
            current.roughness = v[0];
        } else if (key == "metallic" && (ok = readFloats(in, v, 1))) {
            current.metallic = v[0];
        } else if (key == "normalStrength" && (ok = readFloats(in, v, 1))) {
            current.normalStrength = v[0];
        } else if (key == "emissiveColor" && (ok = readFloats(in, v, 3))) {
            current.emissiveColor = {v[0], v[1], v[2]};
        } else if (key == "emissiveIntensity" && (ok = readFloats(in, v, 1))) {
            current.emissiveIntensity = v[0];
        } else if (key == "shadingModel") {
            u32 model = 0;
            ok = static_cast<bool>(in >> model) && model <= static_cast<u32>(renderer::ShadingModel::Cloth);
            current.shadingModel = static_cast<renderer::ShadingModel>(model);
        } else if (key == "subsurface" && (ok = readFloats(in, v, 4))) {
            current.parameters.subsurface.scatterColor = {v[0], v[1], v[2]};
            current.parameters.subsurface.scatterRadius = v[3];
        } else if (key == "clearCoat" && (ok = readFloats(in, v, 2))) {
            current.parameters.clearCoat.clearCoat = v[0];
            current.parameters.clearCoat.clearCoatRoughness = v[1];
        } else if (key == "cloth" && (ok = readFloats(in, v, 4))) {
            current.parameters.cloth.sheenColor = {v[0], v[1], v[2]};
            current.parameters.cloth.sheenRoughness = v[3];
        } else if (key == "procedural") {
            u32 flag = 0;
            ok = static_cast<bool>(in >> flag >> current.proceduralFnId >> current.proceduralSeed) && flag <= 1u;
            current.isProcedural = flag == 1u;
        } else if (ok) {
            result.error = "unknown field: " + key;
            return result;
        }
        if (!ok) {
            result.error = "malformed field: " + key;
            return result;
        }
    }
    if (inMaterial || parsed.size() != count) {
        result.error = "truncated material library";
        return result;
    }

    for (u32 id = 0; id < count; ++id) {
        if (id < materials.materialCount()) {
            materials.updateMaterial(id, parsed[id]);
        } else if (materials.registerMaterial(parsed[id]) != id) {
            result.error = "material registration failed";
            return result;
        }
    }
    result.ok = true;
    result.materialCount = count;
#else
    (void)path;
    (void)materials;
    result.error = "material system unavailable (built without fuse_rhi)";
#endif
    return result;
}

} // namespace fuse::editor
