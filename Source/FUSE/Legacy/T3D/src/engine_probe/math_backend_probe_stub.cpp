// Minimal math_backend dispatch tables for FUSE_T3D_LEGACY_ENGINE_PROBE (bitStream.cpp).
// Replaces Engine/source/math/public/math_backend.cpp — scalar only, no CPU dispatch.

#include "math/public/math_backend.h"

namespace math_backend::float4::dispatch
{
Float4Funcs gFloat4{};
}

namespace math_backend::float3::dispatch
{
Float3Funcs gFloat3{};
}

namespace math_backend::mat44::dispatch
{
Mat44Funcs gMat44{};
}

namespace
{
struct ScalarInitializer
{
    ScalarInitializer()
    {
        math_backend::float4::dispatch::install_scalar();
        math_backend::float3::dispatch::install_scalar();
        math_backend::mat44::dispatch::install_scalar();
    }
};

ScalarInitializer g_scalarInitializer;
} // namespace
