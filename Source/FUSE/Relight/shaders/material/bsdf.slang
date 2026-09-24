// FUSE Relight RL-4.3: the Relight BSDF in Slang (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2). The body is the
// single-source core Relight/kernels/bsdf_core.h (the CPU reference compiles the same text through
// kernels/bsdf_cpp.hpp); this file sets the Slang dialect. Compile with -I <Relight>/kernels and -fp-mode precise.
//
// The includer declares the albedo table (kBsdfLutWords floats, baked on the CPU by bsdf_albedo_lut) and defines
// FUSE_BSDF_LUT_READ(i) before including this file, e.g.
//   [[vk::binding(1, 0)]] StructuredBuffer<float> g_bsdfLut;
//   #define FUSE_BSDF_LUT_READ(i) g_bsdfLut[(i)]
//   #include "bsdf.slang"
// API: bsdfEval / bsdfPdf / bsdfSample(BsdfMaterial, wo, wi | u) in the local frame (see bsdf_core.h),
// bsdfMaterialUnpack(float4[11]) for the packed material rows.
#pragma once

#ifndef FUSE_BSDF_LUT_READ
#error "define FUSE_BSDF_LUT_READ(i) (the albedo table) before including bsdf.slang"
#endif

#define FUSE_BSDF_FN
#define FUSE_BSDF_CONST static const
#define FUSE_BSDF_OUT(T) out T
#define FUSE_BSDF_LUT_PARAM
#define FUSE_BSDF_LUT_ARG
#define FUSE_BSDF_WORDS_PARAM(name) float4 name[11]

#include "bsdf_core.h"
