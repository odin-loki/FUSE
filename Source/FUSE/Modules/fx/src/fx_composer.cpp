#include <fuse/fx/fx_composer.hpp>

namespace fuse::fx {

void FxComposer::attach(const FxSocket& /*socket*/) {
    ++m_attachments;
}

void FxComposer::tick() {
    ++m_tickCount;
}

} // namespace fuse::fx
