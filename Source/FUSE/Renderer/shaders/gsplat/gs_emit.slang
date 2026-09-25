// WP-9.2 "gsplat.emit". Slang twin of gs_emit.comp (see there).
#include "gs_common.slang"

[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    gsBegin();
    const uint i = tid.x;
    if (i >= F.splatCount) {
        return;
    }
    const uint4 rect = gsProjected(i, 3u);
    if (gsTilesTouched(rect) == 0u) {
        return;
    }
    const uint64_t depthBits = uint64_t(gsProjected(i, 0u).z);
    uint64_t* keys = gsKeys(F.keys);
    uint* values = gsUints(F.values);
    uint at = gsUints(F.offsets)[i];
    const uint cap = F.capacity;
    for (uint ty = rect.y; ty < rect.w; ++ty) {
        for (uint tx = rect.x; tx < rect.z; ++tx) {
            if (at < cap) {
                keys[at] = (uint64_t(ty * F.tilesX + tx) << 32) | depthBits;
                values[at] = i;
            }
            ++at;
        }
    }
}
