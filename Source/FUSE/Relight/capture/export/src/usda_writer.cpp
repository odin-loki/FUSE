/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/lssusd/game_exporter.cpp@0867d3c (see usda_writer.hpp).
// FUSE Relight RL-1.8: the Remix-compatible USDA capture writer.
#include <fuse/relight/capture/export/usda_writer.hpp>

#include <fuse/relight/hash/hash_string.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace fuse::relight::capture::exporter {

namespace {

constexpr double kPi = 3.14159265358979323846;
/// GfCamera's default horizontal aperture (mm): SetPerspectiveFromAspectRatioAndFieldOfView keeps it.
constexpr double kDefaultHorizontalAperture = 20.955;

std::string quoteUsd(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string vec3(const Vec3f& v) { return "(" + formatFloat(v[0]) + ", " + formatFloat(v[1]) + ", " + formatFloat(v[2]) + ")"; }
std::string vec2(const Vec2f& v) { return "(" + formatFloat(v[0]) + ", " + formatFloat(v[1]) + ")"; }

std::string matrix(const Mat4d& m) {
    std::string s = "( ";
    for (int r = 0; r < 4; ++r) {
        s += r ? ", (" : "(";
        for (int c = 0; c < 4; ++c) {
            s += (c ? ", " : "") + formatDouble(m[r * 4 + c]);
        }
        s += ")";
    }
    return s + " )";
}

template <typename T, typename F>
std::string array(const std::vector<T>& v, F&& fmt) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) {
            s += ", ";
        }
        s += fmt(v[i]);
    }
    return s + "]";
}

std::string boolText(bool b) { return b ? "1" : "0"; }

std::string timeCode(double t) { return formatDouble(t); }

/// A USDA layer under construction.
class Layer {
public:
    void header(const std::vector<std::string>& metadata) {
        m_s += "#usda 1.0\n(\n";
        for (const std::string& m : metadata) {
            m_s += "    " + m + "\n";
        }
        m_s += ")\n";
    }
    /// `def Type "name" (metadata) {`
    void open(std::string_view specifier, std::string_view type, std::string_view name,
              const std::vector<std::string>& metadata = {}) {
        line("");
        std::string h = std::string(specifier);
        if (!type.empty()) {
            h += " " + std::string(type);
        }
        h += " " + quoteUsd(name);
        if (metadata.empty()) {
            line(h);
        } else {
            line(h + " (");
            ++m_indent;
            for (const std::string& m : metadata) {
                line(m);
            }
            --m_indent;
            line(")");
        }
        line("{");
        ++m_indent;
    }
    void close() {
        --m_indent;
        line("}");
    }
    void line(std::string_view text) {
        if (text.empty()) {
            if (!m_s.empty() && m_s.back() == '\n' && m_s.size() >= 2 && m_s[m_s.size() - 2] != '{') {
                m_s += '\n';
            }
            return;
        }
        m_s.append(static_cast<std::size_t>(m_indent) * 4, ' ');
        m_s += text;
        m_s += '\n';
    }
    /// `decl = value`, or `decl.timeSamples = { t: value, ... }`.
    void samples(const std::string& decl, const std::vector<std::pair<double, std::string>>& values, bool timeSampled) {
        if (values.empty()) {
            return;
        }
        if (!timeSampled) {
            line(decl + " = " + values.front().second);
            return;
        }
        line(decl + ".timeSamples = {");
        ++m_indent;
        for (const auto& [t, v] : values) {
            line(timeCode(t) + ": " + v + ",");
        }
        --m_indent;
        line("}");
    }
    std::string done() const { return m_s; }

private:
    std::string m_s;
    int m_indent = 0;
};

std::vector<std::string> commonMetadata(const CaptureMeta& meta) {
    return {"metersPerUnit = " + formatDouble(meta.metersPerUnit), "timeCodesPerSecond = " + formatDouble(meta.timeCodesPerSecond),
            std::string("upAxis = ") + (meta.isZUp ? "\"Z\"" : "\"Y\"")};
}

std::string customLayerData(const std::vector<std::string>& entries) {
    std::string s = "customLayerData = {\n";
    for (const std::string& e : entries) {
        s += "        " + e + "\n";
    }
    return s + "    }";
}

bool isSingleFrame(const CaptureMeta& meta) { return meta.numFramesCaptured <= 1; }

/// setTimeSampledXforms (one sample per SampledXform; the default value when one frame was captured).
void xformOps(Layer& l, const CaptureMeta& meta, const std::vector<SampledXform>& xforms) {
    if (xforms.empty()) {
        return;
    }
    std::vector<std::pair<double, std::string>> values;
    for (const SampledXform& x : xforms) {
        values.emplace_back(x.time, matrix(x.xform));
    }
    l.samples("matrix4d xformOp:transform", values, !isSingleFrame(meta));
    l.line("uniform token[] xformOpOrder = [\"xformOp:transform\"]");
}

/// setVisibilityTimeSpan.
void visibilitySpan(Layer& l, const CaptureMeta& meta, double firstTime, double finalTime, const char* defaultValue) {
    l.line(std::string("token visibility = ") + quoteUsd(defaultValue));
    if (isSingleFrame(meta)) {
        return;
    }
    std::vector<std::pair<double, std::string>> v;
    if (firstTime != 0.0) {
        v.emplace_back(0.0, "\"invisible\"");
    }
    v.emplace_back(firstTime, "\"inherited\"");
    if (finalTime != firstTime) {
        v.emplace_back(finalTime, "\"inherited\"");
    }
    v.emplace_back(std::nextafter(finalTime, finalTime + 1.0), "\"invisible\"");
    l.samples("token visibility", v, true);
}

/// setLightIntensityOnTimeSpan.
void intensitySpan(Layer& l, const CaptureMeta& meta, float intensity, double firstTime, double finalTime) {
    if (isSingleFrame(meta)) {
        l.line("float inputs:intensity = " + formatFloat(intensity));
        return;
    }
    std::vector<std::pair<double, std::string>> v;
    if (firstTime != 0.0) {
        v.emplace_back(0.0, "0");
    }
    v.emplace_back(firstTime, formatFloat(intensity));
    if (finalTime != firstTime) {
        v.emplace_back(finalTime, formatFloat(intensity));
    }
    v.emplace_back(std::nextafter(finalTime, finalTime + 1.0), "0");
    l.line("float inputs:intensity = " + formatFloat(intensity));
    l.samples("float inputs:intensity", v, true);
}

/// Translation / rotation (quaternion, real first) / scale of a row-vector matrix (UsdSkelDecomposeTransform).
void decompose(const Mat4d& m, Vec3f& t, std::array<float, 4>& q, Vec3f& s) {
    t = {float(m[12]), float(m[13]), float(m[14])};
    double r[9];
    for (int i = 0; i < 3; ++i) {
        const double len = std::sqrt(m[i * 4] * m[i * 4] + m[i * 4 + 1] * m[i * 4 + 1] + m[i * 4 + 2] * m[i * 4 + 2]);
        s[i] = float(len);
        for (int j = 0; j < 3; ++j) {
            r[i * 3 + j] = len > 0.0 ? m[i * 4 + j] / len : (i == j ? 1.0 : 0.0);
        }
    }
    // Row-vector matrix r = R^T of the column-vector rotation R.
    const double R00 = r[0], R01 = r[3], R02 = r[6], R10 = r[1], R11 = r[4], R12 = r[7], R20 = r[2], R21 = r[5], R22 = r[8];
    const double tr = R00 + R11 + R22;
    double w, x, y, z;
    if (tr > 0.0) {
        const double k = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * k;
        x = (R21 - R12) / k;
        y = (R02 - R20) / k;
        z = (R10 - R01) / k;
    } else if (R00 > R11 && R00 > R22) {
        const double k = std::sqrt(1.0 + R00 - R11 - R22) * 2.0;
        w = (R21 - R12) / k;
        x = 0.25 * k;
        y = (R01 + R10) / k;
        z = (R02 + R20) / k;
    } else if (R11 > R22) {
        const double k = std::sqrt(1.0 + R11 - R00 - R22) * 2.0;
        w = (R02 - R20) / k;
        x = (R01 + R10) / k;
        y = 0.25 * k;
        z = (R12 + R21) / k;
    } else {
        const double k = std::sqrt(1.0 + R22 - R00 - R11) * 2.0;
        w = (R10 - R01) / k;
        x = (R02 + R20) / k;
        y = (R12 + R21) / k;
        z = 0.25 * k;
    }
    q = {float(w), float(x), float(y), float(z)};
}

std::string meshName(Hash64 h) { return hash::primName(hash::prim_prefix::kMesh, h); }
std::string matName(Hash64 h) { return hash::primName(hash::prim_prefix::kMaterial, h); }
std::string lightName(Hash64 h) { return hash::primName(hash::prim_prefix::kLight, h); }
std::string skelName(Hash64 h) { return hash::primName(hash::prim_prefix::kSkeleton, h); }

bool visualCorrection(const CaptureData& c, bool& invX, bool& invY) {
    invX = c.camera.valid && !c.camera.viewInv && (c.camera.projInv || c.camera.isLHS());
    invY = c.camera.valid && !c.camera.viewInv && c.camera.projInv;
    return invX || invY;
}

// ---- layers -----------------------------------------------------------------------------------------------

std::string materialLayer(const CaptureData& c, const CaptureMaterial& mat) {
    const std::string name = matName(mat.hash);
    Layer l;
    std::vector<std::string> meta{"defaultPrim = \"Looks\""};
    for (const std::string& m : commonMetadata(c.meta)) {
        meta.push_back(m);
    }
    l.header(meta);
    l.open("def", "Scope", "Looks");
    l.open("def", "Material", name);
    l.line("token outputs:mdl:surface.connect = </Looks/" + name + "/Shader.outputs:out>");
    l.open("def", "Shader", "Shader", {"kind = \"Material\""});
    l.line("uniform bool enable_opacity = " + boolText(mat.enableOpacity));
    l.line("uniform uint filter_mode = " + std::to_string(mat.filter));
    l.line("uniform token info:implementationSource = \"sourceAsset\"");
    l.line("uniform asset info:mdl:sourceAsset = @./AperturePBR_Opacity.mdl@");
    l.line("uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"");
    l.line("asset inputs:diffuse_texture = @../" + captureTexturePath(mat.albedoTexture) + "@ (");
    l.line("    colorSpace = \"auto\"");
    l.line(")");
    l.line("token outputs:out");
    l.line("uniform uint wrap_mode_u = " + std::to_string(mat.wrapU));
    l.line("uniform uint wrap_mode_v = " + std::to_string(mat.wrapV));
    l.close();
    l.close();
    l.close();
    return l.done();
}

std::vector<std::string> componentLayerData(const CaptureMesh& mesh) {
    std::vector<std::string> out;
    for (std::uint32_t i = 0; i < hash::kHashComponentCount; ++i) {
        const auto c = static_cast<hash::HashComponent>(i);
        out.push_back("uint64 " + std::string(hash::hashComponentName(c)) + " = " + std::to_string(mesh.components[c]));
    }
    return out;
}

std::string meshLayer(const CaptureData& c, const CaptureMesh& mesh) {
    const std::string name = meshName(mesh.hash);
    const bool skinned = mesh.numBones > 0;
    bool invX = false, invY = false;
    const bool correction = visualCorrection(c, invX, invY);
    Layer l;
    std::vector<std::string> meta{customLayerData(componentLayerData(mesh)), "defaultPrim = " + quoteUsd(name)};
    for (const std::string& m : commonMetadata(c.meta)) {
        meta.push_back(m);
    }
    l.header(meta);
    if (correction) {
        // visual_correction: flip the standalone mesh like the instance stage flips its instances.
        l.open("def", "Xform", "visual_correction");
        Mat4d x = identity4d();
        x[0] = invX ? -1.0 : 1.0;
        x[5] = invY ? -1.0 : 1.0;
        l.line("matrix4d xformOp:transform = " + matrix(x));
        l.line("uniform token[] xformOpOrder = [\"xformOp:transform\"]");
    }
    const std::string xformPath = correction ? "/visual_correction/" + name : "/" + name;
    std::vector<std::string> xmeta;
    if (mesh.materialHash != 0) {
        xmeta.push_back("prepend apiSchemas = [\"MaterialBindingAPI\"]");
    }
    l.open("def", skinned ? "SkelRoot" : "Xform", name, xmeta);
    if (mesh.materialHash != 0) {
        l.line("rel material:binding = </Looks/" + matName(mesh.materialHash) + ">");
    }
    l.line("token visibility = \"inherited\"");
    std::vector<std::string> mmeta;
    if (skinned) {
        mmeta.push_back("prepend apiSchemas = [\"SkelBindingAPI\"]");
    }
    l.open("def", "Mesh", "mesh", mmeta);
    l.line("uniform bool doubleSided = " + boolText(mesh.isDoubleSided));
    std::vector<int> counts(mesh.indices.size() / 3, 3);
    l.line("int[] faceVertexCounts = " + array(counts, [](int v) { return std::to_string(v); }));
    l.line("int[] faceVertexIndices = " + array(mesh.indices, [](std::int32_t v) { return std::to_string(v); }));
    // exportBufferSet: one buffer -> the default value, several (dynamic geometry) -> time samples only.
    auto buffers = [&](const std::string& decl, const std::vector<Vec3f>& first, const std::map<double, std::vector<Vec3f>>& later) {
        if (later.empty()) {
            l.line(decl + " = " + array(first, vec3));
            return;
        }
        std::vector<std::pair<double, std::string>> v{{mesh.firstTime, array(first, vec3)}};
        for (const auto& [t, values] : later) {
            v.emplace_back(t, array(values, vec3));
        }
        l.samples(decl, v, true);
    };
    if (!mesh.normals.empty()) {
        buffers("normal3f[] normals", mesh.normals, mesh.normalSamples);
    }
    l.line("uniform token orientation = \"rightHanded\"");
    buffers("point3f[] points", mesh.points, mesh.pointSamples);
    if (!mesh.colors.empty()) {
        const char* interp = mesh.colors.size() == 1 ? "constant" : "vertex";
        std::vector<Vec3f> rgb;
        std::vector<float> a;
        for (const Vec4f& col : mesh.colors) {
            rgb.push_back({col[0], col[1], col[2]});
            a.push_back(col[3]);
        }
        l.line("color3f[] primvars:displayColor = " + array(rgb, vec3) + " (");
        l.line(std::string("    interpolation = \"") + interp + "\"");
        l.line(")");
        l.line("float[] primvars:displayOpacity = " + array(a, formatFloat) + " (");
        l.line(std::string("    interpolation = \"") + interp + "\"");
        l.line(")");
    }
    if (skinned) {
        const std::string es = std::to_string(mesh.bonesPerVertex);
        l.line("int[] primvars:skel:jointIndices = " + array(mesh.jointIndices, [](std::int32_t v) { return std::to_string(v); }) + " (");
        l.line("    elementSize = " + es);
        l.line("    interpolation = \"vertex\"");
        l.line(")");
        l.line("float[] primvars:skel:jointWeights = " + array(mesh.jointWeights, formatFloat) + " (");
        l.line("    elementSize = " + es);
        l.line("    interpolation = \"vertex\"");
        l.line(")");
    }
    if (!mesh.texcoords.empty()) {
        l.line("texCoord2f[] primvars:st = " + array(mesh.texcoords, vec2) + " (");
        l.line("    interpolation = \"vertex\"");
        l.line(")");
    }
    for (std::uint32_t i = 0; i < scene::kInstanceCategoryCount; ++i) {
        const auto cat = static_cast<scene::InstanceCategories>(i);
        const bool set = mesh.categories.test(cat);
        if (cat == scene::InstanceCategories::HairCards && !set) {
            continue; // upstream keeps hair cards absent when false
        }
        l.line(std::string("custom uniform bool ") + remixCategoryAttribute(cat) + " = " + boolText(set));
    }
    if (skinned) {
        l.line("rel skel:skeleton = <" + xformPath + "/skel>");
    }
    l.line("uniform token subdivisionScheme = \"none\"");
    l.line("token visibility = \"inherited\"");
    l.line("matrix4d xformOp:transform = " + matrix(identity4d()));
    l.line("uniform token[] xformOpOrder = [\"xformOp:transform\"]");
    l.close(); // mesh
    l.close(); // mesh_<H>
    if (correction) {
        l.close();
    }
    if (mesh.materialHash != 0) {
        const std::string mat = matName(mesh.materialHash);
        l.open("def", "", "Looks");
        l.open("def", "Material", mat,
               {"prepend references = @../" + std::string(usd_dir::kMaterials) + "/" + mat + ".usda@</Looks/" + mat + ">"});
        l.close();
        l.close();
    }
    return l.done();
}

std::string skeletonLayer(const CaptureData& c, const CaptureMesh& mesh, const CaptureSkeleton& skel) {
    const std::string name = skelName(mesh.hash);
    Layer l;
    std::vector<std::string> meta{customLayerData(componentLayerData(mesh)), "defaultPrim = " + quoteUsd(name)};
    for (const std::string& m : commonMetadata(c.meta)) {
        meta.push_back(m);
    }
    l.header(meta);
    l.open("def", "SkelRoot", name);
    l.open("def", "Skeleton", "skel");
    l.line("uniform matrix4d[] bindTransforms = " + array(skel.bindPose, matrix));
    l.line("uniform token[] joints = " + array(skel.jointNames, [](const std::string& s) { return quoteUsd(s); }));
    l.line("uniform matrix4d[] restTransforms = " + array(skel.restPose, matrix));
    l.close();
    l.close();
    return l.done();
}

std::string sphereLightLayer(const CaptureData& c, const CaptureSphereLight& light) {
    const std::string name = lightName(light.hash);
    Layer l;
    std::vector<std::string> meta{"defaultPrim = " + quoteUsd(name)};
    for (const std::string& m : commonMetadata(c.meta)) {
        meta.push_back(m);
    }
    l.header(meta);
    l.open("def", "SphereLight", name, {"prepend apiSchemas = [\"ShapingAPI\"]"});
    l.line("color3f inputs:color = " + vec3(light.color));
    intensitySpan(l, c.meta, light.intensity, light.firstTime, light.finalTime);
    l.line("float inputs:radius = " + formatFloat(light.radius));
    // The shaping attributes always exist (external tools expect them); values only when shaping is on.
    // Remix's cone angle default is 180 (USD's is 90).
    l.line("float inputs:shaping:cone:angle = " + formatFloat(light.shapingEnabled ? light.coneAngleDegrees : 180.f));
    if (light.shapingEnabled) {
        l.line("float inputs:shaping:cone:softness = " + formatFloat(light.coneSoftness));
        l.line("float inputs:shaping:focus = " + formatFloat(light.focusExponent));
    } else {
        l.line("float inputs:shaping:cone:softness");
        l.line("float inputs:shaping:focus");
    }
    xformOps(l, c.meta, light.xforms);
    l.close();
    return l.done();
}

std::string instanceStage(const CaptureData& c) {
    const CaptureMeta& m = c.meta;
    Layer l;
    std::vector<std::string> layerData;
    if (c.camera.valid) {
        layerData.push_back("dictionary cameraSettings = {\n            string boundCamera = \"/RootNode/cameras/Camera\"\n        }");
    }
    layerData.push_back("string lightspeed_exe_name = " + quoteUsd(m.exeName));
    layerData.push_back("string lightspeed_game_icon = " + quoteUsd(m.iconPath));
    layerData.push_back("string lightspeed_game_name = " + quoteUsd(m.windowTitle));
    layerData.push_back("string lightspeed_geometry_hash_rules = " + quoteUsd(m.geometryHashRule));
    layerData.push_back("string lightspeed_layer_type = \"capture\"");
    std::vector<std::string> meta{customLayerData(layerData), "defaultPrim = \"RootNode\"",
                                  "endTimeCode = " + formatDouble(m.endTimeCode), "metersPerUnit = " + formatDouble(m.metersPerUnit),
                                  "startTimeCode = " + formatDouble(m.startTimeCode),
                                  "timeCodesPerSecond = " + formatDouble(m.timeCodesPerSecond),
                                  std::string("upAxis = ") + (m.isZUp ? "\"Z\"" : "\"Y\"")};
    l.header(meta);
    l.open("def", "", "RootNode");

    // lights (exportSphereLights / exportDistantLights): the root carries the global correction.
    l.open("def", "Xform", "lights");
    for (const auto& [h, light] : c.sphereLights) {
        const std::string name = lightName(h);
        l.open("def", "SphereLight", name,
               {"prepend references = @./" + std::string(usd_dir::kLights) + "/" + name + ".usda@"});
        l.close();
    }
    for (const auto& [h, light] : c.distantLights) {
        l.open("def", "DistantLight", lightName(h));
        l.line("float inputs:angle = " + formatFloat(light.angleDegrees));
        l.line("color3f inputs:color = " + vec3(light.color));
        intensitySpan(l, m, light.intensity, light.firstTime, light.finalTime);
        xformOps(l, m, {{0.0, rotationBetween({0.f, 0.f, -1.f}, light.direction, {0.f, 0.f, 0.f})}});
        l.close();
    }
    l.line("matrix4d xformOp:transform = " + matrix(c.globalXform));
    l.line("uniform token[] xformOpOrder = [\"xformOp:transform\"]");
    l.close();

    // meshes (exportMeshes / exportSkeletons on the instance stage): hidden prototypes.
    l.open("def", "", "meshes");
    for (const auto& [h, mesh] : c.meshes) {
        const std::string name = meshName(h);
        bool invX = false, invY = false;
        const std::string src = visualCorrection(c, invX, invY) ? "/visual_correction/" + name : "/" + name;
        std::vector<std::string> pm;
        if (mesh.materialHash != 0) {
            pm.push_back("prepend apiSchemas = [\"MaterialBindingAPI\"]");
        }
        pm.push_back("prepend references = @./" + std::string(usd_dir::kMeshes) + "/" + name + ".usda@<" + src + ">");
        l.open("def", mesh.numBones > 0 ? "SkelRoot" : "Xform", name, pm);
        if (mesh.materialHash != 0) {
            l.line("rel material:binding = </RootNode/Looks/" + matName(mesh.materialHash) + ">");
        }
        l.line("token visibility = \"invisible\"");
        if (mesh.numBones > 0 && c.skeletons.count(h) != 0) {
            const std::string sk = skelName(h);
            l.open("def", "Skeleton", "skel",
                   {"prepend references = @./" + std::string(usd_dir::kSkeletons) + "/" + sk + ".usda@</" + sk + "/skel>"});
            l.close();
        }
        l.close();
    }
    l.close();

    // Looks (exportMaterials on the instance stage).
    l.open("def", "", "Looks");
    for (const auto& [h, mat] : c.materials) {
        const std::string name = matName(h);
        l.open("def", "Material", name,
               {"prepend references = @./" + std::string(usd_dir::kMaterials) + "/" + name + ".usda@</Looks/" + name + ">"});
        l.close();
    }
    l.close();

    // instances (exportInstances).
    l.open("def", "Xform", "instances");
    for (const auto& [id, inst] : c.instances) {
        const CaptureMesh& mesh = c.meshes.at(inst.mesh);
        const bool skinned = !inst.boneXforms.empty() && c.skeletons.count(inst.mesh) != 0;
        const std::string path = "/RootNode/instances/" + inst.primName();
        std::vector<std::string> im;
        if (inst.material != 0) {
            im.push_back("prepend apiSchemas = [\"MaterialBindingAPI\"]");
        }
        im.push_back("prepend references = </RootNode/meshes/" + meshName(inst.mesh) + ">");
        l.open("def", skinned ? "SkelRoot" : "Xform", inst.primName(), im);
        if (inst.material != 0) {
            l.line("rel material:binding = </RootNode/Looks/" + matName(inst.material) + ">");
        }
        // The sky mesh stays hidden: it may block the dome light and cast shadows.
        visibilitySpan(l, m, inst.firstTime, inst.finalTime, inst.isSky ? "invisible" : "inherited");
        xformOps(l, m, inst.xforms);
        if (skinned) {
            const CaptureSkeleton& skel = c.skeletons.at(inst.mesh);
            l.open("def", "SkelAnimation", "pose");
            l.line("uniform token[] joints = " + array(skel.jointNames, [](const std::string& s) { return quoteUsd(s); }));
            std::vector<std::pair<double, std::string>> rot, scl, tr;
            for (const SampledBoneXforms& b : inst.boneXforms) {
                std::vector<Vec3f> ts, ss;
                std::vector<std::array<float, 4>> qs;
                for (const Mat4d& x : sanitizeBoneXforms(b.xforms, skel.bindPose)) {
                    Vec3f t, s;
                    std::array<float, 4> q;
                    decompose(x, t, q, s);
                    ts.push_back(t);
                    ss.push_back(s);
                    qs.push_back(q);
                }
                tr.emplace_back(b.time, array(ts, vec3));
                scl.emplace_back(b.time, array(ss, vec3));
                rot.emplace_back(b.time, array(qs, [](const std::array<float, 4>& q) {
                                     return "(" + formatFloat(q[0]) + ", " + formatFloat(q[1]) + ", " + formatFloat(q[2]) + ", " +
                                            formatFloat(q[3]) + ")";
                                 }));
            }
            l.samples("quatf[] rotations", rot, !isSingleFrame(m));
            l.samples("half3[] scales", scl, !isSingleFrame(m));
            l.samples("float3[] translations", tr, !isSingleFrame(m));
            l.close();
            l.open("over", "", "skel", {"prepend apiSchemas = [\"SkelBindingAPI\"]"});
            l.line("rel skel:animationSource = <" + path + "/pose>");
            l.close();
        } else {
            l.open("def", "Mesh", "mesh");
            const RenderingMetaData& md = inst.metadata;
            auto u = [&](const char* n, std::uint32_t v) { l.line(std::string("uint primvars:_remix_metadata:") + n + " = " + std::to_string(v)); };
            auto b = [&](const char* n, bool v) { l.line(std::string("bool primvars:_remix_metadata:") + n + " = " + boolText(v)); };
            b("alphaTestEnabled", md.alphaTestEnabled);
            u("alphaTestReferenceValue", md.alphaTestReferenceValue);
            u("alphaTestCompareOp", md.alphaTestCompareOp);
            b("alphaBlendEnabled", md.alphaBlendEnabled);
            u("srcColorBlendFactor", md.srcColorBlendFactor);
            u("dstColorBlendFactor", md.dstColorBlendFactor);
            u("colorBlendOp", md.colorBlendOp);
            u("srcAlphaBlendFactor", md.srcAlphaBlendFactor);
            u("dstAlphaBlendFactor", md.dstAlphaBlendFactor);
            u("alphaBlendOp", md.alphaBlendOp);
            u("writeMask", md.writeMask);
            u("textureColorArg1Source", md.textureColorArg1Source);
            u("textureColorArg2Source", md.textureColorArg2Source);
            u("textureColorOperation", md.textureColorOperation);
            u("textureAlphaArg1Source", md.textureAlphaArg1Source);
            u("textureAlphaArg2Source", md.textureAlphaArg2Source);
            u("textureAlphaOperation", md.textureAlphaOperation);
            u("tFactor", md.tFactor);
            b("isTextureFactorBlend", md.isTextureFactorBlend);
            b("isVertexColorBakedLighting", md.isVertexColorBakedLighting);
            l.close();
        }
        (void)mesh;
        l.close();
    }
    l.close();

    // cameras (exportCamera).
    l.open("def", "Xform", "cameras");
    if (c.camera.valid) {
        const CaptureCamera& cam = c.camera;
        l.open("def", "Camera", "Camera");
        l.line("float2 clippingRange = (" + formatFloat(cam.nearPlane) + ", " + formatFloat(cam.farPlane) + ")");
        // GfCamera::SetPerspectiveFromAspectRatioAndFieldOfView(aspect, vertical FoV in degrees).
        const double aspect = cam.aspectRatio != 0.f ? std::fabs(double(cam.aspectRatio)) : 1.0;
        const double verticalAperture = kDefaultHorizontalAperture / aspect;
        const double focalLength = verticalAperture / (2.0 * std::tan(double(cam.fov) * 0.5));
        l.line("float focalLength = " + formatFloat(float(focalLength)));
        l.line("float horizontalAperture = " + formatFloat(float(kDefaultHorizontalAperture)));
        xformOps(l, m, cam.xforms);
        l.close();
    }
    l.close();

    l.close(); // RootNode
    return l.done();
}

} // namespace

std::string formatFloat(float v) {
    if (std::isnan(v)) {
        return "nan";
    }
    if (std::isinf(v)) {
        return v > 0 ? "inf" : "-inf";
    }
    if (v == 0.f) {
        return "0"; // also -0
    }
    char buf[32];
    if (v == std::floor(v) && std::fabs(v) < 1e7f) {
        std::snprintf(buf, sizeof buf, "%.0f", double(v));
        return buf;
    }
    for (int p = 1; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, double(v));
        if (std::strtof(buf, nullptr) == v) {
            break;
        }
    }
    return buf;
}

std::string formatDouble(double v) {
    if (std::isnan(v)) {
        return "nan";
    }
    if (std::isinf(v)) {
        return v > 0 ? "inf" : "-inf";
    }
    if (v == 0.0) {
        return "0"; // also -0
    }
    char buf[40];
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof buf, "%.0f", v);
        return buf;
    }
    for (int p = 1; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) {
            break;
        }
    }
    return buf;
}

std::string captureStageFileName(const CaptureMeta& meta) { return meta.stageName + ".usda"; }

std::string captureTexturePath(Hash64 textureHash) {
    return std::string(usd_dir::kTextures) + "/" + hash::hashToString(textureHash) + ".dds";
}

std::map<std::string, std::string> writeRemixUsda(const CaptureData& capture) {
    std::map<std::string, std::string> files;
    for (const auto& [h, mat] : capture.materials) {
        files[std::string(usd_dir::kMaterials) + "/" + matName(h) + ".usda"] = materialLayer(capture, mat);
    }
    for (const auto& [h, mesh] : capture.meshes) {
        files[std::string(usd_dir::kMeshes) + "/" + meshName(h) + ".usda"] = meshLayer(capture, mesh);
        if (auto it = capture.skeletons.find(h); it != capture.skeletons.end()) {
            files[std::string(usd_dir::kSkeletons) + "/" + skelName(h) + ".usda"] = skeletonLayer(capture, mesh, it->second);
        }
    }
    for (const auto& [h, light] : capture.sphereLights) {
        files[std::string(usd_dir::kLights) + "/" + lightName(h) + ".usda"] = sphereLightLayer(capture, light);
    }
    files[captureStageFileName(capture.meta)] = instanceStage(capture);
    return files;
}

} // namespace fuse::relight::capture::exporter
