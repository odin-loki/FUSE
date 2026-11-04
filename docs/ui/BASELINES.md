# FUSE UI baselines (plan §6, UI0 exit)

Measured 2026-11-04 on one PC: Intel Xeon Gold 6242 (2.8 GHz, 64 threads), NVIDIA RTX 3090,
Windows 11 Pro for Workstations 10.0.26300. Linux numbers are WSL2 Ubuntu 24.04 on the same machine (GCC 13.3,
Xvfb + lavapipe / llvmpipe LLVM 20.1, no GPU). Release builds. Other agents were building on the machine at the
same time: CPU timings are best-of-3 where noted and carry roughly ±30 % noise; sizes, memory and idle counts are
exact.

Reproduce with [`scripts/ui_baselines.py`](../../scripts/ui_baselines.py) (it prints these tables):

```
# Windows (Developer prompt; Qt DLLs on PATH for the Qt editor row)
python scripts/ui_baselines.py --tree <full tree> --ui-min <UI MINIMAL> --ui-full <UI FULL> [--qt <tree with Qt editor>]
# Linux
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json xvfb-run -a -s "-screen 0 1920x1080x24" \
    python3 scripts/ui_baselines.py --tree <full tree> --ui-min <UI MINIMAL> --ui-full <UI FULL>
```

Build trees used: full tree `-DFUSE_BUILD_VULKAN=ON -DFUSE_BUILD_EDITOR=OFF -DFUSE_EDITOR_SHELL=fuse`; standalone UI
builds `cmake -S Source/FUSE/UI -DFUSE_UI_TEXT=MINIMAL|FULL` (what an app links: fuse_app + fuse_ui_vk, no engine);
Qt editor tree `-DFUSE_BUILD_EDITOR=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/msvc2022_64`.

## Binary size (stripped)

Linux sizes are of `strip`ped copies; MSVC Release executables carry no symbols (PDB separate), so the file size is
the stripped size. `game_menu` does not exist yet; `hud_demo` (fuse_ui_game HUD sample) stands in for a game-UI binary.

| Binary | Windows MSVC | Linux GCC |
|---|---|---|
| `hello_app`, MINIMAL text (standalone UI build) | 0.73 MB | 1.00 MB |
| `hello_app`, FULL text (HarfBuzz + FreeType + SheenBidi + libunibreak) | 2.27 MB | 3.15 MB |
| `notes_app` MINIMAL / FULL | 0.69 / 2.24 MB | 1.02 / 3.17 MB |
| `hud_demo` MINIMAL / FULL | 0.56 / 2.11 MB | 1.02 / 3.17 MB |
| `hello_app` in the full tree (MINIMAL) | 0.76 MB | 1.07 MB |
| `fuse_app_host` (schema 2 app runner, fuse_project + fuse_core linked) | 0.88 MB | 1.32 MB |
| `fuse_runtime_smoke` (today's runtime: core, jobs, worlds, script, renderer, hybrid) | 14.49 MB | 28.48 MB |
| `fuse_editor_fuse` (FUSE UI editor shell) | 16.75 MB | 31.76 MB |
| `fuse_editor_qt_gates` (Qt editor + gate harness; Qt DLLs not counted) | 16.97 MB | — |

FULL text costs +1.5 MB (MSVC) / +2.15 MB (GCC) per executable. Proposed `fuse_app_binary_size` hold: hello_app
MINIMAL ≤ 0.80 MB (MSVC) / 1.10 MB (GCC), FULL ≤ 2.50 / 3.45 MB (+10 %).

## Startup to first presented frame

"Process" is spawn → exit of `hello_app --first-frame` / `fuse_app_host --first-frame` (median of 5); "in-process" is
`main()` → first present as the program reports it.

| Path | Windows process / in-process | Linux process / in-process |
|---|---|---|
| hello_app, Vulkan presenter, GPU draw list | 1903 / 1326 ms (RTX 3090) | 119 / 102 ms (lavapipe) |
| hello_app, Vulkan presenter, CPU raster upload | 2231 / 1634 ms | 103 / 88 ms |
| hello_app, software (GDI, `--software`) | 280 / 158 ms | — |
| `fuse_app_host Templates/FuseUIApp`, headless (minimal init path) | 66 / 24 ms | 18 / 15 ms |
| `fuse_app_host Templates/FuseUIApp`, real window (Vulkan) | 2020 / 1485 ms | 141 / 122 ms |
| `fuse_runtime_smoke --gpu-frame` (core + renderer bootstrap + 4 offscreen GPU frames) | 2397 ms | 7977 ms |

The 150 ms desktop target is met headless and with lavapipe, and nearly with GDI, but not with the NVIDIA Vulkan
driver: `ProjectHost` timing shows ~780 ms in window creation (Vulkan instance, device, swapchain — mostly the NVIDIA
ICD loading) and ~700 ms more to the first present (compute pipeline creation for `ui.draw_list`). Follow-ups: show the
first frame through GDI / CPU upload while Vulkan initialises on a worker, and a pipeline cache for the UI kernel.

## Idle (§6: 0 frames when nothing happens)

`fuse_app_idle_native --seconds=10`: a real window (Vulkan presenter) with a document and nothing to do.

| | Windows (Win32, RTX 3090) | Linux (X11 under Xvfb, lavapipe) |
|---|---|---|
| Frames presented in 10 s | 0 | 0 |
| Loop wakeups in 10 s | 1 (the timer that ends the run) | 1 |
| Process CPU over 10 s | 31–62 ms (0.3–0.6 %; GetProcessTimes has a 15.6 ms tick) | 0.1 ms (0.001 %) |
| Context switches | — | 1 |

Under ctest on Windows a few stray system `WM_TIMER`s wake the pump (≤ 26 in 3 s were seen); the gate allows ≤ 10
wakeups / s and ≤ 2 % CPU. The virtual-clock gate `fuse_app_idle_zero_frames` covers the loop logic.

## Memory (resident set of an idle app, after 3–5 s)

| Process | Windows working set | Linux RSS |
|---|---|---|
| hello_app, Vulkan presenter | 132.7 MB | 95.9 MB |
| hello_app, GDI | 18.6 MB | — |
| fuse_app_host FuseUIApp (Vulkan) | 132.7 MB | 96.8 MB |
| fuse_editor_fuse (editor shell, empty scene) | 173.2 MB (peak 175.4) | 127.0 MB |

The Vulkan driver dominates: hello_app is 18.6 MB without it, ~133 MB with the NVIDIA ICD loaded.

## Frame times

| Measure | Windows | Linux | §6 target |
|---|---|---|---|
| `fuse_ui_bench_10k` dirty panel in a 10k-node tree, median (MINIMAL) | 0.57–0.67 ms (best of 3+) | 0.47–0.50 ms | ≤ 0.5 ms |
| same, FULL text | 0.65 ms | 0.51 ms | ≤ 1.0 ms (FULL gate) |
| `fuse_ui_vk_budget` ~270-node HUD at 2560×1440, GPU median | 0.155–0.47 ms (RTX 3090; GPU shared with the desktop, clocks vary) | 32 ms (llvmpipe, CPU device: report only) | ≤ 0.5 ms |
| FUSE editor shell `fuse_editor_ui_gates --gate=ui_frame`: hover frame (update + damage raster) | 2.50 ms | 2.69 ms | — |
| FUSE editor shell full 1600×900 CPU raster | 16.8 ms | 17.8 ms | ≤ 2 ms (full repaint, GPU path) |
| Qt editor `fuse_editor_qt_gates ui_frame`: steady frame (viewport + status bar repaint + flush) | 1.70–2.05 ms | not built | — |
| Qt editor full-window repaint (1280×844, 10 docks) | 11.4–18.0 ms | not built | Qt baseline in §6 was 4.1–4.4 ms |

Notes:
- `fuse_ui_bench_10k` exceeds its 0.5 ms gate on this Xeon (2.8 GHz) in MSVC builds and sits on the edge with GCC;
  the earlier 0.30 ms figure came from a faster desktop CPU. Treat 0.5 ms as "mid desktop" and this machine as below it.
- The Qt full-window repaint here (11–18 ms) is far above the 4.1–4.4 ms recorded in §6: different window size / dock
  set and a loaded machine; the steady-state frame (≈ 1.7 ms) is the like-for-like comparison with the FUSE hover frame.
- The FUSE shell's full-window number is the CPU reference raster; the native presenter draws the list on the GPU
  (`fuse_ui_vk`), which `fuse_ui_vk_budget` times.
- `fuse_ui_vk_parity` / `fuse_ui_vk_budget` do not link in MSVC in-tree builds (vulkan-1.lib vs fuse_rhi volk symbols);
  the Windows GPU numbers come from the standalone UI build.
