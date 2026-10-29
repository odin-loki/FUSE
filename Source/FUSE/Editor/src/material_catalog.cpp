#include <fuse/editor/material_catalog.hpp>

#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fuse::editor {
namespace {

MaterialEditState make(f32 r, f32 g, f32 b, f32 rough, f32 metal) {
    MaterialEditState s;
    s.baseColorR = r;
    s.baseColorG = g;
    s.baseColorB = b;
    s.roughness = rough;
    s.metallic = metal;
    return s;
}

} // namespace

void MaterialCatalog::resetToDefaults() {
    m_entries = {
        {"Default", make(0.8f, 0.8f, 0.8f, 0.5f, 0.f)},
        {"Red Plastic", make(0.75f, 0.08f, 0.06f, 0.35f, 0.f)},
        {"Gold", make(1.0f, 0.71f, 0.29f, 0.25f, 1.f)},
        {"Chrome", make(0.55f, 0.56f, 0.56f, 0.05f, 1.f)},
        {"Rubber", make(0.05f, 0.05f, 0.05f, 0.9f, 0.f)},
    };
    ++m_revision;
}

void MaterialCatalog::setState(u32 id, const MaterialEditState& state) {
    if (valid(id)) {
        m_entries[id].state = state;
        ++m_revision;
    }
}

u32 MaterialCatalog::add(MaterialCatalogEntry entry) {
    m_entries.push_back(std::move(entry));
    ++m_revision;
    return count() - 1u;
}

void MaterialCatalog::removeLast() {
    if (m_entries.size() > 1u) {
        m_entries.pop_back();
        ++m_revision;
    }
}

bool MaterialCatalog::save(const std::string& path, std::string* error) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    out << "FUSEEDMAT 1\n" << m_entries.size() << '\n' << std::setprecision(9);
    for (const MaterialCatalogEntry& e : m_entries) {
        const MaterialEditState& s = e.state;
        out << std::quoted(e.name) << ' ' << s.baseColorR << ' ' << s.baseColorG << ' ' << s.baseColorB << ' '
            << s.roughness << ' ' << s.metallic << ' ' << unsigned(s.shadingModel) << ' ' << (s.procedural ? 1 : 0)
            << ' ' << s.proceduralFnId << ' ' << s.proceduralSeed << '\n';
    }
    return static_cast<bool>(out);
}

bool MaterialCatalog::load(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    std::string magic;
    int version = 0;
    std::size_t n = 0;
    if (!(in >> magic >> version >> n) || magic != "FUSEEDMAT" || version != 1 || n == 0 || n > 65536) {
        if (error) *error = "not a FUSEEDMAT 1 file";
        return false;
    }
    std::vector<MaterialCatalogEntry> entries(n);
    for (MaterialCatalogEntry& e : entries) {
        MaterialEditState& s = e.state;
        unsigned shading = 0;
        int procedural = 0;
        if (!(in >> std::quoted(e.name) >> s.baseColorR >> s.baseColorG >> s.baseColorB >> s.roughness >>
              s.metallic >> shading >> procedural >> s.proceduralFnId >> s.proceduralSeed)) {
            if (error) *error = "truncated material entry";
            return false;
        }
        s.shadingModel = static_cast<u8>(shading);
        s.procedural = procedural != 0;
    }
    m_entries = std::move(entries);
    ++m_revision;
    return true;
}

bool MaterialEditCommand::merge(const UndoCommand& other) {
    const auto* typed = dynamic_cast<const MaterialEditCommand*>(&other);
    if (typed == nullptr || &typed->m_catalog != &m_catalog || typed->m_id != m_id) {
        return false;
    }
    m_after = typed->m_after;
    return true;
}

bool AssignMaterialCommand::materialOf(const ecs::Registry& registry, ecs::EntityID entity, u32& id) {
    if (!registry.alive(entity)) {
        return false;
    }
    if (const auto* sdf = registry.get<ecs::SDFObject>(entity)) {
        id = sdf->material_id;
        return true;
    }
    if (const auto* mesh = registry.get<ecs::Mesh>(entity)) {
        id = mesh->material_id;
        return true;
    }
    return false;
}

void AssignMaterialCommand::apply_(u32 id) {
    if (!m_registry.alive(m_entity)) {
        return;
    }
    if (auto* sdf = m_registry.get<ecs::SDFObject>(m_entity)) {
        sdf->material_id = id;
    }
    if (auto* mesh = m_registry.get<ecs::Mesh>(m_entity)) {
        mesh->material_id = id;
    }
}

} // namespace fuse::editor
