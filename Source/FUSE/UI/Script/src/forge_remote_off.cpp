// fuse_forge_script without forge::remote: FORGE's remote.cpp pulls in forge::flow (rpc, generated op tables, lane
// kernels), which a GUI host does not need. Scripts calling forge::remote() get a clear PermissionError instead.
#include "interp.hpp"

namespace forge::script {
void install_remote(Shared& sh) {
    auto n = make<Native>("forge::remote", [](Interp& in, std::vector<Value>&) -> Value {
        in.raise("PermissionError", "forge::remote is not available in FUSE UI scripts");
    }, 2, 3);
    n->frozen = true;
    sh.builtins["forge::remote"] = obj(n);
}
} // namespace forge::script
