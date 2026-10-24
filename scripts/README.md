# FUSE scripts

| Script | Purpose |
|--------|---------|
| [`wsl-bootstrap.sh`](wsl-bootstrap.sh) | **WSL2 (Ubuntu):** install toolchain + Vulkan/Lavapipe + Khronos validation layer + `glslangValidator`. Run once: `bash scripts/wsl-bootstrap.sh` (sudo), or from PowerShell: `wsl -u root apt-get install -y glslang-tools vulkan-validationlayers` then re-run `cmake -B build-wsl …`. |
| [`wsl-run-frame-gates.sh`](wsl-run-frame-gates.sh) | After bootstrap + `build-wsl`: run `fuse_rp_frame_vk_*`, `fuse_scene_renderer_vk_*`, `fuse_hybrid_frame_vk_*` ctests. |
| [`wsl-build-local.sh`](wsl-build-local.sh) | Configure `build-wsl`, build, and run all gate ctests (editor off; see below). |

[`wsl-build-local.sh`](wsl-build-local.sh) configures and builds `build-wsl` with the Qt editor disabled (`FUSE_BUILD_EDITOR=OFF`) to avoid WSL Qt6 header build breaks. Run [`wsl-bootstrap.sh`](wsl-bootstrap.sh) once first so Vulkan validation layers, Lavapipe, and `glslang-tools` are installed for vk gate ctests.

If `glslangValidator` is missing from the host shell, install from **Windows** without opening WSL interactively:

```powershell
wsl -u root apt-get install -y glslang-tools vulkan-validationlayers
```

Then **re-run** `cmake -B build-wsl …` inside WSL so configure-time SPIR-V regen sees `glslangValidator` (see [`docs/building.md`](../docs/building.md#wsl2-recommended-on-windows)).

After bootstrap:

```bash
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
cmake -B build-wsl -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON -DFUSE_BUILD_VULKAN=ON \
  -DFUSE_BUILD_EDITOR=OFF -DFUSE_BUILD_EDITOR_API=OFF
cmake --build build-wsl
ctest --test-dir build-wsl -L gate -j"$(nproc)" --output-on-failure
```

Do **not** use Strawberry Perl’s CMake/GCC on Windows for these gates — see [`docs/building.md`](../docs/building.md#wsl2-recommended-on-windows).

**One-shot local build (no Qt editor):** `bash scripts/wsl-build-local.sh` — configures `build-wsl` with `FUSE_BUILD_EDITOR=OFF` (avoids a known WSL Qt6 header issue), builds with `-j$(nproc)`, then runs `ctest -L gate`. Run `wsl-bootstrap.sh` first so Vulkan frame tests do not skip.
