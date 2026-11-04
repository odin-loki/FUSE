# FUSE UI baselines (plan §6, UI0 exit)

Measured 2026-11-04 on one PC: Intel Xeon Gold 6242 (2.8 GHz, 64 threads), NVIDIA RTX 3090, Windows 11 Pro for
Workstations 10.0.26300, MSVC Build Tools 2026 (19.51). Linux numbers are WSL2 Ubuntu 24.04 on the same machine
(GCC 13.3, Xvfb + lavapipe / llvmpipe LLVM 20.1, no GPU). Release builds, merged code of the UI5 schema 2 branch
(integration tip f96265c35 plus ProjectHost / pipeline cache). Other agents were building on the machine throughout
(Windows CPU load up to 100 %): CPU timings carry roughly ±30 % noise and are given as ranges over two full runs where
they moved; sizes, memory and idle counts are exact.

Updated 2026-11-05 for the CPU rasteriser / software presenter / Vulkan start-up work (rows marked *11-05*): the
machine was at 80–100 % CPU load the whole day (other agents' builds, VMs, OneDrive sync), so before / after pairs were
measured interleaved (old and new binary alternately, same minutes) and are given as min / median.

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

*11-05*: the faster CPU rasteriser (row runs, SSE2 / NEON lanes, the band thread pool) adds ~33 KB per executable:
standalone MINIMAL hello_app 870,912 → 904,192 B with MSVC (94.4 % of the re-baselined 958,000 B budget), 1.53 → 1.56 MB
stripped with GCC; notes_app / game_menu / hud_demo MINIMAL are 0.86 / 0.84 / 0.66 MB on Windows.

*11-05, after round 3* (clipboard / IME, variable fonts, the high-contrast / zoom polish): the GCC standalone hello_app
had grown to 1,822,104 B stripped, over its 1,683,600 B budget. GCC linked whole objects: FontCatalog::load called
enableVariableFonts() (the glyf / gvar decoder, ~46 KB), paint_enable.o pulled the COLRv1 / CBDT / MSDF rasterisers
with enableMsdf, and stb_image was compiled twice (fuse_text's bitmap glyphs and fuse_app's clipboard). Fixes: the UI
targets compile with `-ffunction-sections -fdata-sections` and link with `--gc-sections` (GCC / Clang; MSVC Release
already has /OPT:REF); FontCatalog lists fvar named instances only through a hook enableVariableFonts() installs (and
no longer enables variations itself); one PNG codec (`fuse/text/png_codec.hpp`, stb_image + stb_image_write) serves
fuse_text and fuse_app.

| hello_app (MINIMAL, Release) | before | after | budget |
|---|---|---|---|
| GCC 13.3 standalone, stripped | 1,822,104 B | 1,076,352 B | 1,683,600 → re-baselined 1,184,000 (+10 %) |
| MSVC standalone | 944,640 B | 945,664 B | 958,000 (98.7 %) |
| MSVC engine tree | — | 978,944 B | 994,600 (98.4 %) |

## Startup to first presented frame

`hello_app --startup-probe` (median of 5): milliseconds from process creation to the first presented frame, and to
the first frame presented through Vulkan after the asynchronous start-up switch. `fuse_app_host` reports `main()` →
first frame in-process and process spawn → exit.

| Path | Windows (RTX 3090) | Linux (lavapipe) |
|---|---|---|
| hello_app default: software first frame, then Vulkan | 91–129 ms, Vulkan at 1413–1773 ms | 38 ms, Vulkan at 244 ms |
| *11-05* same, before → after (interleaved, loaded machine) | Vulkan at 1591 / 1806 → 1047 / 1216 ms (first frame 233 / 288 → 204 / 267 ms) | Vulkan at 315 / 356 → 346 / 399 ms (code path unchanged; the gap is the document phase reading from a slower source tree, see the software presenter table) |
| hello_app synchronous Vulkan start-up (GPU draw list) | 1415–1869 ms | 218 ms |
| *11-05* same, before → after (interleaved) | 1083 / 1183 → 710 / 732 ms | 226 ms |
| hello_app synchronous Vulkan, CPU raster upload | 1159–1852 ms | 144 ms |
| hello_app software only (`--no-vulkan`, GDI / XPutImage) | 111–152 ms | 55 ms |
| `fuse_app_host Templates/FuseUIApp`, headless (minimal init path) | 12–19 ms (process 43–56 ms) | 36 ms (42 ms) |
| `fuse_app_host Templates/FuseUIApp`, real window (software first frame) | 75–79 ms (process 1222–2096 ms) | 59 ms (235 ms) |
| `fuse_runtime_smoke --gpu-frame` whole run (core + renderer bootstrap + 4 offscreen GPU frames) | 2035–2898 ms | 26.9 s |

### Where the Vulkan switch time goes (`FUSE_APP_VK_TRACE=1`)

*11-05*, Windows, min / median of 10 interleaved runs before → after (MAILBOX + DWM pacing on NVIDIA, below):

| Phase | before | after |
|---|---|---|
| instance | 468 / 612 ms | 436 / 578 ms (driver, unchanged) |
| device | 178 / 216 ms | 176 / 210 ms (driver, unchanged) |
| `ui.draw_list` pipeline (cache hit) | 5.3 / 6.1 ms | 3.8 / 5.8 ms |
| swapchain | 632 / 671 ms | **85 / 103 ms** |
| first Vulkan present | 2.1 / 2.5 ms | 4.9 / 6.0 ms |
| switch (process start → first Vulkan frame) | 1591 / 1806 ms | **1047 / 1216 ms** |

The 11-04 table:

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

*11-05* investigation (a standalone probe timing each call, `VK_LOADER_DEBUG`, DLL-load notifications):

- **Swapchain: fixed.** NVIDIA's first FIFO swapchain in a process costs 0.4–0.6 s on Windows; MAILBOX (and
  IMMEDIATE) take 35–50 ms. Recreating a MAILBOX swapchain as FIFO is quick (2.6 ms) but moves the cost into the first
  present (714 ms); FIFO_RELAXED is as slow as FIFO, and the image count (2 / 3) does not matter. The FIFO path also
  keeps ~49 MB more in the working set (hello_app 140 → 91 MB; `FUSE_APP_VK_FIFO=1` brings the 140 MB back). With
  `vsync` on, an NVIDIA device on Windows now presents through MAILBOX, and `dwmPace` restores FIFO's pacing: a window
  that presents again within 25 ms waits for that frame's fence, then `DwmFlush` (continuous frames measure 16.67 ms,
  exactly FIFO's). VK_KHR_present_wait does not pace MAILBOX here (it returns at once: 1 ms frames) and adds ~100 ms
  to `vkCreateDevice`.
- **Instance: driver.** `vkCreateInstance` loads nvoglv64.dll (47 MB; ~76 ms when loaded alone, ~120 ms of mapping and
  DllMain inside the call), which then verifies signatures (WINTRUST / CRYPT32 / rsaenh, ~100–150 ms), loads its
  shader compiler nvgpucomp64.dll (96 MB) and the NVIDIA App / ShadowPlay capture hook nvspcap64.dll, and walks the
  process modules with dbghelp (application profiles). A second `vkCreateInstance` in the same process takes 11 ms.
  Implicit layers are not the cost: this PC registers Steam's overlay and Fossilize layers (HKLM
  `SOFTWARE\Khronos\Vulkan\ImplicitLayers`, 64- and 32-bit) and RenderDoc 1.46 (HKCU), none of which activate
  without their environment variables, plus the driver's own VK_LAYER_NV_optimus / VK_LAYER_NV_present (from
  nv-vk64.json). Disabling them (`VK_LOADER_LAYERS_DISABLE=~implicit~` / `~all~`, `DISABLE_LAYER_NV_OPTIMUS_1`,
  `DISABLE_LAYER_NV_PRESENT_1`) or selecting the driver (`VK_LOADER_DRIVERS_SELECT`) changed nothing beyond noise. Only
  the user can trim this (the NVIDIA App overlay / instant replay off removes nvspcap64).
- **Device: driver.** `vkCreateDevice` takes 130–260 ms and loads no DLLs; the enumeration calls around it are ≤ 5 ms.
- What was already minimal stays: one VkInstance per process shared by all windows, two instance extensions, two
  swapchain images. Starting the worker before `show()` stays dropped: the DLL timeline shows window creation loading
  ~15 system DLLs (uxtheme, MSCTF, TextInputFramework, CoreMessaging, ...) that would queue behind nvoglv64's
  loader-lock time.

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
| hello_app, Vulkan presenter | 133.8 MB; *11-05* 139.3–140.0 → 90.6–91.1 MB (MAILBOX on NVIDIA) | 86.2 MB; *11-05* 89.4 MB |
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
| FUSE editor shell `fuse_editor_ui_gates --gate=ui_frame`: hover frame (update + damage raster) | 0.26 ms; *11-05* 0.02–0.05 ms | 0.18 ms | — |
| FUSE editor shell full 1600×900 CPU raster | 17.3–29.7 ms; *11-05* 42–47 → 1.4–2.9 ms (best of 3; gate ≤ 8 ms) | 29.9 ms (not re-measured) | ≤ 2 ms full repaint (GPU path) |
| Qt editor `fuse_editor_qt_gates ui_frame`: steady frame (viewport + status bar repaint + flush) | 1.70–3.8 ms | not built | — |
| Qt editor full-window repaint (1280×844, 10 docks) | 11.4–20.5 ms | not built | Qt baseline in §6 was 4.1–4.4 ms |

### CPU reference rasteriser (*11-05*)

`fuse_ui_cpu_raster_bench` (gate: 1080p cards ≤ 20 ms, small incremental frame ≤ 2 ms; every frame and 48 fuzzed
lists must equal the frozen pre-change rasteriser byte for byte). Median ms; "pre-change" is that frozen copy timed in
the same run. Windows MSVC (two runs, loaded machine) / Linux GCC 13:

| Frame | threaded | one thread | pre-change | Linux threaded / one thread / pre-change |
|---|---|---|---|---|
| sample 488×400 (194 cmds) | 0.26–0.28 | 1.29–1.67 | 10.8–13.1 | 0.16 / 0.97 / 4.95 |
| cards 1920×1080 (1553 cmds) | 1.48–1.53 | 13.4–13.7 | 177–194 | 1.33–1.45 / 10.3–10.7 / 75.6–102 |
| HUD 2560×1440 (451 cmds) | 1.07–1.11 | 9.4–16.5 | 82–97 | 0.80–0.89 / 7.2–7.9 / 42.8–43.2 |
| incremental: one button of the cards frame (59×30 px region) | 0.06–0.11 | — | 0.25–0.34 | 0.034 / — / 0.10 |

Where it came from (cards frame, one thread, op by op, before → after): rects 152 → 7.5 ms (the full-screen gradient
is a per-row fill; rounded interiors and rounded clips blend whole runs), shadows 85 → 9 ms (only the ring is evaluated
per pixel, bounds blur + 1 px instead of 2 × blur), HUD lines 25 → 3 ms (per-row reach of the segment); then 4-lane
SSE2 / NEON edges (~2–3×) and horizontal bands on ≤ 16 threads (~8–10×). An NDK arm64 build of the fuzz check run
under qemu-aarch64 matched the reference in 384 / 384 renders.

Software presenter, `hello_app --frame-probe --no-vulkan` (360×220 window), median of 40 frames, before → after (the
presenter now copies only the redrawn rect):

| | Windows (GDI) | Linux (XPutImage, Xvfb) |
|---|---|---|
| hover frame (one button changes) | 0.42–0.49 → 0.10–0.18 ms | 0.062 ms (after) |
| whole-window frame | 0.67–0.94 → 0.36–0.61 ms | 0.31 ms (after) |
| first software frame (process start → present), min / median | 128 / 152 → 151 / 171 ms, and with the order swapped 147 / 238 → 191 / 230 ms: load noise, the raster share is < 1 ms | 106 / 111 → 116 / 138 ms, all of it in the document phase (the bundled font and .fuseui are read from different source trees: OneDrive vs D:) |

Notes:
- The Qt editor rows come from a Qt build of the pre-merge branch the same day (the merge did not touch the Qt
  editor); the Qt editor was not built in WSL.
- The FUSE shell's full-window number is the CPU reference raster; native windows draw the list on the GPU
  (`fuse_ui_vk`), which `fuse_ui_vk_budget` times. Hover frames are like-for-like with the Qt steady frame.
- GPU numbers on the RTX 3090 move with clocks and with other GPU users (the desktop, other agents' tests).
