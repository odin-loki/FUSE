// "ssfx.ao_temporal". Slang twin of sx_ao_temporal.comp (see there) and of ssfx_gpu::ao_temporal_pixel.
#include "sx_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    sxBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint i = sxIndex(p.x, p.y);
    const uint64_t source = (F.flags & kSxFlagAoBlur) != 0u ? F.aoBlurred : F.ao;
    const float ao = sxFloats(source)[i];
    const float z = sxDepthAt(p.x, p.y);
    float2 motion = float2(0.0, 0.0);
    if (F.motion != 0ull) {
        motion = sxVec2s(F.motion)[i];
    }
    float result = ao;
    if (F.aoHistoryValid != 0u && z > 0.0) {
        const float fw = float(F.width);
        const float fh = float(F.height);
        const float px = (float(p.x) + 0.5) - motion.x * fw;
        const float py = (float(p.y) + 0.5) - motion.y * fh;
        if (px >= 0.0 && py >= 0.0 && px < fw && py < fh) {
            const uint h = uint(py) * F.width + uint(px);
            const float2 hist = sxVec2s(F.aoHistoryIn)[h];
            const float tolerance = F.aoTemporalDepthReject * z;
            if (hist.y > 0.0 && !(abs(hist.y - z) > tolerance)) {
                result = hist.x + (ao - hist.x) * F.aoTemporalAlpha;
            }
        }
    }
    sxFloats(F.aoDenoised)[i] = result;
    sxVec2s(F.aoHistoryOut)[i] = float2(result, z);
}
