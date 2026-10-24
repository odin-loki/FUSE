#!/usr/bin/env bash
# Run renderer frame integration gates (WP-FI) in WSL. Requires wsl-bootstrap.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-wsl"

export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json

if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
  echo "Missing ${BUILD}; configure first (see docs/building.md)" >&2
  exit 1
fi

if [[ ! -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]]; then
  echo "Khronos validation layer not installed." >&2
  echo "Run: bash ${ROOT}/scripts/wsl-bootstrap.sh" >&2
  exit 2
fi

cd "${BUILD}"
ctest -R 'fuse_rp_frame_|fuse_scene_renderer_vk|fuse_hybrid_frame_vk' --output-on-failure "$@"
