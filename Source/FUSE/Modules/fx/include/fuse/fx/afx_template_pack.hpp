#pragma once

// Ore: third_party/addons/AFX-Template/game/ sample spell/FX content pack

#include <fuse/fx/fx_composer.hpp>

#include <string>

namespace fuse::fx {

/// Parse a minimal AFX-Template sample pack (line-based, no JSON dependency).
bool load_afx_template_pack_from_text(const std::string& text, FxComposer& composer, std::string* errorOut = nullptr);

/// Register AFX-Template sample effects + particle pool capacity from embedded pack.
bool registerAfxTemplateSamplePack(FxComposer& composer);

} // namespace fuse::fx
