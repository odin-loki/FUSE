/*
* Copyright (c) 2021-2023, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_mod_usd.cpp@0867d3c (processParticleSystem),
// src/lssusd/particle_system_helpers.h@0867d3c
// (primvar conversions) and src/dxvk/rtx_render/rtx_particle_system.cpp@0867d3c (resolveSpawnPrevTransform)
// Portions Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved. (src/lssusd/particle_system_helpers.h@0867d3c),
// same MIT licence.
// Portions Copyright (c) 2025-2026, NVIDIA CORPORATION. All rights reserved.
// (src/dxvk/rtx_render/rtx_particle_system.cpp@0867d3c),
// same MIT licence.

// FUSE Relight RL-3.6: particle systems from mods and the emitter bridge (see particle_mods.hpp).
//
// descFromPrimvars follows dxvk-remix src/dxvk/rtx_render/rtx_mod_usd.cpp processParticleSystem and
// src/lssusd/particle_system_helpers.h (token and value conversions) @0867d3c (MIT; facts: names, fallbacks, the
// legacy fallback order, the scalar-to-vector broadcast of ConvertPrimvarValue); resolveSpawnPrevTransform is ported
// from rtx_particle_system.cpp (MIT; notice above).
#include <fuse/relight/particles/particle_mods.hpp>

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/mods/import/mod_importer.hpp>
#include <fuse/relight/mods/usd/usd_value.hpp>
#include <fuse/relight/particles/curve_bake.hpp>
#include <fuse/relight/particles/particle_options.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fuse::relight::particles {

namespace {

// ---- primvar reading ----------------------------------------------------------------------------------------------

struct Reader {
    const json::Value& pv;
    std::vector<std::string>* issues;

    const json::Value* get(const std::string& name) const {
        const json::Value* v = pv.get(name);
        return v != nullptr && !v->isNull() ? v : nullptr;
    }
    void issue(const std::string& name, const char* what) const {
        if (issues != nullptr) {
            issues->push_back(name + ": " + what);
        }
    }
    bool number(const std::string& name, float& out) const {
        const json::Value* v = get(name);
        if (v == nullptr) {
            return false;
        }
        if (!v->isNumber()) {
            issue(name, "not a number");
            return false;
        }
        out = static_cast<float>(v->n);
        return true;
    }
    bool integer(const std::string& name, std::uint32_t& out) const {
        float f = 0.f;
        if (!number(name, f)) {
            return false;
        }
        out = f <= 0.f ? 0u : static_cast<std::uint32_t>(std::min(f, 1.0e9f));
        return true;
    }
    bool boolean(const std::string& name, bool& out) const {
        const json::Value* v = get(name);
        if (v == nullptr) {
            return false;
        }
        if (v->kind == json::Value::Kind::Bool) {
            out = v->b;
            return true;
        }
        if (v->isNumber()) {
            out = v->n != 0.0;
            return true;
        }
        issue(name, "not a bool");
        return false;
    }
    bool token(const std::string& name, std::string& out) const {
        const json::Value* v = get(name);
        if (v == nullptr) {
            return false;
        }
        if (!v->isString()) {
            issue(name, "not a token");
            return false;
        }
        out = v->s;
        return true;
    }
    /// An N-tuple, or a scalar broadcast to every component (ConvertPrimvarValue).
    bool tuple(const std::string& name, std::size_t n, float* out) const {
        const json::Value* v = get(name);
        if (v == nullptr) {
            return false;
        }
        if (v->isNumber()) {
            std::fill(out, out + n, static_cast<float>(v->n));
            return true;
        }
        if (!v->isArray() || v->a.size() != n) {
            issue(name, "wrong tuple size");
            return false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!v->a[i].isNumber()) {
                issue(name, "non-numeric tuple");
                return false;
            }
            out[i] = static_cast<float>(v->a[i].n);
        }
        return true;
    }
    /// A non-empty array of numbers.
    bool floats(const std::string& name, std::vector<float>& out) const {
        const json::Value* v = get(name);
        if (v == nullptr || !v->isArray() || v->a.empty()) {
            return false;
        }
        out.clear();
        for (const json::Value& e : v->a) {
            if (!e.isNumber()) {
                issue(name, "non-numeric array");
                out.clear();
                return false;
            }
            out.push_back(static_cast<float>(e.n));
        }
        return true;
    }
    bool tokens(const std::string& name, std::vector<std::string>& out) const {
        const json::Value* v = get(name);
        if (v == nullptr || !v->isArray() || v->a.empty()) {
            return false;
        }
        out.clear();
        for (const json::Value& e : v->a) {
            out.push_back(e.isString() ? e.s : std::string());
        }
        return true;
    }
    bool bools(const std::string& name, std::vector<bool>& out) const {
        const json::Value* v = get(name);
        if (v == nullptr || !v->isArray() || v->a.empty()) {
            return false;
        }
        out.clear();
        for (const json::Value& e : v->a) {
            out.push_back(e.kind == json::Value::Kind::Bool ? e.b : (e.isNumber() && e.n != 0.0));
        }
        return true;
    }
    bool vec4s(const std::string& name, std::vector<std::array<float, 4>>& out) const {
        const json::Value* v = get(name);
        if (v == nullptr || !v->isArray() || v->a.empty()) {
            return false;
        }
        out.clear();
        for (const json::Value& e : v->a) {
            if (!e.isArray() || e.a.size() != 4) {
                issue(name, "not a float4 array");
                out.clear();
                return false;
            }
            std::array<float, 4> c{};
            for (int i = 0; i < 4; ++i) {
                c[static_cast<std::size_t>(i)] = e.a[static_cast<std::size_t>(i)].isNumber() ? static_cast<float>(e.a[static_cast<std::size_t>(i)].n) : 0.f;
            }
            out.push_back(c);
        }
        return true;
    }

    FloatCurve curve(const std::string& base) const {
        FloatCurve c;
        floats(base + ":times", c.times);
        floats(base + ":values", c.values);
        std::vector<std::string> t;
        if (tokens(base + ":inTangentTypes", t)) {
            for (const std::string& s : t) {
                c.inTangentTypes.push_back(parseTangentType(s));
            }
        }
        if (tokens(base + ":outTangentTypes", t)) {
            for (const std::string& s : t) {
                c.outTangentTypes.push_back(parseTangentType(s));
            }
        }
        floats(base + ":inTangentValues", c.inTangentValues);
        floats(base + ":outTangentValues", c.outTangentValues);
        floats(base + ":inTangentTimes", c.inTangentTimes);
        floats(base + ":outTangentTimes", c.outTangentTimes);
        bools(base + ":tangentBrokens", c.tangentBrokens);
        return c;
    }
};

/// Bakes float channels `bases` (1-3 of them) into a Channel; false when none has a valid curve.
bool bakeChannels(const Reader& r, std::initializer_list<const char*> bases, std::initializer_list<float> defaults, Channel& out) {
    std::vector<std::vector<float>> baked;
    std::vector<bool> present;
    std::vector<float> defs(defaults);
    std::size_t i = 0;
    for (const char* base : bases) {
        std::vector<float> ch;
        present.push_back(bakeFloatCurve(r.curve(base), ch, kCurveResolution, defs[i]));
        baked.push_back(std::move(ch));
        ++i;
    }
    std::vector<const std::vector<float>*> ptrs;
    for (const auto& b : baked) {
        ptrs.push_back(&b);
    }
    Channel tmp;
    if (!combineChannels(ptrs, present, defs, tmp)) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

template <typename E>
E enumFromToken(const std::string& t, std::initializer_list<std::pair<const char*, E>> table, E fallback) {
    for (const auto& [name, value] : table) {
        if (t == name) {
            return value;
        }
    }
    return fallback;
}

} // namespace

ParticleSystemDesc descFromPrimvars(const json::Value& primvars, std::vector<std::string>* issues) {
    ParticleSystemDesc d = schemaDefaultDesc();
    const Reader r{primvars, issues};
    GpuSystemDesc& g = d.gpu;

    // Animated channels (curves), else the legacy spawn -> target pairs.
    ColorGradient minGrad, maxGrad;
    r.floats("minColor:times", minGrad.times);
    r.vec4s("minColor:values", minGrad.values);
    r.floats("maxColor:times", maxGrad.times);
    r.vec4s("maxColor:values", maxGrad.values);
    std::vector<std::array<float, 4>> baked;
    const bool newMinColor = bakeColorGradient(minGrad, baked);
    if (newMinColor) {
        d.minColor = baked;
    }
    const bool newMaxColor = bakeColorGradient(maxGrad, baked);
    if (newMaxColor) {
        d.maxColor = baked;
    }
    Channel ch;
    const bool newMinSize = bakeChannels(r, {"minSize:x", "minSize:y"}, {10.f, 10.f}, ch);
    if (newMinSize) {
        d.minSize = ch;
    }
    const bool newMaxSize = bakeChannels(r, {"maxSize:x", "maxSize:y"}, {10.f, 10.f}, ch);
    if (newMaxSize) {
        d.maxSize = ch;
    }
    const bool newMinRot = bakeChannels(r, {"minRotationSpeed"}, {0.f}, ch);
    if (newMinRot) {
        d.minRotationSpeed = ch;
    }
    const bool newMaxRot = bakeChannels(r, {"maxRotationSpeed"}, {0.f}, ch);
    if (newMaxRot) {
        d.maxRotationSpeed = ch;
    }
    const bool newMaxVel = bakeChannels(r, {"maxVelocity:x", "maxVelocity:y", "maxVelocity:z"}, {-1.f, -1.f, -1.f}, ch);
    if (newMaxVel) {
        d.maxVelocity = ch;
    }

    float minSpawnColor[4] = {1.f, 1.f, 1.f, 1.f}, maxSpawnColor[4] = {1.f, 1.f, 1.f, 1.f};
    float minTargetColor[4] = {1.f, 1.f, 1.f, 0.f}, maxTargetColor[4] = {1.f, 1.f, 1.f, 0.f};
    float minSpawnSize[2] = {10.f, 10.f}, maxSpawnSize[2] = {10.f, 10.f}, minTargetSize[2] = {0.f, 0.f}, maxTargetSize[2] = {0.f, 0.f};
    float minSpawnRot = 0.f, maxSpawnRot = 0.f, minTargetRot = 0.f, maxTargetRot = 0.f, maxSpeed = 0.f;
    r.tuple("minSpawnColor", 4, minSpawnColor);
    r.tuple("maxSpawnColor", 4, maxSpawnColor);
    r.tuple("minTargetColor", 4, minTargetColor);
    r.tuple("maxTargetColor", 4, maxTargetColor);
    r.tuple("minSpawnSize", 2, minSpawnSize);
    r.tuple("maxSpawnSize", 2, maxSpawnSize);
    r.tuple("minTargetSize", 2, minTargetSize);
    r.tuple("maxTargetSize", 2, maxTargetSize);
    r.number("minSpawnRotationSpeed", minSpawnRot);
    r.number("maxSpawnRotationSpeed", maxSpawnRot);
    r.number("minTargetRotationSpeed", minTargetRot);
    r.number("maxTargetRotationSpeed", maxTargetRot);
    r.number("maxSpeed", maxSpeed); // deprecated
    const auto c4 = [](const float* v) { return std::array<float, 4>{v[0], v[1], v[2], v[3]}; };
    if (!newMinColor) {
        d.minColor = {c4(minSpawnColor), c4(minTargetColor)};
    }
    if (!newMaxColor) {
        d.maxColor = {c4(maxSpawnColor), c4(maxTargetColor)};
    }
    if (!newMinSize) {
        d.minSize = {{minSpawnSize[0], minSpawnSize[1], 0.f, 0.f}, {minTargetSize[0], minTargetSize[1], 0.f, 0.f}};
    }
    if (!newMaxSize) {
        d.maxSize = {{maxSpawnSize[0], maxSpawnSize[1], 0.f, 0.f}, {maxTargetSize[0], maxTargetSize[1], 0.f, 0.f}};
    }
    if (!newMinRot) {
        d.minRotationSpeed = {{minSpawnRot, 0.f, 0.f, 0.f}, {minTargetRot, 0.f, 0.f, 0.f}};
    }
    if (!newMaxRot) {
        d.maxRotationSpeed = {{maxSpawnRot, 0.f, 0.f, 0.f}, {maxTargetRot, 0.f, 0.f, 0.f}};
    }
    if (!newMaxVel) {
        d.maxVelocity = {{maxSpeed, maxSpeed, maxSpeed, 0.f}, {maxSpeed, maxSpeed, maxSpeed, 0.f}};
    }

    r.tuple("attractorPosition", 3, g.attractorPosition);
    r.number("attractorForce", g.attractorForce);
    r.number("minTimeToLive", g.minTimeToLive);
    r.number("maxTimeToLive", g.maxTimeToLive);
    r.number("initialVelocityFromNormal", g.initialVelocityFromNormal);
    r.number("initialVelocityConeAngleDegrees", g.initialVelocityConeAngleDegrees);
    r.number("turbulenceFrequency", g.turbulenceFrequency);
    r.number("turbulenceForce", g.turbulenceForce);
    r.number("motionTrailMultiplier", g.motionTrailMultiplier);
    r.number("spawnRatePerSecond", g.spawnRatePerSecond);
    r.number("collisionThickness", g.collisionThickness);
    r.number("collisionRestitution", g.collisionRestitution);
    r.number("initialRotationDeviationDegrees", g.initialRotationDeviationDegrees);
    r.number("spawnBurstDuration", g.spawnBurstDuration);
    r.number("dragCoefficient", g.dragCoefficient);
    r.number("attractorRadius", g.attractorRadius);
    r.number("gravityForce", g.gravityForce);
    r.number("initialVelocityFromMotion", g.initialVelocityFromMotion);
    r.integer("maxNumParticles", g.maxNumParticles);
    g.minSpawnRotationSpeed = minSpawnRot;
    std::string t;
    if (r.token("billboardType", t)) {
        g.billboardType = static_cast<u32>(enumFromToken<BillboardType>(
            t, {{"FaceCamera_UpAxisLocked", BillboardType::FaceCameraUpAxisLocked}, {"FaceCamera_Position", BillboardType::FaceCameraPosition},
                {"FaceWorldUp", BillboardType::FaceWorldUp}},
            BillboardType::FaceCameraSpherical));
    }
    if (r.token("spriteSheetMode", t)) {
        g.spriteSheetMode = static_cast<u32>(enumFromToken<SpriteSheetMode>(
            t, {{"OverrideMaterial_Lifetime", SpriteSheetMode::OverrideMaterialLifetime}, {"OverrideMaterial_Random", SpriteSheetMode::OverrideMaterialRandom}},
            SpriteSheetMode::UseMaterialSpriteSheet));
    }
    if (r.token("collisionMode", t)) {
        g.collisionMode = static_cast<u32>(
            enumFromToken<CollisionMode>(t, {{"Stop", CollisionMode::Stop}, {"Kill", CollisionMode::Kill}}, CollisionMode::Bounce));
    }
    if (r.token("randomFlipAxis", t)) {
        g.randomFlipAxis = static_cast<u32>(enumFromToken<RandomFlipAxis>(
            t, {{"Horizontal", RandomFlipAxis::Horizontal}, {"Vertical", RandomFlipAxis::Vertical}, {"Both", RandomFlipAxis::Both}},
            RandomFlipAxis::None));
    }
    const std::pair<const char*, u32> flags[] = {
        {"hideEmitter", kFlagHideEmitter},
        {"enableMotionTrail", kFlagEnableMotionTrail},
        {"useTurbulence", kFlagUseTurbulence},
        {"alignParticlesToVelocity", kFlagAlignParticlesToVelocity},
        {"useSpawnTexcoords", kFlagUseSpawnTexcoords},
        {"enableCollisionDetection", kFlagEnableCollisionDetection},
        {"restrictVelocityX", kFlagRestrictVelocityX},
        {"restrictVelocityY", kFlagRestrictVelocityY},
        {"restrictVelocityZ", kFlagRestrictVelocityZ},
    };
    for (const auto& [name, bit] : flags) {
        bool b = false;
        if (r.boolean(name, b)) {
            g.flags = b ? (g.flags | bit) : (g.flags & ~bit);
        }
    }
    return d;
}

ParticleSystemDesc descFromUsd(const mods::usd::ParticleSystem& ps, std::vector<std::string>* issues) {
    json::Value pv = json::Value::object();
    for (const auto& [name, value] : ps.primvars) {
        const auto parsed = json::parse(mods::usd::valueToJson(value));
        pv[name] = parsed ? *parsed : json::Value();
    }
    return descFromPrimvars(pv, issues);
}

std::optional<ParticleSystemDesc> descFromRecord(const json::Value& record, std::vector<std::string>* issues) {
    if (record.str("kind") != "relight_particles") {
        return std::nullopt;
    }
    const json::Value* payload = record.get("payload");
    const json::Value* pv = payload != nullptr ? payload->get("primvars") : nullptr;
    if (pv == nullptr || !pv->isObject()) {
        return std::nullopt;
    }
    return descFromPrimvars(*pv, issues);
}

// ---- mesh records -------------------------------------------------------------------------------------------------

namespace {

std::optional<std::string> readBlob(const json::Value& ref, const replace::StoreReader& read) {
    const std::string sha = ref.str("sha256");
    if (sha.size() < 3) {
        return std::nullopt;
    }
    auto bytes = read("blobs/sha256/" + sha.substr(0, 2) + "/" + sha);
    if (!bytes || bytes->size() != static_cast<std::size_t>(ref.num("size", -1.0))) {
        return std::nullopt;
    }
    return bytes;
}

template <typename T>
bool decode(const std::string& bytes, std::size_t components, std::vector<T>& out) {
    if (bytes.size() % (sizeof(T) * components) != 0) {
        return false;
    }
    out.resize(bytes.size() / sizeof(T));
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

} // namespace

std::optional<EmitterMeshData> meshFromRecord(const json::Value& record, const replace::StoreReader& read) {
    const json::Value* payload = record.get("payload");
    if (payload == nullptr || record.str("kind") != "mesh") {
        return std::nullopt;
    }
    EmitterMeshData m;
    const json::Value* streams = payload->get("streams");
    if (streams == nullptr || !streams->isArray()) {
        return std::nullopt;
    }
    for (const json::Value& s : streams->a) {
        const std::string semantic = s.str("semantic");
        const std::string format = s.str("format");
        const json::Value* data = s.get("data");
        if (data == nullptr) {
            continue;
        }
        if (semantic == "Position" && format == "F32x3") {
            const auto b = readBlob(*data, read);
            if (!b || !decode(*b, 3, m.positions)) {
                return std::nullopt;
            }
        } else if (semantic == "Color0" && format == "Unorm8x4") {
            const auto b = readBlob(*data, read);
            if (b) {
                decode(*b, 1, m.colors);
            }
        } else if (semantic == "Uv0" && format == "F32x2") {
            const auto b = readBlob(*data, read);
            if (b) {
                decode(*b, 2, m.texcoords);
            }
        }
    }
    const json::Value* idx = payload->get("indices32");
    if (idx == nullptr) {
        return std::nullopt;
    }
    const auto ib = readBlob(*idx, read);
    if (!ib || !decode(*ib, 1, m.indices) || m.positions.empty()) {
        return std::nullopt;
    }
    return m;
}

// ---- catalog ------------------------------------------------------------------------------------------------------

struct ParticleCatalog::ModReader {
    replace::StoreReader read;
    bool ok = false;
    std::shared_ptr<mods::import::ImportResult> imported;
};

ParticleCatalog::ParticleCatalog(const replace::ReplacementEngine& engine) : m_engine(engine) {}
ParticleCatalog::~ParticleCatalog() = default;

void ParticleCatalog::sync() {
    if (m_engine.generation() == m_generation) {
        return;
    }
    m_generation = m_engine.generation();
    m_readers.clear();
    m_descs.clear();
    m_meshes.clear();
    m_diagnostics.clear();
}

ParticleCatalog::ModReader* ParticleCatalog::reader(const std::string& modName) {
    const auto it = m_readers.find(modName);
    if (it != m_readers.end()) {
        return it->second.get();
    }
    auto r = std::make_unique<ModReader>();
    for (const replace::ModContent* mod : m_engine.index().stack()) {
        if (mod->location.name != modName) {
            continue;
        }
        if (mod->location.format == replace::ModFormat::Store) {
            const std::string dir = mod->location.dir;
            r->read = [dir](const std::string& rel) -> std::optional<std::string> {
                std::ifstream f(dir + "/" + rel, std::ios::binary);
                if (!f) {
                    return std::nullopt;
                }
                std::ostringstream s;
                s << f.rdbuf();
                return s.str();
            };
            r->ok = true;
        } else {
            mods::import::ImportOptions o;
            o.root = mod->location.dir;
            o.gameId = m_engine.config().gameId;
            auto result = std::make_shared<mods::import::ImportResult>(mods::import::importMod(o));
            r->ok = result->ok;
            r->imported = result;
            r->read = [result](const std::string& rel) -> std::optional<std::string> {
                const auto f = result->files.find(rel);
                if (f == result->files.end()) {
                    return std::nullopt;
                }
                return std::string(f->second.begin(), f->second.end());
            };
        }
        break;
    }
    if (!r->ok) {
        m_diagnostics.push_back(modName + ": mod not readable for particles");
    }
    ModReader* out = r.get();
    m_readers.emplace(modName, std::move(r));
    return out;
}

namespace {
std::uint64_t pairKey(const std::string& a, const std::string& b) {
    const std::uint64_t h = hash::xxh64(a.data(), a.size(), 0x70617274u);
    return hash::xxh64(b.data(), b.size(), h);
}
} // namespace

const ParticleSystemDesc* ParticleCatalog::particles(const std::string& modName, const std::string& recordId) {
    if (modName.empty() || recordId.empty()) {
        return nullptr;
    }
    const std::uint64_t key = pairKey(modName, recordId);
    auto it = m_descs.find(key);
    if (it == m_descs.end()) {
        DescEntry e;
        ModReader* r = reader(modName);
        if (r != nullptr && r->ok) {
            if (const auto text = r->read("poco/relight_particles/" + recordId + ".poco.json")) {
                if (const auto rec = json::parse(*text)) {
                    std::vector<std::string> issues;
                    e.desc = descFromRecord(*rec, &issues);
                    for (const std::string& i : issues) {
                        m_diagnostics.push_back(modName + ": " + recordId + ": " + i);
                    }
                    if (e.desc) {
                        e.hash = hashDesc(*e.desc);
                    }
                }
            }
        }
        it = m_descs.emplace(key, std::move(e)).first;
    }
    return it->second.desc ? &*it->second.desc : nullptr;
}

std::uint64_t ParticleCatalog::particlesHash(const std::string& modName, const std::string& recordId) {
    if (particles(modName, recordId) == nullptr) {
        return 0;
    }
    return m_descs[pairKey(modName, recordId)].hash;
}

const EmitterMeshData* ParticleCatalog::mesh(const std::string& modName, const std::string& meshId) {
    if (modName.empty() || meshId.empty()) {
        return nullptr;
    }
    const std::uint64_t key = pairKey(modName, meshId);
    auto it = m_meshes.find(key);
    if (it == m_meshes.end()) {
        std::optional<EmitterMeshData> m;
        ModReader* r = reader(modName);
        if (r != nullptr && r->ok) {
            if (const auto text = r->read("poco/mesh/" + meshId + ".poco.json")) {
                if (const auto rec = json::parse(*text)) {
                    m = meshFromRecord(*rec, r->read);
                }
            }
        }
        if (!m) {
            m_diagnostics.push_back(modName + ": " + meshId + ": emitter mesh not readable");
        }
        it = m_meshes.emplace(key, std::move(m)).first;
    }
    return it->second ? &*it->second : nullptr;
}

// ---- emitter bridge -----------------------------------------------------------------------------------------------

ParticleEmitterBridge::ParticleEmitterBridge(ParticleSystemManager& manager, ParticleCatalog& catalog)
    : m_manager(manager), m_catalog(catalog) {
    m_motion.reserve(256);
    m_partMeshes.reserve(256);
}

void ParticleEmitterBridge::beginFrame(std::uint64_t frame) {
    m_catalog.sync();
    m_frame = frame;
    m_stats = EmitterFrameStats{};
    m_preset = globalPresetDesc();
    m_presetHash = hashDesc(m_preset);
}

EmitterMeshId ParticleEmitterBridge::partMesh(const std::string& mod, const std::string& meshId) {
    const std::uint64_t key = pairKey(mod, meshId);
    const auto it = m_partMeshes.find(key);
    if (it != m_partMeshes.end()) {
        return it->second;
    }
    EmitterMeshId id = kInvalidMesh;
    if (const EmitterMeshData* data = m_catalog.mesh(mod, meshId)) {
        id = m_manager.registerMesh(data->view());
    }
    m_partMeshes.emplace(key, id);
    return id;
}

Mat34 ParticleEmitterBridge::prevTransform(std::uint64_t instance, std::uint32_t part, const Mat34& current) {
    const std::uint64_t key = instance * 0x9E3779B97F4A7C15ull + part;
    auto it = m_motion.find(key);
    if (it == m_motion.end()) {
        Motion m;
        m.last = current;
        m.frame = m_frame;
        m_motion.emplace(key, m);
        return current;
    }
    Motion& m = it->second;
    const bool continuous = m.frame + 1 == m_frame;
    const Mat34 prev = continuous ? m.last : current;
    if (ParticleOptions::enableDiscontinuityGuard() && continuous) {
        // upstream resolveSpawnPrevTransform: a jump away from the recent average collapses prev to current.
        const float gap[3] = {current[3] - prev[3], current[7] - prev[7], current[11] - prev[11]};
        const float res[3] = {gap[0] - m.average[0], gap[1] - m.average[1], gap[2] - m.average[2]};
        const float residual = std::sqrt(res[0] * res[0] + res[1] * res[1] + res[2] * res[2]);
        const float avgLen = std::sqrt(m.average[0] * m.average[0] + m.average[1] * m.average[1] + m.average[2] * m.average[2]);
        const float threshold = ParticleOptions::discontinuityFactor() * (avgLen + ParticleOptions::discontinuityFloor() * ParticleOptions::sceneScale());
        m.discontinuity = residual > threshold;
        const float gapLen = std::sqrt(gap[0] * gap[0] + gap[1] * gap[1] + gap[2] * gap[2]);
        const float fold = gapLen > threshold && gapLen > 0.f ? threshold / gapLen : 1.f;
        for (int i = 0; i < 3; ++i) {
            m.average[i] = m.average[i] * 0.8f + gap[i] * fold * 0.2f;
        }
    } else if (!continuous) {
        m.average[0] = m.average[1] = m.average[2] = 0.f;
        m.discontinuity = false;
    }
    m.last = current;
    m.frame = m_frame;
    return m.discontinuity ? current : prev;
}

bool ParticleEmitterBridge::request(const Choice& c, std::uint64_t materialKey, EmitterMeshId mesh, const Mat34& world, const Mat34& prev) {
    SpawnRequest req;
    req.desc = c.desc;
    req.descHash = c.hash;
    req.materialKey = materialKey;
    req.mesh = mesh;
    req.objectToWorld = world;
    req.prevObjectToWorld = prev;
    const SpawnResult r = m_manager.spawn(req);
    if (r.system < 0) {
        return false;
    }
    ++m_stats.emitters;
    m_stats.particles += r.particles;
    m_stats.meshSystems += c.source == 1 ? 1u : 0u;
    m_stats.materialSystems += c.source == 2 ? 1u : 0u;
    m_stats.presetSystems += c.source == 3 ? 1u : 0u;
    return true;
}

EmitterResult ParticleEmitterBridge::processDraw(const replace::DrawInput& input, const replace::ReplacedDraw& draw, EmitterMeshId originalMesh) {
    EmitterResult result;
    const bool emitterCategory = draw.categories.test(scene::InstanceCategories::ParticleEmitter);
    Choice meshChoice;
    if (draw.meshReplaced) {
        if (const ParticleSystemDesc* d = m_catalog.particles(draw.meshMod, draw.meshRecord)) {
            meshChoice = Choice{d, m_catalog.particlesHash(draw.meshMod, draw.meshRecord), 1};
        }
    }
    const Choice preset = emitterCategory ? Choice{&m_preset, m_presetHash, 3} : Choice{};

    // Replacement parts.
    for (std::size_t k = 0; k < draw.parts.size(); ++k) {
        const replace::ReplacedPart& part = draw.parts[k];
        Choice c = meshChoice;
        if (c.desc == nullptr && !part.material.empty()) {
            const std::string& mod = part.materialMod.empty() ? draw.meshMod : part.materialMod;
            if (const ParticleSystemDesc* d = m_catalog.particles(mod, part.material)) {
                c = Choice{d, m_catalog.particlesHash(mod, part.material), 2};
            }
        }
        if (c.desc == nullptr) {
            c = preset;
        }
        if (c.desc == nullptr) {
            continue;
        }
        const EmitterMeshId mesh = partMesh(draw.meshMod, part.meshId);
        if (mesh == kInvalidMesh) {
            continue;
        }
        const Mat34 world = fromRowVector44(part.objectToWorld);
        const Mat34 prev = prevTransform(draw.instanceId, static_cast<std::uint32_t>(k + 1), world);
        const std::string& matId = part.material;
        const std::uint64_t matKey = matId.empty() ? input.materialHash : hash::xxh64(matId.data(), matId.size(), 0);
        if (request(c, matKey, mesh, world, prev)) {
            ++result.requests;
            if (c.desc->hideEmitter() && k < 64) {
                result.hiddenParts |= 1ull << k;
                ++m_stats.hiddenEmitters;
            }
        }
    }

    // The original draw.
    if (draw.drawOriginal || (draw.meshReplaced && draw.parts.empty())) {
        Choice c;
        if (draw.meshReplaced && draw.parts.empty()) {
            c = meshChoice;
        }
        if (c.desc == nullptr && draw.materialReplaced) {
            if (const ParticleSystemDesc* d = m_catalog.particles(draw.materialMod, draw.materialRecord)) {
                c = Choice{d, m_catalog.particlesHash(draw.materialMod, draw.materialRecord), 2};
            }
        }
        if (c.desc == nullptr) {
            c = preset;
        }
        if (c.desc != nullptr && originalMesh != kInvalidMesh) {
            const Mat34 world = fromRowVector44(input.objectToWorld);
            const Mat34 prev = prevTransform(draw.instanceId, 0u, world);
            if (request(c, input.materialHash, originalMesh, world, prev)) {
                ++result.requests;
                if (c.desc->hideEmitter()) {
                    result.hideOriginal = true;
                    ++m_stats.hiddenEmitters;
                }
            }
        }
    }
    return result;
}

} // namespace fuse::relight::particles
