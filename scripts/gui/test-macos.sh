#!/usr/bin/env bash
# macOS GUI build + tests (Cocoa backend). CI: macos-latest. Installs ninja/cmake via Homebrew if missing.
source "$(dirname "$0")/lib.sh"
BUILD_DIR=${BUILD_DIR:-$ROOT/build-gui-macos}
[ "$(uname)" = Darwin ] || { echo "test-macos.sh must run on macOS"; skip macos "not Darwin"; summary macos; }
if want_deps; then for t in ninja cmake; do command -v $t >/dev/null || step "deps ($t)" brew install $t; done; fi
step configure configure
step build build
step test ctest_run
summary macos
