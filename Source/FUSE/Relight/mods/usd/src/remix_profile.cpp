// FUSE Relight RL-3.1: the Remix view of a composed mod stage (see remix_profile.hpp).
#include <fuse/relight/mods/usd/remix_profile.hpp>

#include <fuse/relight/hash/hash_string.hpp>

#include <algorithm>
#include <functional>

namespace fuse::relight::mods::usd {

namespace {

constexpr std::string_view kLegacyLightPrefix = "sphereLight_";

bool isLightType(std::string_view t) {
    return t == "SphereLight" || t == "DistantLight" || t == "RectLight" || t == "DiskLight" || t == "CylinderLight" ||
           t == "DomeLight";
}

std::string childPath(const std::string& parent, const std::string& name) { return parent + "/" + name; }

std::vector<std::string> split(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t b = 1;
    while (b <= path.size() && path.size() > 1) {
        std::size_t e = path.find('/', b);
        if (e == std::string_view::npos) {
            e = path.size();
        }
        parts.emplace_back(path.substr(b, e - b));
        b = e + 1;
    }
    return parts;
}

} // namespace

const char* primClassName(PrimClass c) {
    switch (c) {
    case PrimClass::None: return "none";
    case PrimClass::Section: return "section";
    case PrimClass::Mesh: return "mesh";
    case PrimClass::Material: return "material";
    case PrimClass::Light: return "light";
    case PrimClass::Unrecognized: return "unrecognized";
    }
    return "none";
}

Classification classifyPrimPath(std::string_view path) {
    const std::vector<std::string> parts = split(path);
    Classification c;
    if (parts.empty() || parts[0] != "RootNode") {
        return c;
    }
    if (parts.size() == 1) {
        c.cls = PrimClass::Section;
        return c;
    }
    const std::string& section = parts[1];
    if (section != "meshes" && section != "Looks" && section != "lights") {
        return c;
    }
    if (parts.size() == 2) {
        c.cls = PrimClass::Section;
        return c;
    }
    if (parts.size() > 3) {
        return c; // inside a replacement: classified by its root
    }
    const std::string& name = parts[2];
    if (section == "meshes") {
        c.hash = hash::hashFromPrimName(name, hash::prim_prefix::kMesh);
        c.cls = c.hash != 0 ? PrimClass::Mesh : PrimClass::Unrecognized;
    } else if (section == "Looks") {
        c.hash = hash::hashFromPrimName(name, hash::prim_prefix::kMaterial);
        c.cls = c.hash != 0 ? PrimClass::Material : PrimClass::Unrecognized;
    } else {
        // getLightHash: names starting with 's' use the legacy "sphereLight_" prefix.
        if (!name.empty() && name[0] == 's') {
            c.hash = hash::hashFromPrimName(name, kLegacyLightPrefix);
            c.legacyLightName = c.hash != 0;
        } else {
            c.hash = hash::hashFromPrimName(name, hash::prim_prefix::kLight);
        }
        c.cls = c.hash != 0 ? PrimClass::Light : PrimClass::Unrecognized;
    }
    return c;
}

const char* remixCategoryAttributeName(scene::InstanceCategories c) {
    // rtx_instance_manager / rtx_types getInstanceCategorySubKey spellings (note "decal_Static").
    static const char* const kNames[] = {
        "remix_category:world_ui",
        "remix_category:world_matte",
        "remix_category:sky",
        "remix_category:ignore",
        "remix_category:ignore_lights",
        "remix_category:ignore_anti_culling",
        "remix_category:ignore_motion_blur",
        "remix_category:ignore_opacity_micromap",
        "remix_category:ignore_alpha_channel",
        "remix_category:hidden",
        "remix_category:particle",
        "remix_category:beam",
        "remix_category:decal_Static",
        "remix_category:decal_dynamic",
        "remix_category:decal_single_offset",
        "remix_category:decal_no_offset",
        "remix_category:alpha_blend_to_cutout",
        "remix_category:terrain",
        "remix_category:animated_water",
        "remix_category:third_person_player_model",
        "remix_category:third_person_player_body",
        "remix_category:ignore_baked_lighting",
        "remix_category:particle_emitter",
        "remix_category:smooth_normals",
        "remix_category:hair_cards",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == scene::kInstanceCategoryCount);
    const auto i = static_cast<std::uint32_t>(c);
    return i < scene::kInstanceCategoryCount ? kNames[i] : "";
}

CategoryOverrides readCategories(const Prim& prim) {
    CategoryOverrides out;
    for (std::uint32_t i = 0; i < scene::kInstanceCategoryCount; ++i) {
        const auto cat = static_cast<scene::InstanceCategories>(i);
        const Attribute* a = prim.attribute(remixCategoryAttributeName(cat));
        if (!a || !a->hasDefault || a->defaultValue->isNone()) {
            continue; // Get() fails without a value
        }
        out.exists.set(cat);
        if (a->defaultValue->asBool().value_or(false)) {
            out.flags.set(cat);
        }
    }
    return out;
}

const Value* ParticleSystem::get(std::string_view name) const {
    const auto it = primvars.find(std::string(name));
    return it == primvars.end() ? nullptr : &it->second;
}

std::optional<double> ParticleSystem::number(std::string_view name) const {
    const Value* v = get(name);
    return v ? v->asNumber() : std::nullopt;
}

std::optional<ParticleSystem> readParticleSystem(const Prim& prim) {
    if (!prim.hasApiSchema(kParticleSystemApi)) {
        return std::nullopt;
    }
    ParticleSystem ps;
    for (const auto& [name, a] : prim.attributes) {
        if (name.compare(0, kParticlePrimvarPrefix.size(), kParticlePrimvarPrefix) != 0) {
            continue;
        }
        if (a.hasDefault && !a.defaultValue->isNone()) {
            ps.primvars[name.substr(kParticlePrimvarPrefix.size())] = a.defaultValue;
        } else if (!a.timeSamples.empty()) {
            ps.primvars[name.substr(kParticlePrimvarPrefix.size())] = a.timeSamples.front().second;
        }
    }
    return ps;
}

namespace {

/// Remix getStrongestOpinionatedPathHash: the first spec in the prim stack with a property other than xform* and
/// material:binding; XXH64(layer real path) then XXH64(spec path) with that as seed.
std::optional<Hash64> strongestOpinionHash(const Prim& shader) {
    for (const SpecSite& site : shader.stack) {
        for (const std::string& p : site.propertyNames) {
            if (p.compare(0, 5, "xform") == 0 || p == "material:binding") {
                continue;
            }
            Hash64 h = hash::xxh64(site.layer.data(), site.layer.size(), 0);
            h = hash::xxh64(site.path.data(), site.path.size(), h);
            return h;
        }
    }
    return std::nullopt;
}

const Prim* findShader(const ComposedStage& stage, const Prim& material) {
    if (const Prim* s = stage.find(childPath(material.path, "Shader")); s && s->typeName == "Shader") {
        return s;
    }
    for (const std::string& c : material.children) {
        const Prim* s = stage.find(childPath(material.path, c));
        if (s && s->active && s->typeName == "Shader") {
            return s;
        }
    }
    return nullptr;
}

std::string stringAttr(const Prim& p, std::string_view name) {
    const Attribute* a = p.attribute(name);
    if (!a || !a->hasDefault) {
        return "";
    }
    if (a->defaultValue->kind == Value::Kind::Asset || a->defaultValue->kind == Value::Kind::String) {
        return a->defaultValue->text;
    }
    return "";
}

void walkActive(const ComposedStage& stage, const Prim& root, const std::function<void(const Prim&)>& f) {
    for (const std::string& c : root.children) {
        const Prim* p = stage.find(childPath(root.path, c));
        if (!p || !p->active) {
            continue;
        }
        f(*p);
        walkActive(stage, *p, f);
    }
}

} // namespace

RemixMod collectRemixMod(const ComposedStage& stage) {
    RemixMod mod;
    auto sectionChildren = [&](std::string_view section) {
        std::vector<const Prim*> out;
        if (const Prim* s = stage.find(section)) {
            for (const std::string& c : s->children) {
                const Prim* p = stage.find(childPath(s->path, c));
                if (p && p->active) {
                    out.push_back(p);
                }
            }
        }
        return out;
    };
    auto unrecognized = [&](const Prim& p, const char* section) {
        mod.diagnostics.push_back({Diagnostic::Severity::Warning, "unrecognized_prim_name", p.path, "",
                                   std::string("unrecognized prim name under ") + section + ": " + p.name});
    };

    for (const Prim* m : sectionChildren(kLooksSection)) {
        MaterialReplacement r;
        r.path = m->path;
        const Classification c = classifyPrimPath(m->path);
        const Prim* shader = findShader(stage, *m);
        if (shader) {
            r.shaderPath = shader->path;
            r.mdlSourceAsset = stringAttr(*shader, "info:mdl:sourceAsset");
            r.mdlSubIdentifier = stringAttr(*shader, "info:mdl:sourceAsset:subIdentifier");
        }
        if (c.cls == PrimClass::Material) {
            r.hash = c.hash;
        } else if (m->typeName == "Material" && shader) {
            r.hashFromName = false;
            if (auto h = strongestOpinionHash(*shader)) {
                r.hash = *h;
            } else {
                const std::string& name = m->path;
                r.hash = hash::xxh3_64(name.data(), name.size());
            }
        } else {
            unrecognized(*m, "/RootNode/Looks");
            continue;
        }
        r.particles = readParticleSystem(*m);
        mod.materials.push_back(std::move(r));
    }

    for (const Prim* m : sectionChildren(kMeshSection)) {
        const Classification c = classifyPrimPath(m->path);
        if (c.cls != PrimClass::Mesh) {
            unrecognized(*m, "/RootNode/meshes");
            continue;
        }
        MeshReplacement r;
        r.hash = c.hash;
        r.path = m->path;
        r.instanceable = m->instanceable;
        r.categories = readCategories(*m);
        r.particles = readParticleSystem(*m);
        if (const Attribute* a = m->attribute("preserveOriginalDrawCall"); a && a->hasDefault) {
            if (auto b = a->defaultValue->asBool()) {
                r.preserveOriginalDrawCall = *b;
            }
        }
        auto binding = [&](const Prim& p) {
            if (const Relationship* rel = p.relationship("material:binding")) {
                for (const std::string& t : rel->targets) {
                    if (std::find(r.materialBindings.begin(), r.materialBindings.end(), t) == r.materialBindings.end()) {
                        r.materialBindings.push_back(t);
                    }
                }
            }
        };
        binding(*m);
        walkActive(stage, *m, [&](const Prim& p) {
            // The category flags and ParticleSystemAPI are usually authored on the mesh child (an `over` of the
            // captured "mesh" prim when a mod only overrides the original draw): first authoring descendant wins.
            if (!r.categories.exists.any()) {
                r.categories = readCategories(p);
            }
            if (!r.particles) {
                r.particles = readParticleSystem(p);
            }
            if (p.typeName == "Mesh") {
                r.meshPrims.push_back(p.path);
                binding(p);
            } else if (isLightType(p.typeName)) {
                r.lightPrims.push_back(p.path);
            }
        });
        mod.meshes.push_back(std::move(r));
    }

    for (const Prim* l : sectionChildren(kLightsSection)) {
        const Classification c = classifyPrimPath(l->path);
        if (c.cls != PrimClass::Light) {
            unrecognized(*l, "/RootNode/lights");
            continue;
        }
        mod.lights.push_back({c.hash, l->path, l->typeName, c.legacyLightName});
    }
    return mod;
}

} // namespace fuse::relight::mods::usd
