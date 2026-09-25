// FUSE Relight RL-4.3: RL-3.2 material records -> the Relight BSDF inputs (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.5,
// §5.2).
//
// The RL-3.2 importer (mods/import/material_table.hpp) turns every AperturePBR Opacity / Translucent / Portal shader
// into a MaterialParams (every table parameter, sanitized to upstream's ranges). bsdfMaterialFromParams resolves the
// constants into a bsdf::BsdfMaterial; the texture parameters are sampled at shading time and override the
// matching fields (see BsdfTextureSlots): the mapping here is the untextured material and the flags.
//
// Mapping (upstream semantics, dxvk-remix @0867d3c):
//   Opaque       albedo = diffuse_color_constant^2.2 (upstream gammaToLinear, kSRGBGamma = 2.2), opacity =
//                opacity_constant, roughness = reflection_roughness_constant (perceptual), metallic_constant,
//                anisotropy, thin film (enable_thin_film, thin_film_thickness_constant nm; *_from_albedo_alpha keeps
//                the constant here and flags the texture path), emission = enable_emission ?
//                emissive_color_constant^2.2 * emissive_intensity : 0.
//                SSS: subsurface_diffusion_profile with a non-zero radius -> kBsdfFlagSssDiffusion (sssRadius =
//                subsurface_radius * subsurface_radius_scale); otherwise subsurface_measurement_distance > 0 ->
//                kBsdfFlagSssThin (upstream isThinOpaqueSubsurfaceMaterial); transmittance colour ^2.2,
//                single-scattering albedo and volumetric anisotropy as authored.
//                Hair: when the draw carries the HairCards category (plan §5.2), the Chiang model: beta_m = beta_n =
//                roughness, sigma_a from the albedo (Chiang eq. 9), eta 1.55, alpha 2 degrees.
//   Translucent  ior_constant, transmittance_color^2.2 at transmittance_measurement_distance (thin_walled:
//                thin_wall_thickness), use_diffuse_layer -> kBsdfFlagDiffuseLayer (colour / coverage come from the
//                transmittance texture's rgb / alpha at shading time, upstream; untextured: no coverage).
//   Portal       no scattering; emission = enable_emission ? emissive_intensity : 0 (the mask texture modulates it).
//
// Divergences from upstream (FUSE decisions, plan §5.2):
//   * Energy conservation (white furnace): upstream's opaque lobe is single-scattering GGX + Hammon diffuse (no
//     multiple scattering, diffuse not coupled to the specular albedo). FUSE adds Kulla-Conty f_ms and the reciprocal
//     albedo-scaled diffuse (Lambert default; Burley and upstream's Hammon selectable).
//   * Opacity: upstream folds opacity into albedo and base reflectivity; FUSE scales the whole reflective BSDF by
//     opacity and passes 1 - opacity through (same energy, furnace-exact).
//   * Thin film: Belcour-Barla spectral integration instead of upstream's three-wavelength Airy sum.
//   * Translucent diffuse layer: a Lambertian layer on the outer face (coverage d: reflects d * colour, passes 1 - d)
//     instead of upstream's PSR-only radiance-cache weight; energy <= 1 from inside.
//   * SSS diffusion (Burley) and hair (Chiang) are FUSE's own, from the papers (upstream: RTXCR, not used).
//   * Anisotropy: the energy compensation indexes the albedo table by each direction's projected roughness (exact
//     Smith masking, approximate albedo): white furnace within 2.5% for |anisotropy| <= 0.5, looser beyond.
//   * Thin-opaque SSS sheets are two-sided (the frame mirrors below the surface) so the transmission is reciprocal;
//     upstream's Lambert fallback below 0.05 measurement distance is not ported (Hanrahan transmission throughout).
//   * Alpha is floored at 1e-3 instead of upstream's 1e-4 + dirac switch; GGX sampling is the unbounded spherical cap.
//   * Hair: I0 is summed to convergence and log I0 uses the full asymptotic series (pbrt's 10-term / halved forms
//     are ~2% off near x = 12, which breaks sampling-vs-pdf consistency); fiber angles use a portable atan2 so the
//     GPU (loose Vulkan atan precision) matches the CPU.
#pragma once

#include <fuse/relight/mods/import/material_table.hpp>
#include <fuse/relight/render/material/bsdf_host.hpp>

#include <string>
#include <vector>

namespace fuse::relight::render::material {

struct BsdfMapOptions {
    bool hairCards = false;                          ///< the draw's HairCards instance category
    bsdf::uint diffuseModel = bsdf::kBsdfDiffuseLambert; ///< kBsdfDiffuse*
    float hairIor = 1.55f;
    float hairAlphaRadians = 0.0349066f;             ///< 2 degrees (Chiang 2016)
};

/// Texture parameters that override BsdfMaterial fields at shading time (authored ones only).
struct BsdfTextureSlots {
    std::vector<std::string> params;  ///< RL-3.2 texture parameter names in table order
};

/// Upstream gammaToLinear: pow(c, 2.2) per channel.
bsdf::float3 gammaToLinear(const bsdf::float3& c);

/// Resolves the constants of `p` into a BSDF material (see the header comment).
bsdf::BsdfMaterial bsdfMaterialFromParams(const mods::import::MaterialParams& p, const BsdfMapOptions& options = {});

/// The authored texture parameters of `p` that the shading path samples.
BsdfTextureSlots bsdfTextureSlots(const mods::import::MaterialParams& p);

} // namespace fuse::relight::render::material
