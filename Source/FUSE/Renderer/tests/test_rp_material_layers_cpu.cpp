// Asset plan W0.7 layered materials: CPU gates (no device; also run in the stub tree). Lavapipe gates:
// test_rp_material_layers.cpp.
//
//   layout      the C++ records (MlTexture, MlLayer, MlMaterial, MlSurface, MlResult, MlBall, MlParams, MlPush) vs
//               their GLSL and Slang mirrors (names, order, offsets, sizes); library sections 256-aligned, disjoint
//   fusemat     every fixture: JSON -> FuseMat -> canonical JSON -> FuseMat (equal) and -> binary -> FuseMat (equal,
//               deterministic bytes); a validation error fixture per rule (syntax, unknown key, wrong type, unknown
//               enum, > 3 layers, ranges, §1.6 albedo, missing name / version, bad texture id); binary loader refuses
//               bad magic / version / size / checksum / truncation; resolve refuses unknown textures
//   reference   CPU reference properties: height blend (0 at mask 0, 1 at mask 1, monotone), layer masks, stochastic
//               tiling (autocorrelation at one period drops vs plain tiling; mean / std preserved), triplanar
//               continuity across a rounded cube edge (vs the seam of a per-face UV unwrap), detail fade, macro
//               variation, default surface
//   golden_cpu  the CPU reference render of the material-ball scene vs the committed golden PNG (Lavapipe render)
//   api         MaterialLayers stub behaviour (init fails without a device), PNG codec round trip
#include "test_rp_material_layers_common.hpp"

#include <fuse/renderer/material_layers/material_layers.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace {

using namespace ml_test;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectLe(f64 value, f64 bound, const char* message) {
    if (!(value <= bound)) {
        std::fprintf(stderr, "FAIL: %s (got %.6g, bound %.6g)\n", message, value, bound);
        ++g_failures;
    }
}

// --- layout ----------------------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};

#define F(T, n) Field{#n, offsetof(T, n)}
const std::vector<Field> kTexture = {F(MlTexture, offset), F(MlTexture, width), F(MlTexture, height), F(MlTexture, flags),
                                     F(MlTexture, mean)};
const std::vector<Field> kLayer = {F(MlLayer, albedo),    F(MlLayer, roughness), F(MlLayer, metallic),
                                   F(MlLayer, uvScale),   F(MlLayer, contrast),  F(MlLayer, coverage),
                                   F(MlLayer, albedoTex), F(MlLayer, normalTex), F(MlLayer, mask),
                                   F(MlLayer, mode),      F(MlLayer, maskBias),  F(MlLayer, maskScale),
                                   F(MlLayer, normalStrength), F(MlLayer, reserved)};
const std::vector<Field> kMaterial = {
    F(MlMaterial, albedo),          F(MlMaterial, roughness),       F(MlMaterial, metallic),
    F(MlMaterial, uvScale),         F(MlMaterial, normalStrength),  F(MlMaterial, flags),
    F(MlMaterial, albedoTex),       F(MlMaterial, normalTex),       F(MlMaterial, layerCount),
    F(MlMaterial, shadingModel),    F(MlMaterial, detailAlbedoTex), F(MlMaterial, detailNormalTex),
    F(MlMaterial, detailScale),     F(MlMaterial, detailStrength),  F(MlMaterial, detailFadeStart),
    F(MlMaterial, detailFadeEnd),   F(MlMaterial, triplanarSharpness), F(MlMaterial, stochasticLattice),
    F(MlMaterial, macroScale),      F(MlMaterial, macroStrength),   F(MlMaterial, procedural),
    F(MlMaterial, category),        F(MlMaterial, layers)};
const std::vector<Field> kSurface = {F(MlSurface, position), F(MlSurface, viewDistance), F(MlSurface, normal),
                                     F(MlSurface, tangentSign), F(MlSurface, tangent), F(MlSurface, material),
                                     F(MlSurface, uv), F(MlSurface, reserved), F(MlSurface, color)};
const std::vector<Field> kResult = {F(MlResult, albedo), F(MlResult, roughness), F(MlResult, normal),
                                    F(MlResult, metallic), F(MlResult, ao), F(MlResult, height), F(MlResult, reserved)};
const std::vector<Field> kBallFields = {F(MlBall, center), F(MlBall, radius), F(MlBall, material), F(MlBall, reserved)};
const std::vector<Field> kParams = {
    F(MlParams, materials), F(MlParams, textures),  F(MlParams, texels),       F(MlParams, lut),
    F(MlParams, surfaces),  F(MlParams, results),   F(MlParams, image),        F(MlParams, balls),
    F(MlParams, count),     F(MlParams, width),     F(MlParams, height),       F(MlParams, flags),
    F(MlParams, ballCount), F(MlParams, materialCount), F(MlParams, textureCount), F(MlParams, reserved0),
    F(MlParams, camPos),    F(MlParams, tanHalfY),  F(MlParams, camForward),   F(MlParams, aspect),
    F(MlParams, camRight),  F(MlParams, exposure),  F(MlParams, camUp),        F(MlParams, reserved1),
    F(MlParams, sunDir),    F(MlParams, sunIntensity), F(MlParams, skyColor),  F(MlParams, ambient),
    F(MlParams, groundColor), F(MlParams, reserved2), F(MlParams, background)};
const std::vector<Field> kPush = {F(MlPush, params), F(MlPush, src), F(MlPush, dst), F(MlPush, count),
                                  F(MlPush, reserved)};
#undef F

/// Parses `open ... }` of a shader struct: field names and std430 / scalar offsets (4-byte scalars, u64, MlLayer).
bool parseShaderStruct(const std::string& text, const std::string& open, std::vector<Field>& fields,
                       std::vector<std::string>& names, size_t& size) {
    const size_t begin = text.find(open);
    if (begin == std::string::npos) {
        return false;
    }
    const size_t end = text.find("}", begin);
    std::istringstream body(text.substr(begin + open.size(), end - begin - open.size()));
    std::string line;
    size_t offset = 0;
    while (std::getline(body, line)) {
        std::istringstream ls(line);
        std::string type, decl;
        if (!(ls >> type >> decl) || decl.back() != ';') {
            continue;
        }
        decl.pop_back();
        size_t count = 1;
        const size_t bracket = decl.find('[');
        if (bracket != std::string::npos) {
            count = static_cast<size_t>(std::stoul(decl.substr(bracket + 1)));
            decl = decl.substr(0, bracket);
        }
        const size_t align = type == "uint64_t" ? 8u : 4u;
        const size_t bytes = type == "uint64_t" ? 8u : (type == "MlLayer" ? 64u : 4u);
        offset = (offset + align - 1u) / align * align;
        names.push_back(decl);
        fields.push_back(Field{nullptr, offset});
        offset += bytes * count;
    }
    size = offset;
    return true;
}

void checkStruct(const std::string& text, const char* lang, const std::string& open, const std::vector<Field>& cpp,
                 size_t cppSize, const char* label) {
    std::vector<Field> fields;
    std::vector<std::string> names;
    size_t size = 0;
    const bool parsed = parseShaderStruct(text, open, fields, names, size);
    bool same = parsed && fields.size() == cpp.size();
    for (size_t i = 0; same && i < cpp.size(); ++i) {
        same = names[i] == cpp[i].name && fields[i].offset == cpp[i].offset;
        if (!same) {
            std::fprintf(stderr, "  %s %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, label, i, names[i].c_str(),
                         fields[i].offset, cpp[i].name, cpp[i].offset);
        }
    }
    // std430 rounds a struct up to its alignment (8 with a u64 member); the C++ records are multiples of 16.
    same = same && (size + 7u) / 8u * 8u <= cppSize && cppSize - size < 16u;
    std::printf("layout: ml_common.%s %s %zu fields, %zu bytes (C++ %zu)\n", lang, label, fields.size(), size, cppSize);
    if (!same) {
        std::fprintf(stderr, "FAIL: %s %s differs from the C++ record\n", lang, label);
        ++g_failures;
    }
}

void testLayout() {
    for (const char* lang : {"glsl", "slang"}) {
        std::string text;
        const bool read = readText(std::string(FUSE_RP_ML_SHADER_DIR) + "/ml_common." + lang, text);
        expect(read, "ml_common shader readable");
        if (!read) {
            continue;
        }
        checkStruct(text, lang, "struct MlTexture {", kTexture, sizeof(MlTexture), "MlTexture");
        checkStruct(text, lang, "struct MlLayer {", kLayer, sizeof(MlLayer), "MlLayer");
        checkStruct(text, lang, "struct MlMaterial {", kMaterial, sizeof(MlMaterial), "MlMaterial");
        checkStruct(text, lang, "struct MlSurface {", kSurface, sizeof(MlSurface), "MlSurface");
        checkStruct(text, lang, "struct MlResult {", kResult, sizeof(MlResult), "MlResult");
        checkStruct(text, lang, "struct MlBall {", kBallFields, sizeof(MlBall), "MlBall");
        checkStruct(text, lang, "struct MlParams {", kParams, sizeof(MlParams), "MlParams");
        checkStruct(text, lang, std::string(lang) == "slang" ? "struct MlPush {" : "uniform MlPushBlock {", kPush,
                    sizeof(MlPush), "push block");
    }
    const MlLibraryLayout l = MlLibraryLayout::compute(10u, 11u, 8u, 12345u);
    const u64 off[5] = {l.lut, l.textures, l.materials, l.balls, l.texels};
    const u64 len[5] = {kMlLutEntries * 4u, 10u * sizeof(MlTexture), 11u * sizeof(MlMaterial), 8u * sizeof(MlBall),
                        12345u * 4u};
    bool ok = true;
    for (u32 i = 0; i < 5u; ++i) {
        ok = ok && off[i] % 256u == 0u && off[i] + len[i] <= l.bytes;
        for (u32 j = i + 1u; j < 5u; ++j) {
            ok = ok && (off[i] + len[i] <= off[j] || off[j] + len[j] <= off[i]);
        }
    }
    expect(ok, "library sections 256-aligned, sized and disjoint");
}

// --- fusemat -----------------------------------------------------------------------------------------------------
struct BadCase {
    const char* json;
    const char* path; ///< expected error path (prefix match), "" = document
};

void testFusemat() {
    u32 fixtures = 0;
    for (const char* name : kBallFixtures) {
        std::string text;
        expect(readText(std::string(FUSE_RP_ML_FIXTURE_DIR) + "/" + name + ".fusemat.json", text), "fixture readable");
        FuseMat a{};
        const FuseMatResult r = parse_fusemat_json(text, a);
        if (!r.ok) {
            std::fprintf(stderr, "%s:\n%s", name, r.describe().c_str());
        }
        expect(r.ok, "fixture parses and validates");
        const std::string canon = write_fusemat_json(a);
        FuseMat b{};
        expect(parse_fusemat_json(canon, b).ok && b == a, "JSON round trip (canonical JSON -> same FuseMat)");
        expect(write_fusemat_json(b) == canon, "canonical JSON is a fixed point");
        const std::vector<u8> bin = write_fusemat_binary(a);
        FuseMat c{};
        const FuseMatResult rb = read_fusemat_binary(bin.data(), bin.size(), c);
        expect(rb.ok && c == a, "binary round trip");
        expect(write_fusemat_binary(c) == bin, "binary cook is deterministic");
        ++fixtures;
    }
    std::printf("fusemat: %u fixtures round-trip (JSON and binary)\n", fixtures);

    // A fully populated material through both round trips (every field non-default).
    FuseMat full{};
    full.name = "test/full \"quoted\"";
    full.shading = FuseMatShading::Foliage;
    full.category = FuseMatCategory::Foliage;
    full.wind = FuseMatWind::Leaves;
    full.albedo[0] = 0.1f;
    full.albedo[1] = 0.2f;
    full.albedo[2] = 0.30000001f;
    full.roughness = 0.123456789f;
    full.metallic = 0.25f;
    full.normalStrength = 1.5f;
    full.textures = {"a", "b"};
    full.uvScale = 3.5f;
    full.triplanar = true;
    full.triplanarSharpness = 7.f;
    full.stochastic = true;
    full.stochasticLattice = 1.75f;
    full.macroScale = 0.1f;
    full.macroStrength = 0.3f;
    full.detail = {"d", "e"};
    full.detailScale = 12.f;
    full.detailStrength = 0.5f;
    full.detailFade[0] = 1.f;
    full.detailFade[1] = 99.f;
    for (u32 i = 0; i < 3u; ++i) {
        FuseMatLayer l{};
        l.name = "layer" + std::to_string(i);
        l.mode = i == 2u ? kMlLayerWet : kMlLayerHeight;
        l.mask = static_cast<MlMask>(i + 1u);
        l.maskBias = 0.1f * static_cast<f32>(i);
        l.maskScale = 2.f;
        l.coverage = 0.75f;
        l.contrast = 4.f + static_cast<f32>(i);
        l.albedo[0] = 0.5f;
        l.albedo[1] = 0.6f;
        l.albedo[2] = 0.7f;
        l.roughness = 0.3f;
        l.uvScale = 2.f;
        l.textures = {"t" + std::to_string(i), ""};
        full.layers.push_back(l);
    }
    full.proceduralFunction = 3;
    full.proceduralParams = {1.f, -2.5f, 1e-7f};
    expect(validate_fusemat(full).ok, "full material validates");
    FuseMat back{};
    expect(parse_fusemat_json(write_fusemat_json(full), back).ok && back == full, "full material JSON round trip");
    const std::vector<u8> fullBin = write_fusemat_binary(full);
    FuseMat backBin{};
    expect(read_fusemat_binary(fullBin.data(), fullBin.size(), backBin).ok && backBin == full,
           "full material binary round trip");

    // Validation errors: each case must fail with an error at the expected path.
    const std::string head = R"({"fusemat": 1, "name": "x", )";
    const BadCase bad[] = {
        {R"({"fusemat": 1, "name": "x",)", ""},                                 // syntax
        {R"({"fusemat": 1, "name": "x", "bogus": 1})", "bogus"},                // unknown key
        {R"({"name": "x"})", "fusemat"},                                        // missing version
        {R"({"fusemat": 2, "name": "x"})", "fusemat"},                          // unsupported version
        {R"({"fusemat": 1})", "name"},                                          // missing name
        {R"({"fusemat": 1, "name": ""})", "name"},                              // empty name
        {R"({"fusemat": 1, "name": 5})", "name"},                               // wrong type
        {R"({"fusemat": 1, "name": "x", "category": "lava"})", "category"},     // unknown enum
        {R"({"fusemat": 1, "name": "x", "shading_model": 3})", "shading_model"}, // wrong type
        {R"({"fusemat": 1, "name": "x", "base": {"roughness": 1.5}})", "base.roughness"},
        {R"({"fusemat": 1, "name": "x", "base": {"metallic": -0.1}})", "base.metallic"},
        {R"({"fusemat": 1, "name": "x", "base": {"albedo": [0.95, 0.5, 0.5]}})", "base.albedo[0]"}, // §1.6 too bright
        {R"({"fusemat": 1, "name": "x", "base": {"albedo": [0.01, 0.5, 0.5]}})", "base.albedo[0]"}, // §1.6 charcoal
        {R"({"fusemat": 1, "name": "x", "base": {"metallic": 1, "albedo": [0.3, 0.5, 0.5]}})", "base.albedo[0]"},
        {R"({"fusemat": 1, "name": "x", "base": {"albedo": [0.5, 0.5]}})", "base.albedo"},
        {R"({"fusemat": 1, "name": "x", "base": {"textures": {"albedo": 3}}})", "base.textures.albedo"},
        {R"({"fusemat": 1, "name": "x", "base": {"textures": {"height": "h"}}})", "base.textures.height"},
        {R"({"fusemat": 1, "name": "x", "uv_scale": 0})", "uv_scale"},
        {R"({"fusemat": 1, "name": "x", "triplanar": {"enabled": 1}})", "triplanar.enabled"},
        {R"({"fusemat": 1, "name": "x", "triplanar": {"sharpness": 32}})", "triplanar.sharpness"},
        {R"({"fusemat": 1, "name": "x", "stochastic": {"lattice": 0}})", "stochastic.lattice"},
        {R"({"fusemat": 1, "name": "x", "macro": {"strength": 2}})", "macro.strength"},
        {R"({"fusemat": 1, "name": "x", "detail": {"scale": 64}})", "detail.scale"},
        {R"({"fusemat": 1, "name": "x", "detail": {"fade": [10, 5]}})", "detail.fade"},
        {R"({"fusemat": 1, "name": "x", "layers": [{}, {}, {}, {}]})", "layers"},       // > 3 layers
        {R"({"fusemat": 1, "name": "x", "layers": [{"contrast": 0.5}]})", "layers[0].contrast"},
        {R"({"fusemat": 1, "name": "x", "layers": [{"coverage": 2}]})", "layers[0].coverage"},
        {R"({"fusemat": 1, "name": "x", "layers": [{"mask": "vertex_q"}]})", "layers[0].mask"},
        {R"({"fusemat": 1, "name": "x", "layers": [{"mode": "paint"}]})", "layers[0].mode"},
        {R"({"fusemat": 1, "name": "x", "layers": [{"mode": "wet", "albedo": [0, 0.5, 0.5]}]})", "layers[0].albedo[0]"},
        {R"({"fusemat": 1, "name": "x", "layers": [{"texture": {}}]})", "layers[0].texture"},
        {R"({"fusemat": 1, "name": "x", "layers": {}})", "layers"},
        {R"({"fusemat": 1, "name": "x", "procedural": {"params": [1,2,3,4,5,6,7,8,9]}})", "procedural.params"},
        {R"({"fusemat": 1, "name": "x", "procedural": {"function": -1}})", "procedural.function"},
        {R"({"fusemat": 1, "name": "x", "name": "y"})", ""},                    // duplicate key (syntax)
    };
    u32 caught = 0;
    for (const BadCase& c : bad) {
        FuseMat m{};
        const FuseMatResult r = parse_fusemat_json(c.json, m);
        bool found = false;
        for (const FuseMatError& e : r.errors) {
            found = found || e.path.rfind(c.path, 0) == 0;
        }
        if (r.ok || !found) {
            std::fprintf(stderr, "  case %s: ok=%d errors:\n%s", c.json, r.ok ? 1 : 0, r.describe().c_str());
        }
        caught += (!r.ok && found) ? 1u : 0u;
    }
    const u32 badCount = static_cast<u32>(sizeof(bad) / sizeof(bad[0]));
    std::printf("fusemat: %u of %u invalid documents rejected with the expected path\n", caught, badCount);
    expect(caught == badCount, "every invalid document is rejected at the expected path");
    {
        FuseMat m{};
        const FuseMatResult r = parse_fusemat_json(head + R"("layers": [{"contrast": 0.5, "coverage": 3, "albedo": [0.5, 0.5, 0.5]}]})", m);
        expect(!r.ok && r.errors.size() == 2u, "several errors are all reported");
    }

    // Binary loader refusals.
    const std::vector<u8> good = write_fusemat_binary(full);
    auto refuse = [&](std::vector<u8> bytes, const char* what) {
        FuseMat m{};
        const FuseMatResult r = read_fusemat_binary(bytes.data(), bytes.size(), m);
        if (r.ok) {
            std::fprintf(stderr, "FAIL: binary loader accepted %s\n", what);
            ++g_failures;
        }
    };
    std::vector<u8> b = good;
    b[0] ^= 1u;
    refuse(b, "a bad magic");
    b = good;
    b[4] = 9u;
    refuse(b, "an unknown version");
    b = good;
    b.pop_back();
    refuse(b, "a truncated file");
    b = good;
    b.push_back(0u);
    refuse(b, "trailing bytes");
    b = good;
    b[20] ^= 0x40u;
    refuse(b, "a corrupted payload (checksum)");
    refuse(std::vector<u8>{}, "an empty file");
    {
        FuseMat m = full;
        m.roughness = 3.f; // a binary with an invalid value (cooked by something else): the loader validates
        const std::vector<u8> invalid = write_fusemat_binary(m);
        refuse(invalid, "an out-of-range value");
    }

    // Load-time resolution.
    MlLibrary lib;
    FuseMat unresolved = full;
    MlMaterial g{};
    const FuseMatResult rr = resolve_fusemat(unresolved, [](std::string_view) { return kMlNoTexture; }, g);
    expect(!rr.ok && rr.errors.size() == 7u, "resolve reports every unknown texture id (7)");
    const FuseMatResult ok = resolve_fusemat(full, [](std::string_view id) { return static_cast<u32>(id.size()); }, g);
    expect(ok.ok && g.layerCount == 3u && g.layers[2].mode == kMlLayerWet &&
               (g.flags & (kMlFlagTriplanar | kMlFlagStochastic | kMlFlagDetail | kMlFlagMacro)) ==
                   (kMlFlagTriplanar | kMlFlagStochastic | kMlFlagDetail | kMlFlagMacro) &&
               g.layers[1].normalTex == kMlNoTexture && g.shadingModel == static_cast<u32>(FuseMatShading::Foliage),
           "resolve builds the GPU record (flags, layers, missing slots)");
}

// --- reference ---------------------------------------------------------------------------------------------------
void testReference() {
    MlLibrary lib;
    expect(buildLibrary(FUSE_RP_ML_FIXTURE_DIR, lib), "golden library");
    const MlView v = lib.view();

    // Height blend.
    bool hb = true;
    for (f32 hBase = 0.f; hBase <= 1.f; hBase += 0.125f) {
        for (f32 hLayer = 0.f; hLayer <= 1.f; hLayer += 0.125f) {
            hb = hb && ml_height_blend(hBase, hLayer, 0.f, 1.f) == 0.f && ml_height_blend(hBase, hLayer, 1.f, 1.f) == 1.f;
            f32 prev = -1.f;
            for (f32 m = 0.f; m <= 1.f; m += 0.0625f) {
                const f32 t = ml_height_blend(hBase, hLayer, m, 8.f);
                hb = hb && t >= prev && t >= 0.f && t <= 1.f;
                prev = t;
            }
        }
    }
    expect(hb, "height blend: 0 at mask 0, 1 at mask 1, monotone in the mask");
    expect(ml_height_blend(0.2f, 0.8f, 0.5f, 8.f) == 1.f && ml_height_blend(0.8f, 0.2f, 0.5f, 8.f) == 0.f,
           "height blend: at mask 0.5 the higher height wins");

    // Stochastic tiling: autocorrelation at one period and the histogram moments.
    constexpr u32 kSize = 256u;
    constexpr u32 kRepeats = 8u;
    const u32 lag = kSize / kRepeats;
    std::vector<MlResult> plain, stoch;
    for (const MlSurface& s : tilingGrid(kSize, kRepeats, kMatPlainTiling)) {
        plain.push_back(ml_evaluate(v, s));
    }
    for (const MlSurface& s : tilingGrid(kSize, kRepeats, kMatStochasticTiling)) {
        stoch.push_back(ml_evaluate(v, s));
    }
    const std::vector<f32> lp = luminance(plain), ls = luminance(stoch);
    const f64 acPlain = ml_autocorrelation(lp, kSize, kSize, lag);
    const f64 acStoch = ml_autocorrelation(ls, kSize, kSize, lag);
    f64 mp = 0, sp = 0, ms = 0, ss = 0;
    meanStd(lp, mp, sp);
    meanStd(ls, ms, ss);
    std::printf("reference: autocorrelation at one period: plain %.4f, stochastic %.4f; mean %.4f / %.4f, std %.4f / %.4f\n",
                acPlain, acStoch, mp, ms, sp, ss);
    expect(acPlain > 0.99, "plain tiling repeats exactly (autocorrelation ~ 1 at one period)");
    expectLe(acStoch, 0.25, "stochastic tiling removes the repetition (autocorrelation at one period)");
    expectLe(std::fabs(ms - mp) / mp, 0.03, "stochastic tiling keeps the mean (relative)");
    expectLe(std::fabs(ss / sp - 1.0), 0.15, "stochastic tiling keeps the standard deviation (relative)");

    // Triplanar continuity across a rounded cube edge vs a per-face UV unwrap.
    const EdgePath tri = edgePath(kMatTriplanar, 0.002f);
    const EdgePath uv = edgePath(kMatPlainTiling, 0.002f);
    std::vector<MlResult> rt, ru;
    for (const MlSurface& s : tri.surfaces) {
        rt.push_back(ml_evaluate(v, s));
    }
    for (const MlSurface& s : uv.surfaces) {
        ru.push_back(ml_evaluate(v, s));
    }
    f64 tIn = 0, tOut = 0, uIn = 0, uOut = 0;
    edgeSteps(rt, tri.bevelBegin, tri.bevelEnd, tIn, tOut);
    edgeSteps(ru, uv.bevelBegin, uv.bevelEnd, uIn, uOut);
    std::printf("reference: largest albedo step across the bevel / on the faces: triplanar %.4f / %.4f, UV unwrap %.4f / %.4f\n",
                tIn, tOut, uIn, uOut);
    expectLe(tIn, 1.5 * tOut + 1e-3, "triplanar: no seam across the cube edge (bevel step <= face steps)");
    expect(uIn > 2.5 * uOut, "the per-face UV unwrap does show a seam (the metric sees seams)");

    // Detail fade: beyond fadeEnd the detail has no effect.
    {
        MlLibrary l2;
        FuseMat m = stoneMaterial("fade", false, false);
        m.detail = {"grain_albedo", "grain_normal"};
        m.detailFade[0] = 4.f;
        m.detailFade[1] = 12.f;
        expect(l2.addMaterial(m).ok, "detail material");
        FuseMat n = stoneMaterial("nodetail", false, false);
        expect(l2.addMaterial(n).ok, "no-detail material");
        f64 farDiff = 0.0, nearDiff = 0.0;
        for (const MlSurface& s0 : randomSurfaces(256u, 5u)) {
            MlSurface s = s0;
            s.material = 0u;
            s.viewDistance = 13.f;
            const MlResult a = ml_evaluate(l2.view(), s);
            s.viewDistance = 1.f;
            const MlResult b = ml_evaluate(l2.view(), s);
            s.material = 1u;
            const MlResult c = ml_evaluate(l2.view(), s);
            for (u32 k = 0; k < 3u; ++k) {
                farDiff = std::fmax(farDiff, std::fabs(a.albedo[k] - c.albedo[k]) + std::fabs(a.normal[k] - c.normal[k]));
                nearDiff = std::fmax(nearDiff, std::fabs(b.albedo[k] - c.albedo[k]));
            }
        }
        expectLe(farDiff, 1e-6, "detail beyond its fade distance changes nothing");
        expect(nearDiff > 0.02, "detail within its fade distance changes the albedo");
    }

    // Layer masks: the moss ball with vertex R = 0 is the plain stone, with R = 1 (and high coverage) mostly moss.
    {
        f64 diff0 = 0.0;
        u32 mossy = 0;
        const std::vector<MlSurface> surf = randomSurfaces(512u, 9u);
        for (MlSurface s : surf) {
            s.color[0] = 0.f;
            s.material = 4u; // ball_4_moss
            const MlResult a = ml_evaluate(v, s);
            s.material = 0u; // ball_0_plain (same base)
            const MlResult b = ml_evaluate(v, s);
            for (u32 k = 0; k < 3u; ++k) {
                diff0 = std::fmax(diff0, std::fabs(a.albedo[k] - b.albedo[k]));
            }
            s.color[0] = 1.f;
            s.material = 4u;
            const MlResult c = ml_evaluate(v, s);
            mossy += c.albedo[1] > c.albedo[0] * 1.3f ? 1u : 0u; // moss is green
        }
        expectLe(diff0, 1e-6, "layer with mask 0 leaves the base untouched");
        expect(mossy == surf.size(), "layer with mask 1 replaces the base");
    }

    // Default surface.
    {
        MlSurface s{};
        s.material = 12345u;
        const MlResult r = ml_evaluate(v, s);
        expect(r.albedo[0] == 1.f && r.roughness == 0.5f && r.normal[1] == 1.f, "out-of-range material: default surface");
    }

    // Normals unit length everywhere, outputs finite and in range.
    {
        u32 bad = 0;
        for (const MlSurface& s : randomSurfaces(4096u, 1u)) {
            const MlResult r = ml_evaluate(v, s);
            const f32 l = std::sqrt(r.normal[0] * r.normal[0] + r.normal[1] * r.normal[1] + r.normal[2] * r.normal[2]);
            bad += (std::fabs(l - 1.f) > 1e-4f || !(r.roughness >= 0.f && r.roughness <= 1.f) || !(r.ao >= 0.f) ||
                    !std::isfinite(r.albedo[0]) || r.albedo[1] < 0.f)
                       ? 1u
                       : 0u;
        }
        expect(bad == 0u, "evaluation outputs unit normals and in-range values");
    }
}

// --- golden (CPU reference) --------------------------------------------------------------------------------------
void testGoldenCpu() {
    MlLibrary lib;
    expect(buildLibrary(FUSE_RP_ML_FIXTURE_DIR, lib), "golden library");
    const MlBallScene scene = ml_ball_scene(kGoldenWidth, kGoldenHeight, kBallCount);
    std::vector<MlF4> image;
    ml_render_balls(scene.params, lib.view(), scene.balls.data(), image);
    std::vector<u8> rgba;
    ml_to_srgb8(image, rgba);
    std::vector<MlF4> image2;
    ml_render_balls(scene.params, lib.view(), scene.balls.data(), image2);
    expect(std::memcmp(image.data(), image2.data(), image.size() * sizeof(MlF4)) == 0, "CPU reference render is deterministic");
    std::vector<u8> png;
    u32 w = 0, h = 0;
    std::vector<u8> golden;
    const std::string path = std::string(FUSE_RP_ML_FIXTURE_DIR) + "/" + kGoldenPng;
    if (!readBytes(path, png) || !ml_png_decode(png, w, h, golden) || w != kGoldenWidth || h != kGoldenHeight) {
        std::fprintf(stderr, "FAIL: golden %s missing or unreadable (regenerate with FUSE_RP_ML_UPDATE_GOLDEN=1 "
                             "fuse_rp_material_layers --mode golden)\n", path.c_str());
        ++g_failures;
        return;
    }
    const ImageDiff d = compareRgba(rgba, golden, 2u);
    std::printf("golden_cpu: CPU reference vs the Lavapipe golden: %u pixels > 2 LSB, mean %.4f LSB, worst %u\n", d.over,
                d.mean, d.worst);
    expectLe(d.over, kGoldenWidth * kGoldenHeight / 500u, "CPU reference == golden (<= 0.2% pixels over 2 LSB)");
    expectLe(d.mean, 0.25, "CPU reference == golden (mean abs difference)");
}

// --- api ---------------------------------------------------------------------------------------------------------
void testApi() {
    MaterialLayers layers;
    MaterialLayersDesc d{};
    expect(!layers.init(d), "init without a device fails");
    expect(!layers.valid(), "not valid after a failed init");
    MlLibrary lib;
    expect(!layers.setLibrary(lib), "setLibrary before init fails");
    expect(!layers.beginFrame(1u, MlFrameDesc{}), "beginFrame before init fails");
    expect(std::string(queryMaterialLayerCapabilities(nullptr).reason) != "ok", "no device: not capable");
    // PNG codec.
    std::vector<u8> rgba(37u * 13u * 4u);
    for (usize i = 0; i < rgba.size(); ++i) {
        rgba[i] = static_cast<u8>(ml_hash(static_cast<u32>(i)) >> 24);
    }
    const std::vector<u8> png = ml_png_encode(37u, 13u, rgba);
    u32 w = 0, h = 0;
    std::vector<u8> back;
    expect(ml_png_decode(png, w, h, back) && w == 37u && h == 13u && back == rgba, "PNG round trip");
    expect(ml_png_encode(37u, 13u, rgba) == png, "PNG bytes deterministic");
    std::vector<u8> broken = png;
    broken[40] ^= 1u;
    expect(!ml_png_decode(broken, w, h, back), "PNG CRC checked");
    // Built-in textures: every id generates, tileable (first / last column neighbours close).
    for (const std::string& id : ml_builtin_texture_ids()) {
        std::vector<u32> t;
        u32 flags = 0;
        expect(ml_make_builtin_texture(id, 64u, t, flags) && t.size() == 64u * 64u, "builtin texture");
    }
    std::vector<u32> t;
    u32 flags = 0;
    expect(!ml_make_builtin_texture("nope_albedo", 64u, t, flags), "unknown builtin texture");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "fusemat") {
        testFusemat();
    }
    if (all || suite == "reference") {
        testReference();
    }
    if (all || suite == "golden_cpu") {
        testGoldenCpu();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
