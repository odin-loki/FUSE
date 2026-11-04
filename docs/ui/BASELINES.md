# FUSE UI baselines (plan §6, UI0 exit)

Measured 2026-11-04 on one PC: Intel Xeon Gold 6242 (2.8 GHz, 64 threads), NVIDIA RTX 3090, Windows 11 Pro for
Workstations 10.0.26300, MSVC Build Tools 2026 (19.51). Linux numbers are WSL2 Ubuntu 24.04 on the same machine
(GCC 13.3, Xvfb + lavapipe / llvmpipe LLVM 20.1, no GPU). Release builds, merged code of the UI5 schema 2 branch
(integration tip f96265c35 plus ProjectHost / pipeline cache). Other agents were building on the machine throughout
(Windows CPU load up to 100 %): CPU timings carry roughly ±30 % noise and are given as ranges over two full runs where
they moved; sizes, memory and idle counts are exact.

Reproduce with [`scripts/ui_baselines.py`](../../scripts/ui_baselines.py) (it prints these tables):

```
# Windows (Developer prompt; Qt DLLs on PATH for the Qt editor row)
python scripts/ui_baselines.py --tree <full tree> --ui-min <UI MINIMAL> --ui-full <UI FULL> [--qt <tree with Qt editor>]
# Linux
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a -s "-screen 0 1920x1080x24" \
    python3 scripts/ui_baselines.py --tree <full tree> --ui-min <UI MINIMAL> --ui-full <UI FULL>
```

Build trees: full tree `-DFUSE_BUILD_VULKAN=ON -DFUSE_BUILD_EDITOR=OFF -DFUSE_EDITOR_SHELL=fuse`; standalone UI builds
`cmake -S Source/FUSE/UI -DFUSE_UI_TEXT=MINIMAL|FULL` (what an app links: fuse_app + fuse_ui_vk, no engine); Qt editor
tree `-DFUSE_BUILD_EDITOR=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/msvc2022_64`.

## Binary size (stripped)

Linux sizes are of `strip`ped copies; MSVC Release executables carry no symbols (PDB separate), so their file size is
the stripped size.

| Binary | Windows MSVC | Linux GCC |
|---|---|---|
| `hello_app` MINIMAL / FULL text (standalone UI build) | 0.84 / 2.39 MB | 1.24 / 3.39 MB |
| `notes_app` MINIMAL / FULL | 0.80 / 2.35 MB | 1.26 / 3.42 MB |
| `game_menu` MINIMAL / FULL | 0.79 / 2.33 MB | 1.37 / 3.53 MB |
| `hud_demo` MINIMAL / FULL | 0.61 / 2.16 MB | 1.15 / 3.31 MB |
| `hello_app` in the full tree (the `fuse_app_binary_size` gate binary) | 889,344 B (budget 890,400) | 1.31 MB |
| `fuse_app_host` (schema 2 app runner: fuse_project + fuse_core, no engine) | 0.98 MB | 1.56 MB |
| `fuse_hybrid_host` (same + hybrid game side: composer, worlds, physics, renderer) | 13.82 MB | 18.09 MB |
| `fuse_runtime_smoke` (today's runtime: core, jobs, worlds, script, renderer, hybrid) | 14.49 MB | 28.48 MB |
| `fuse_editor_fuse` (FUSE UI editor shell) | 16.83 MB | 31.99 MB |
| `fuse_editor_qt_gates` (Qt editor + gate harness; Qt DLLs not counted) | 16.97 MB | not built |

FULL text costs +1.55 MB (MSVC) / +2.15 MB (GCC) per executable. The persistent pipeline cache added ~4 KB to
hello_app (it uses stdio, not `<filesystem>`, which alone cost ~7 KB and broke the budget); hello_app is at 99.9 % of
its `fuse_app_binary_size` budget.

## Startup to first presented frame

`hello_app --startup-probe` (median of 5): milliseconds from process creation to the first presented frame, and to
the first frame presented through Vulkan after the asynchronous start-up switch. `fuse_app_host` reports `main()` →
first frame in-process and process spawn → exit.

| Path | Windows (RTX 3090) | Linux (lavapipe) |
|---|---|---|
| hello_app default: software first frame, then Vulkan | 91–129 ms, Vulkan at 1413–1773 ms | 38 ms, Vulkan at 244 ms |
| hello_app synchronous Vulkan start-up (GPU draw list) | 1415–1869 ms | 218 ms |
| hello_app synchronous Vulkan, CPU raster upload | 1159–1852 ms | 144 ms |
| hello_app software only (`--no-vulkan`, GDI / XPutImage) | 111–152 ms | 55 ms |
| `fuse_app_host Templates/FuseUIApp`, headless (minimal init path) | 12–19 ms (process 43–56 ms) | 36 ms (42 ms) |
| `fuse_app_host Templates/FuseUIApp`, real window (software first frame) | 75–79 ms (process 1222–2096 ms) | 59 ms (235 ms) |
| `fuse_runtime_smoke --gpu-frame` whole run (core + renderer bootstrap + 4 offscreen GPU frames) | 2035–2898 ms | 26.9 s |

### Where the Vulkan switch time goes (`FUSE_APP_VK_TRACE=1`)

| Phase | Windows, NVIDIA | Linux, lavapipe |
|---|---|---|
| `vkCreateInstance` (loader + ICD load) | 315–545 ms | 100–125 ms |
| device (enumerate, `vkCreateDevice`) | 100–130 ms | 90–180 ms |
| `ui.draw_list` compute pipeline: no cache / our VkPipelineCache hit | 10–15 ms / 0.8–1.0 ms | 130–165 ms / 105–135 ms |
| swapchain (on the worker) | 360–490 ms | 4–5 ms |
| first Vulkan present | 1.5 ms | 135–175 ms |
| switch (process start → first Vulkan frame), cache cold → warm | 823–931 ms → 795–982 ms (unchanged within noise) | 310–373 ms → 280–337 ms (−30 ms) |

The pipeline is not where the time goes on NVIDIA: the driver's own disk cache already makes it ~3 ms, and even with
`__GL_SHADER_DISK_CACHE=0` the uncached compile is 10–15 ms. The persistent `VkPipelineCache` (per-user cache dir,
validated by the kernel's SPIR-V hash and the driver's vendor / device / `pipelineCacheUUID`) brings that to ~1 ms and
saves ~30 ms on lavapipe, where LLVM still JIT-compiles at pipeline creation. The remaining switch time is driver work:
instance (NVIDIA ICD load; implicit layers off made no difference), device, and NVIDIA's first swapchain (~400 ms).
Starting the instance on a thread at `NativePlatform` construction was tried and dropped: on Windows it contends with
window creation (loader lock) and pushed the software first frame from ~70 ms to ~160 ms without shortening the switch.
The software first frame (≤ 150 ms target) is what users see; the switch happens behind it.

## Idle (§6: 0 frames when nothing happens)

`fuse_app_idle_native --seconds=10`: a real window (Vulkan presenter after the switch) with a document and nothing to do.

| | Windows (Win32, RTX 3090) | Linux (X11 under Xvfb, lavapipe) |
|---|---|---|
| Frames presented in 10 s | 0 | 0 |
| Loop wakeups in 10 s | 1–7 (the ending timer + stray system `WM_TIMER`s) | 1 |
| Process CPU over 10 s | 15–78 ms (0.16–0.78 %; GetProcessTimes ticks are 15.6 ms; machine at 100 % load) | 0.1 ms (0.001 %) |
| Context switches | — | 1 |

## Memory (resident set of an idle app, after 3–5 s)

| Process | Windows working set | Linux RSS |
|---|---|---|
| hello_app, Vulkan presenter | 133.8 MB | 86.2 MB |
| hello_app, software only | 18.9 MB | 11.4 MB |
| fuse_app_host FuseUIApp (Vulkan) | 136.5–137.1 MB | 89.9 MB |
| fuse_editor_fuse (editor shell, empty scene) | 152.4–172.5 MB | 143.8 MB |

The Vulkan driver dominates: ~115 MB of hello_app's working set on NVIDIA, ~75 MB with lavapipe.

## Frame times

| Measure | Windows | Linux | §6 target |
|---|---|---|---|
| `fuse_ui_bench_10k` dirty panel in a 10k-node tree, median, MINIMAL / FULL | 0.36–0.38 / 0.37 ms (in-tree runs under load: 0.39–0.73 ms) | 0.28 / 0.61 ms | ≤ 0.5 ms |
| `fuse_ui_vk_budget` 1920×1080, GPU median: full / blur-only / backdrop / incremental | 0.17 / 0.18–0.19 / 0.34 / 0.11 ms | 56 / 53 / 115 / 52 ms (llvmpipe, report only) | 1.0 / 1.0 / 2.0 / 0.25 ms |
| `fuse_ui_vk_budget` hud (~270-node game HUD), GPU median | 0.12–0.18 ms | 34 ms (report only) | ≤ 0.5 ms |
| FUSE editor shell `fuse_editor_ui_gates --gate=ui_frame`: hover frame (update + damage raster) | 0.26 ms | 0.18 ms | — |
| FUSE editor shell full 1600×900 CPU raster | 17.3–29.7 ms | 29.9 ms | ≤ 2 ms full repaint (GPU path) |
| Qt editor `fuse_editor_qt_gates ui_frame`: steady frame (viewport + status bar repaint + flush) | 1.70–3.8 ms | not built | — |
| Qt editor full-window repaint (1280×844, 10 docks) | 11.4–20.5 ms | not built | Qt baseline in §6 was 4.1–4.4 ms |

Notes:
- The Qt editor rows come from a Qt build of the pre-merge branch the same day (the merge did not touch the Qt
  editor); the Qt editor was not built in WSL.
- The FUSE shell's full-window number is the CPU reference raster; native windows draw the list on the GPU
  (`fuse_ui_vk`), which `fuse_ui_vk_budget` times. Hover frames are like-for-like with the Qt steady frame.
- GPU numbers on the RTX 3090 move with clocks and with other GPU users (the desktop, other agents' tests).
