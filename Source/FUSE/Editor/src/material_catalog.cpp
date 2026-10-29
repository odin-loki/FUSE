#include <fuse/editor/material_catalog.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <atomic>

#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fuse::editor {

u32 nextCatalogRevision() {
    // Process-wide so a fresh catalog (new / opened scene) never repeats a revision a consumer saw.
    static std::atomic<u32> counter{0};
    return ++counter;
}

world3d::RenderMaterial3D MaterialCatalog::toRenderMaterial(const MaterialEditState& state) {
    world3d::RenderMaterial3D m{};
    m.baseColor[0] = state.baseColorR;
    m.baseColor[1] = state.baseColorG;
    m.baseColor[2] = state.baseColorB;
    m.metallic = state.metallic;
    m.roughness = state.roughness;
    m.baseColorTexture = state.baseColorTexture;
    m.shader = state.procedural ? state.proceduralFnId : 0u;
    m.uvScale = state.uvScale > 0.f ? state.uvScale : 1.f;
    m.triplanar = true;
    return m;
}

u32 MaterialCatalog::applyTo(world3d::World3D& world) const {
    for (u32 id = 0; id < count(); ++id) {
        world.setMaterial(id, toRenderMaterial(m_entries[id].state));
    }
    return count();
}

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
    m_revision = nextCatalogRevision();
}

void MaterialCatalog::setState(u32 id, const MaterialEditState& state) {
    if (valid(id)) {
        m_entries[id].state = state;
        m_revision = nextCatalogRevision();
    }
}

void MaterialCatalog::setName(u32 id, std::string name) {
    if (valid(id)) {
        m_entries[id].name = std::move(name);
        m_revision = nextCatalogRevision();
    }
}

const std::vector<MaterialPreset>& materialPresets() {
    static const std::vector<MaterialPreset> presets = [] {
        const auto pbr = [](f32 r, f32 g, f32 b, f32 metal, f32 rough) {
            MaterialEditState s;
            s.baseColorR = r;
            s.baseColorG = g;
            s.baseColorB = b;
            s.metallic = metal;
            s.roughness = rough;
            return s;
        };
        const auto proc = [](u32 fn, f32 rough, f32 metal) {
            MaterialEditState s;
            s.procedural = true;
            s.proceduralFnId = fn;
            s.roughness = rough;
            s.metallic = metal;
            return s;
        };
        return std::vector<MaterialPreset>{
            {"Gold", pbr(1.000f, 0.766f, 0.336f, 1.f, 0.30f)},
            {"Silver", pbr(0.972f, 0.960f, 0.915f, 1.f, 0.20f)},
            {"Copper", pbr(0.955f, 0.638f, 0.538f, 1.f, 0.35f)},
            {"Chrome", pbr(0.550f, 0.556f, 0.554f, 1.f, 0.05f)},
            {"Brushed steel", pbr(0.560f, 0.570f, 0.580f, 1.f, 0.55f)},
            {"White plastic", pbr(0.800f, 0.800f, 0.800f, 0.f, 0.40f)},
            {"Red plastic", pbr(0.700f, 0.040f, 0.030f, 0.f, 0.35f)},
            {"Rubber", pbr(0.020f, 0.020f, 0.020f, 0.f, 0.90f)},
            {"Clay", pbr(0.450f, 0.250f, 0.150f, 0.f, 0.85f)},
            {"Wood (procedural)", proc(1u, 0.7f, 0.f)},
            {"Metal (procedural)", proc(2u, 0.4f, 1.f)},
            {"Concrete (procedural)", proc(3u, 0.9f, 0.f)},
        };
    }();
    return presets;
}

u32 MaterialCatalog::add(MaterialCatalogEntry entry) {
    m_entries.push_back(std::move(entry));
    m_revision = nextCatalogRevision();
    return count() - 1u;
}

void MaterialCatalog::removeLast() {
    if (m_entries.size() > 1u) {
        m_entries.pop_back();
        m_revision = nextCatalogRevision();
    }
}

bool MaterialCatalog::save(const std::string& path, std::string* error) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    out << "FUSEEDMAT 2\n" << m_entries.size() << '\n' << std::setprecision(9);
    for (const MaterialCatalogEntry& e : m_entries) {
        const MaterialEditState& s = e.state;
        out << std::quoted(e.name) << ' ' << s.baseColorR << ' ' << s.baseColorG << ' ' << s.baseColorB << ' '
            << s.roughness << ' ' << s.metallic << ' ' << unsigned(s.shadingModel) << ' ' << (s.procedural ? 1 : 0)
            << ' ' << s.proceduralFnId << ' ' << s.proceduralSeed << ' ' << std::quoted(s.baseColorTexture) << ' '
            << s.uvScale << '\n';
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
    if (!(in >> magic >> version >> n) || magic != "FUSEEDMAT" || version < 1 || version > 2 || n == 0 || n > 65536) {
        if (error) *error = "not a FUSEEDMAT 1/2 file";
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
        if (version >= 2 && !(in >> std::quoted(s.baseColorTexture) >> s.uvScale)) {
            if (error) *error = "truncated material entry";
            return false;
        }
        s.shadingModel = static_cast<u8>(shading);
        s.procedural = procedural != 0;
    }
    m_entries = std::move(entries);
    m_revision = nextCatalogRevision();
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
