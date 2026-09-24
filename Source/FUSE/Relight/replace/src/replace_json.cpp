// FUSE Relight RL-3.4: the replaced scene in the capture record (see replace_json.hpp).
#include <fuse/relight/replace/replace_json.hpp>

#include <fuse/relight/hash/hash_string.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fuse::relight::replace {

using capture::exporter::json::Value;

namespace {

Value str(const std::string& s) { return Value::string(s); }

/// 6 significant digits (and no negative zero).
double round6(double v) {
    if (!std::isfinite(v) || v == 0.0) {
        return std::isfinite(v) ? 0.0 : v;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    const double r = std::strtod(buf, nullptr);
    return r == 0.0 ? 0.0 : r;
}

Value num(double v) { return Value::number(round6(v)); }

Value vec3(const Vec3d& v) {
    Value a = Value::array();
    for (double d : v) {
        a.push(num(d));
    }
    return a;
}

Value names(const std::vector<std::string>& n) {
    Value a = Value::array();
    for (const std::string& s : n) {
        a.push(str(s));
    }
    return a;
}

Value hashes(const std::vector<Hash64>& h) {
    Value a = Value::array();
    for (Hash64 x : h) {
        a.push(str(hash::hashToString(x)));
    }
    return a;
}

} // namespace

Value replacedLightJson(const ReplacedLight& l) {
    Value o = Value::object();
    o["origin"] = str(lightOriginName(l.origin));
    o["hash"] = l.origin == ReplacedLight::Origin::Attached ? Value() : str(hash::hashToString(l.gameHash));
    o["mod"] = l.mod.empty() ? Value() : str(l.mod);
    o["record"] = l.recordId.empty() ? Value() : str(l.recordId);
    o["type"] = str(l.type);
    o["position"] = vec3(l.position);
    o["direction"] = vec3(l.direction);
    o["color"] = vec3(l.color);
    o["intensity"] = num(l.intensity);
    if (l.origin == ReplacedLight::Origin::Attached) {
        o["instance"] = Value::number(double(l.instanceId));
        o["draw"] = Value::number(double(l.drawIndex));
    }
    return o;
}

Value replacedDrawJson(const ReplacedDraw& d) {
    if (!d.affected()) {
        return Value();
    }
    Value o = Value::object();
    o["instance"] = Value::number(double(d.instanceId));
    if (d.meshReplaced) {
        Value m = Value::object();
        m["mod"] = str(d.meshMod);
        m["record"] = str(d.meshRecord);
        m["key"] = str(hash::hashToString(d.meshKey));
        m["rule"] = str(d.meshRule);
        m["preserve"] = Value::boolean(d.drawOriginal);
        m["shadowed"] = names(d.meshShadowed);
        Value parts = Value::array();
        for (const ReplacedPart& p : d.parts) {
            Value po = Value::object();
            po["mesh"] = str(p.meshId);
            po["material"] = p.material.empty() ? Value() : str(p.material);
            po["material_mod"] = p.materialMod.empty() ? Value() : str(p.materialMod);
            po["material_source"] = str(p.materialSource);
            po["translation"] = vec3({p.objectToWorld[12], p.objectToWorld[13], p.objectToWorld[14]});
            parts.push(std::move(po));
        }
        m["parts"] = std::move(parts);
        o["mesh"] = std::move(m);
    } else {
        o["mesh"] = Value();
    }
    o["draw_original"] = Value::boolean(d.drawOriginal);
    if (d.materialReplaced) {
        Value m = Value::object();
        m["mod"] = str(d.materialMod);
        m["record"] = str(d.materialRecord);
        m["ignore"] = Value::boolean(d.materialIgnored);
        m["shadowed"] = names(d.materialShadowed);
        o["material"] = std::move(m);
    } else {
        o["material"] = Value();
    }
    o["categories"] = str(d.categories.toString());
    Value lights = Value::array();
    for (const ReplacedLight& l : d.lights) {
        lights.push(replacedLightJson(l));
    }
    o["lights"] = std::move(lights);
    return o;
}

Value replacedFrameJson(const ReplacedFrame& f, const std::vector<ModInfo>& mods, const std::string& watchBackend) {
    Value o = Value::object();
    o["frame"] = Value::number(double(f.frame));
    o["generation"] = Value::number(double(f.generation));
    o["watch"] = str(watchBackend);
    Value ms = Value::array();
    for (const ModInfo& m : mods) {
        Value mo = Value::object();
        mo["name"] = str(m.location.name);
        mo["kind"] = str(modKindName(m.location.kind));
        mo["format"] = str(modFormatName(m.location.format));
        mo["rank"] = Value::number(m.rank);
        mo["priority"] = m.priority ? Value::number(double(*m.priority)) : Value();
        mo["layer"] = m.optionLayer.empty() ? Value() : str(m.optionLayer);
        mo["ok"] = Value::boolean(m.ok);
        mo["meshes"] = Value::number(double(m.meshes));
        mo["materials"] = Value::number(double(m.materials));
        mo["lights"] = Value::number(double(m.lights));
        mo["deleted_lights"] = Value::number(double(m.deletedLights));
        mo["errors"] = Value::number(double(m.errors));
        ms.push(std::move(mo));
    }
    o["mods"] = std::move(ms);
    const ReplaceFrameStats& s = f.stats;
    Value st = Value::object();
    st["draws"] = Value::number(s.draws);
    st["mesh_replaced"] = Value::number(s.meshReplaced);
    st["hidden"] = Value::number(s.hidden);
    st["preserved"] = Value::number(s.preserved);
    st["parts"] = Value::number(s.parts);
    st["material_replaced"] = Value::number(s.materialReplaced);
    st["part_material_replaced"] = Value::number(s.partMaterialReplaced);
    st["lights_game"] = Value::number(s.lightsGame);
    st["lights_replaced"] = Value::number(s.lightsReplaced);
    st["lights_deleted"] = Value::number(s.lightsDeleted);
    st["lights_attached"] = Value::number(s.lightsAttached);
    o["stats"] = std::move(st);
    Value lights = Value::array();
    for (const ReplacedLight& l : f.lights) {
        lights.push(replacedLightJson(l));
    }
    o["lights"] = std::move(lights);
    o["deleted_lights"] = hashes(f.deletedLights);
    if (f.reload) {
        const ReloadEvent& r = *f.reload;
        Value ro = Value::object();
        ro["generation"] = Value::number(double(r.generation));
        ro["notified_frame"] = Value::number(double(r.notifiedFrame));
        ro["applied_frame"] = Value::number(double(r.appliedFrame));
        ro["latency_frames"] = Value::number(double(r.appliedFrame - r.notifiedFrame));
        ro["changed"] = names(r.changed);
        ro["added"] = names(r.added);
        ro["removed"] = names(r.removed);
        ro["failed"] = names(r.failed);
        Value inv = Value::object();
        inv["meshes"] = hashes(r.invalidated.meshes);
        inv["materials"] = hashes(r.invalidated.materials);
        inv["lights"] = hashes(r.invalidated.lights);
        ro["invalidated"] = std::move(inv);
        o["reload"] = std::move(ro);
    } else {
        o["reload"] = Value();
    }
    const ResidencyStats& r = f.residency;
    Value rs = Value::object();
    rs["budget_bytes"] = Value::number(double(r.budgetBytes));
    rs["resident_bytes"] = Value::number(double(r.residentBytes));
    rs["resident"] = Value::number(r.resident);
    rs["requested"] = Value::number(r.requested);
    rs["loaded"] = Value::number(r.loaded);
    rs["evicted"] = Value::number(r.evicted);
    rs["demoted"] = Value::number(r.demoted);
    rs["over_budget"] = Value::boolean(r.overBudget);
    o["residency"] = std::move(rs);
    return o;
}

} // namespace fuse::relight::replace
