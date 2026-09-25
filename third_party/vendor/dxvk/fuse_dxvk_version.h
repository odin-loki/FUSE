/* FUSE pin record for the vendored DXVK tree (not an upstream file; FUSE contributors, MIT).
 * Declares the upstream release this tree was copied from, for `fuse_lint vendored-pins`
 * (Engine/lib/dxvk/VERSION: version_header). relight_dxvk.cmake cross-checks it against the
 * upstream RELEASE file at configure time. Update together with VERSION on a rebase. */
#pragma once

#define FUSE_DXVK_VERSION_MAJOR 3
#define FUSE_DXVK_VERSION_MINOR 1
#define FUSE_DXVK_VERSION_PATCH 1
