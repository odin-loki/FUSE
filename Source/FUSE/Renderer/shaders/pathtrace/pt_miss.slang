// WP-7.3 miss shader (both ray types), Slang twin of pt_miss.rmiss (see there).
#include "lt_common.slang"
#include "pt_common.slang"

[shader("miss")]
void main(inout PtPayload payload) {
    payload.hit.t = -1.0;
}
