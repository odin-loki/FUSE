// FUSE Relight RL-1.5: JSON spelling of TranslateTap's output. See translate_json.hpp.
#include <fuse/relight/scene/translate/translate_json.hpp>

#include <cmath>
#include <cstdio>

namespace fuse::relight::scene {

namespace {

std::string num(double v) {
    if (std::isnan(v)) {
        return "\"nan\"";
    }
    if (std::isinf(v)) {
        return v > 0 ? "\"inf\"" : "\"-inf\"";
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

template <typename It>
std::string floats(It begin, It end) {
    std::string s = "[";
    for (It it = begin; it != end; ++it) {
        s += (it == begin ? "" : ",") + num(static_cast<double>(*it));
    }
    return s + "]";
}

std::string hex64(std::uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "\"0x%016llx\"", static_cast<unsigned long long>(v));
    return buf;
}

const char* b(bool v) { return v ? "true" : "false"; }

std::string c4(const tap::Color4& c) {
    const float v[4] = {c.r, c.g, c.b, c.a};
    return floats(v, v + 4);
}

std::string lightsJson(const std::vector<LightRecord>& lights) {
    std::string s = "[";
    for (std::size_t i = 0; i < lights.size(); ++i) {
        s += (i ? "," : "") + lightRecordJson(lights[i]);
    }
    return s + "]";
}

} // namespace

std::string legacyMaterialJson(const LegacyMaterialRecord& m) {
    const BlendMode& bm = m.blendMode;
    std::string s = "{";
    s += "\"alpha_test\":" + std::string(b(m.alphaTestEnabled));
    s += ",\"alpha_test_op\":" + std::to_string(m.alphaTestCompareOp);
    s += ",\"alpha_ref\":" + std::to_string(m.alphaTestReferenceValue);
    s += ",\"blend\":" + std::string(b(bm.enableBlending));
    s += ",\"color_src\":" + std::to_string(bm.colorSrcFactor) + ",\"color_dst\":" + std::to_string(bm.colorDstFactor) +
         ",\"color_op\":" + std::to_string(bm.colorBlendOp);
    s += ",\"alpha_src\":" + std::to_string(bm.alphaSrcFactor) + ",\"alpha_dst\":" + std::to_string(bm.alphaDstFactor) +
         ",\"alpha_op\":" + std::to_string(bm.alphaBlendOp);
    s += ",\"write_mask\":" + std::to_string(bm.writeMask);
    s += ",\"diffuse_source\":\"" + std::string(textureArgSourceName(m.diffuseColorSource)) + "\"";
    s += ",\"specular_source\":\"" + std::string(textureArgSourceName(m.specularColorSource)) + "\"";
    s += ",\"tfactor\":" + std::to_string(m.tFactor);
    s += ",\"tex_color_op\":\"" + std::string(textureOperationName(m.textureColorOperation)) + "\"";
    s += ",\"tex_color_arg1\":\"" + std::string(textureArgSourceName(m.textureColorArg1Source)) + "\"";
    s += ",\"tex_color_arg2\":\"" + std::string(textureArgSourceName(m.textureColorArg2Source)) + "\"";
    s += ",\"tex_alpha_op\":\"" + std::string(textureOperationName(m.textureAlphaOperation)) + "\"";
    s += ",\"tex_alpha_arg1\":\"" + std::string(textureArgSourceName(m.textureAlphaArg1Source)) + "\"";
    s += ",\"tex_alpha_arg2\":\"" + std::string(textureArgSourceName(m.textureAlphaArg2Source)) + "\"";
    s += ",\"tf_blend\":" + std::string(b(m.isTextureFactorBlend));
    s += ",\"vc_baked\":" + std::string(b(m.isVertexColorBakedLighting));
    s += ",\"emissive_source\":\"" + std::string(emissiveSourceName(m.emissiveSource)) + "\"";
    s += ",\"d3d_material\":{\"diffuse\":" + c4(m.d3dMaterial.diffuse) + ",\"ambient\":" + c4(m.d3dMaterial.ambient) +
         ",\"specular\":" + c4(m.d3dMaterial.specular) + ",\"emissive\":" + c4(m.d3dMaterial.emissive) +
         ",\"power\":" + num(m.d3dMaterial.power) + "}";
    s += ",\"texture_slots\":[" + std::to_string(m.colorTextureSlots[0]) + "," + std::to_string(m.colorTextureSlots[1]) + "]";
    s += ",\"hash\":" + hex64(m.hash());
    return s + "}";
}

std::string fogRecordJson(const FogRecord& f) {
    return "{\"mode\":" + std::to_string(f.mode) + ",\"color\":" + floats(f.color.begin(), f.color.end()) +
           ",\"scale\":" + num(f.scale) + ",\"end\":" + num(f.end) + ",\"density\":" + num(f.density) +
           ",\"hash\":" + hex64(f.mode == 0 ? 0 : f.hash()) + "}";
}

std::string lightRecordJson(const LightRecord& l) {
    const char* type = l.type == hash::LightType::Distant ? "distant" : "sphere";
    std::string s = "{\"index\":" + std::to_string(l.d3dIndex) + ",\"d3d_type\":" + std::to_string(l.d3dType) +
                    ",\"type\":\"" + type + "\",\"hash\":" + hex64(l.hash) +
                    ",\"radiance\":" + floats(l.radiance.begin(), l.radiance.end()) +
                    ",\"intensity\":" + num(l.intensity);
    if (l.type == hash::LightType::Distant) {
        s += ",\"direction\":" + floats(l.direction.begin(), l.direction.end()) + ",\"half_angle\":" + num(l.halfAngle);
    } else {
        s += ",\"position\":" + floats(l.position.begin(), l.position.end()) + ",\"radius\":" + num(l.radius);
        s += ",\"shaping\":{\"enabled\":" + std::string(b(l.shaping.enabled)) +
             ",\"direction\":" + floats(l.shaping.direction.begin(), l.shaping.direction.end()) +
             ",\"cos_cone\":" + num(l.shaping.cosConeAngle) + ",\"softness\":" + num(l.shaping.coneSoftness) +
             ",\"focus\":" + num(l.shaping.focusExponent) + "}";
    }
    return s + "}";
}

std::string cameraStateJson(const CameraState& c) {
    const std::array<float, 3> p = c.position();
    const std::array<float, 3> d = c.direction();
    return std::string("{\"type\":\"") + cameraTypeName(c.type) + "\",\"fov\":" + num(c.fov) +
           ",\"aspect\":" + num(c.aspectRatio) + ",\"near\":" + num(c.nearPlane) + ",\"far\":" + num(c.farPlane) +
           ",\"lhs\":" + b(c.isLHS) + ",\"reverse_z\":" + b(c.isReverseZ) + ",\"shear_x\":" + num(c.shearX) +
           ",\"shear_y\":" + num(c.shearY) + ",\"position\":" + floats(p.begin(), p.end()) +
           ",\"direction\":" + floats(d.begin(), d.end()) + ",\"jitter_px\":[" + num(c.jitter.pixelX) + "," +
           num(c.jitter.pixelY) + "],\"jitter\":" + b(c.jitterDetected) + "}";
}

std::string translatedDrawJson(const TranslatedDraw& d) {
    const DrawClassification& r = d.classification;
    std::string s = "{\"frame\":" + std::to_string(d.frame) + ",\"index\":" + std::to_string(d.indexInFrame) +
                    ",\"status\":\"" + geometryStatusName(r.status) + "\",\"reason\":\"" + classifyReasonName(r.reason) +
                    "\",\"categories\":\"" + r.categories.toString() + "\",\"translated\":" + b(d.translated) +
                    ",\"texture_stage\":" + b(d.textureStageApplied);
    if (d.rasterOnly) {
        s += ",\"raster_only\":true";
    }
    if (d.translated || d.rasterOnly) {
        const DrawTransforms& t = d.transforms;
        s += ",\"material\":" + legacyMaterialJson(d.material) + ",\"fog\":" + fogRecordJson(d.fog);
        s += ",\"texgen\":\"" + std::string(texGenModeName(t.texgenMode)) + "\"";
        s += ",\"texture_transform\":" + floats(t.textureTransform.begin(), t.textureTransform.end());
        s += ",\"object_to_view\":" + floats(t.objectToView.begin(), t.objectToView.end());
        s += ",\"clip_plane\":" + std::string(b(t.enableClipPlane));
        s += ",\"clip_plane_eq\":" + floats(t.clipPlane.begin(), t.clipPlane.end());
        s += ",\"lights\":" + lightsJson(d.addedLights);
        s += ",\"viewport\":[" + std::to_string(d.viewportX) + "," + std::to_string(d.viewportY) + "," +
             std::to_string(d.viewportWidth) + "," + std::to_string(d.viewportHeight) + "]";
        s += ",\"min_z\":" + num(d.minZ) + ",\"max_z\":" + num(d.maxZ) + ",\"z_write\":" + b(d.zWriteEnable) +
             ",\"z_enable\":" + b(d.zEnable) + ",\"stencil\":" + b(d.stencilEnabled);
    }
    s += ",\"camera\":\"" + std::string(cameraTypeName(d.cameraType)) + "\"";
    s += ",\"alpha_swizzle\":" + std::string(b(d.alphaSwizzle)) + "}";
    return s;
}

std::string translatedFrameJson(const TranslatedFrame& f) {
    std::string s = "{\"frame\":" + std::to_string(f.frame) + ",\"lights\":" + lightsJson(f.lights) +
                    ",\"rejected_lights\":" + std::to_string(f.rejectedLights) + ",\"fog\":" + fogRecordJson(f.fog) +
                    ",\"fog_states\":[";
    for (std::size_t i = 0; i < f.fogStates.size(); ++i) {
        s += (i ? "," : "") + fogRecordJson(f.fogStates[i]);
    }
    s += "],\"cameras\":[";
    for (std::size_t i = 0; i < f.cameras.size(); ++i) {
        s += (i ? "," : "") + cameraStateJson(f.cameras[i]);
    }
    s += "],\"camera_cut\":" + std::string(b(f.cameraCut)) + "}";
    return s;
}

} // namespace fuse::relight::scene
