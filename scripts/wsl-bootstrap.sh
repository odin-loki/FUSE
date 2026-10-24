#!/usr/bin/env bash
# WSL2 dev dependencies for FUSE (Ubuntu 24.04). See docs/building.md#wsl2-recommended-on-windows
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
  echo "wsl-bootstrap: apt-get not found (expected Ubuntu on WSL)" >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y \
  ninja-build cmake g++-13 \
  libvulkan-dev mesa-vulkan-drivers vulkan-tools \
  vulkan-validationlayers glslang-tools spirv-tools \
  xvfb x11-utils libx11-dev libxtst-dev libxi-dev

if [[ -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]]; then
  echo "Khronos validation layer: OK"
else
  echo "WARN: VkLayer_khronos_validation missing" >&2
fi

if command -v glslangValidator >/dev/null 2>&1; then
  echo "glslangValidator: OK ($(command -v glslangValidator))"
else
  echo "WARN: glslangValidator missing — re-run cmake in build-wsl after installing glslang-tools" >&2
fi

echo
echo 'export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json'
echo "Then re-configure: cmake -B build-wsl -G Ninja -DCMAKE_BUILD_TYPE=Release \\"
echo "  -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON -DFUSE_BUILD_VULKAN=ON \\"
echo "  -DFUSE_BUILD_EDITOR=OFF -DFUSE_BUILD_EDITOR_API=OFF"
