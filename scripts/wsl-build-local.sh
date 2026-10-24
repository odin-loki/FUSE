#!/usr/bin/env bash
# Local WSL build + gate sweep (see docs/building.md).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-wsl"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json

missing=0
if [[ ! -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]]; then
  echo "Missing Khronos validation layer. Run: bash ${ROOT}/scripts/wsl-bootstrap.sh" >&2
  missing=1
fi
if ! command -v glslangValidator >/dev/null 2>&1; then
  echo "Missing glslangValidator. Run: bash ${ROOT}/scripts/wsl-bootstrap.sh" >&2
  echo "Or: wsl -u root apt-get install -y glslang-tools vulkan-validationlayers" >&2
  missing=1
fi
if [[ "${missing}" -ne 0 ]]; then
  exit 2
fi

cmake -S "${ROOT}" -B "${BUILD}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON -DFUSE_BUILD_VULKAN=ON \
  -DFUSE_BUILD_EDITOR=OFF -DFUSE_BUILD_EDITOR_API=OFF

cmake --build "${BUILD}" -j"$(nproc)"
ctest --test-dir "${BUILD}" -L gate -j"$(nproc)" --output-on-failure
