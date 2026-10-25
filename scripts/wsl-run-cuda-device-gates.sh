#!/usr/bin/env bash
# Run CUDA device gates (RTX 3090 / sm_86) in WSL. Requires toolkit + NVIDIA GPU.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="build-cuda"
if [[ $# -gt 0 && "${1}" != -* ]]; then
  BUILD_DIR="$1"
  shift
fi
BUILD="${ROOT}/${BUILD_DIR}"

unset VK_ICD_FILENAMES

if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
  echo "Missing ${BUILD}; configure first (see docs/building.md)" >&2
  exit 1
fi

cd "${BUILD}"
ctest -L cuda_device -j1 --output-on-failure "$@"
