# FUSE Relight user guide

FUSE Relight is a drop-in Direct3D 8 and Direct3D 9 runtime. It sits in a game's folder as `d3d9.dll`
(and `d3d8.dll`), translates the game's rendering to Vulkan with DXVK, captures the scene, and renders it
again with the FUSE renderer: remastered raster rendering, or path tracing on GPUs with Vulkan ray
tracing. It reads RTX Remix mods and `rtx.conf` files, so existing community mods keep working.

Relight is AGPL-3.0-licensed. It ships no NVIDIA or Intel binary. DLSS, Reflex, XeSS and NRD are optional
plugins that you install yourself (see [Optional plugins](#optional-plugins)).

Design and status: [plans/FUSE_REMIX_PORT_PLAN.md](plans/FUSE_REMIX_PORT_PLAN.md). Developer notes on the
plugin seams: [nvidia-plugin.md](nvidia-plugin.md), [upscalers.md](upscalers.md).

## Requirements

- Windows 10 or 11, or Linux with a recent Wine (the FUSE test suite runs on Wine with Mesa's Lavapipe).
- A GPU and driver with Vulkan 1.3. Path tracing needs Vulkan ray tracing (`VK_KHR_ray_query`).
- 32-bit games also run a 64-bit helper process (`fuse_relight_host.exe`), so they need a 64-bit OS.

## Install

The package has this layout:

```
FUSE-Relight/
  README.txt  LICENSE.txt  THIRD_PARTY_NOTICES.txt  licenses/  docs/relight.md
  MANIFEST.sha256  manifest.json
  x64/                              for 64-bit games
    d3d9.dll  d3d8.dll  rtx.conf
    fuse_relight_launcher.exe  fuse_relight_plugins.exe
    fuse_relight_plugins/           optional plugins go here
  x86/                              for 32-bit games
    d3d9.dll  d3d8.dll  rtx.conf    32-bit bridge client
    fuse_relight_launcher.exe
    fuse_relight_passthrough/d3d9.dll   32-bit DXVK, used if the host cannot start
    fuse_relight/                   64-bit Relight host (keep this folder next to the bridge)
      fuse_relight_host.exe  d3d9.dll  fuse_relight_plugins.exe
      fuse_relight_plugins/
```

1. Find out whether the game is 32-bit or 64-bit. Most Direct3D 8 and 9 games are 32-bit. Task Manager
   shows "(32 bit)" next to 32-bit processes; on Linux, `file game.exe` prints `PE32` (32-bit) or `PE32+`
   (64-bit).
2. Copy the contents of `x64/` (64-bit) or `x86/` (32-bit) into the folder that holds the game's `.exe`.
   Keep the folder structure: for 32-bit games, `fuse_relight/` must sit next to `d3d9.dll`.
3. Start the game normally. On Wine, allow the native DLLs:
   `WINEDLLOVERRIDES="d3d9,d3d8=n,b" wine game.exe` (or set them to "native, builtin" in `winecfg`).
4. To uninstall, delete the files you copied.

Check the download with `sha256sum -c MANIFEST.sha256` in the package folder. `manifest.json` lists the
same hashes and says whether the 32-bit bridge is included (`"x86_bridge"`).

### How 32-bit games work

A 32-bit process cannot load the 64-bit renderer. The 32-bit `d3d9.dll` in `x86/` is a bridge client: it
starts `fuse_relight\fuse_relight_host.exe`, sends it the game's Direct3D calls through shared memory,
and the host renders them with the 64-bit Relight `d3d9.dll` next to it. `d3d8.dll` is DXVK's Direct3D 8
front end on top of the bridge. If the host cannot start or stops responding, the bridge falls back to
plain DXVK in the game process (`fuse_relight_passthrough\d3d9.dll`) and logs why.

`FUSE_RELIGHT_BRIDGE_HOST=<path>` runs a different host executable, and
`FUSE_RELIGHT_BRIDGE_HOST_ARGS="--verbose"` passes it extra arguments.

### The launcher

Some games load `d3d9.dll` from the system folder, or start through a launcher that resets the DLL search
path. For those, start the game with the launcher next to the copied DLLs:

```
fuse_relight_launcher.exe [-w <working folder>] [--dll <path>]... [--] <game.exe> [game arguments]
fuse_relight_launcher.exe --self-test
fuse_relight_launcher.exe --list-anticheat
```

The launcher starts the game suspended, loads `d3d9.dll` from its own folder into it, and resumes it. Use
the launcher that matches the game (`x64/` for 64-bit, `x86/` for 32-bit). It refuses games that ship a
known anti-cheat (`--list-anticheat` prints the list), because injecting a DLL can get an account banned.

## Options

Relight reads the same configuration files as RTX Remix, in this order (later files win):

1. `dxvk.conf` (or the files in `DXVK_CONFIG_FILE`), DXVK's own settings;
2. the per-game profile FUSE ships for known games;
3. `rtx.conf` in the game's working directory (or the files in `DXVK_RTX_CONFIG_FILE`, `;`-separated);
4. `<mod folder>/rtx.conf` of the base game mod;
5. `user.conf`, written by the in-game menu for user settings.

Environment variables override every file. Keys are `name = value`; `#` starts a comment. Every `rtx.*`
key also answers to its `relight.*` twin name, so `rtx.foo` and `relight.foo` are the same option.

The shipped `rtx.conf` turns Relight on with remastered raster rendering:

```
relight.tap.mode = capture
relight.frame.mode = raster
relight.frame.injectAtUi = True
rtx.preTransformedVerticesIsUI = True
```

### Main switches

| Option | Environment variable | Values |
|---|---|---|
| (whole runtime) | `FUSE_RELIGHT` | `0`: plain DXVK, Relight completely off. Anything else or unset: on. |
| `relight.tap.mode` | `FUSE_RELIGHT_TAP_MODE` | `off`, `null`, `record` (event log), `capture` (needed for rendering and mods). |
| `relight.frame.mode` | `FUSE_RELIGHT_FRAME_MODE` | `off`, `passthrough` (game image through FUSE, diagnostics), `solid` (test colour), `raster`, `pathtrace`. |
| `relight.frame.injectAtUi` | `FUSE_RELIGHT_FRAME_INJECT_AT_UI` | `True`: FUSE's image replaces the scene at the first UI draw, so the HUD stays on top (Remix behaviour). `False`: at Present. |
| `relight.frame.statsPath` | `FUSE_RELIGHT_FRAME_STATS` | A JSON Lines file with one record per frame (troubleshooting). |
| (render tier cap) | `FUSE_RENDER_TIER_MAX` | `0`–`3`: caps FUSE's feature tier (0 is the lightest). |

### Scene and textures

The Remix texture-category keys work unchanged: `rtx.uiTextures`, `rtx.skyBoxTextures`,
`rtx.ignoreTextures`, `rtx.decalTextures`, `rtx.particleTextures` and the others take comma-separated
texture hashes (`0x` followed by 16 hex digits). The in-game developer menu (once available) writes them
for you, and the per-game profiles fill them in for known games.

### Mods

| Option | Environment variable | Default |
|---|---|---|
| `relight.replace.enable` | `FUSE_RELIGHT_REPLACE` | `True` |
| `relight.replace.modPaths` | `FUSE_RELIGHT_REPLACE_MOD_PATHS` | `fuse:fuse-relight/mods,remix:rtx-remix/mods` |
| `relight.replace.mods` | `FUSE_RELIGHT_REPLACE_MODS` | extra single mod folders |
| `relight.replace.hotReload` | `FUSE_RELIGHT_REPLACE_HOT_RELOAD` | `True` |
| `relight.replace.textureBudgetMiB` | `FUSE_RELIGHT_REPLACE_TEXTURE_BUDGET_MIB` | `1024` |
| `relight.replace.textureMipBias` | `FUSE_RELIGHT_REPLACE_TEXTURE_MIP_BIAS` | `0` |

Every option in a build, with its type, default, range and description, is listed by the options
module's generated reference (`OptionManager::writeMarkdownDocumentation`).

## Modding

Relight loads RTX Remix mods as they are published:

1. Put each mod in its own folder under `rtx-remix/mods/` in the game folder, as for Remix
   (`rtx-remix/mods/<mod name>/mod.usda`, with its textures and meshes next to it). Relight reads USDA and
   USDC layers, DDS textures, and Remix `.pkg` packages (GDeflate-compressed).
2. FUSE-native mods (produced by FUSE's import tool) go in `fuse-relight/mods/<mod name>/`.
3. Several mods stack in folder order. A mod's own `rtx.conf` applies when it is the base game mod.
4. Hot reload is on by default: save a file in a mod and the change shows at the next frame.

What Relight maps from a Remix mod: mesh and material replacements keyed by the Remix asset hashes, the
Remix material parameters (`AperturePBR_Opaque`, `AperturePBR_Translucent`, portals), UsdLux lights
(sphere, rect, disk, cylinder, distant), light and mesh instancing rules, and particle system settings.
Remix's MDL files are not needed or read: Relight uses the parameter names only.

Creating a mod: capture a scene with `FUSE_RELIGHT_TAP_CAPTURE_EXPORT=1` (`relight.tap.captureExport`,
capture mode only), which writes a USD capture in Remix's layout plus its DDS textures. Author replacements
against it in any USD tool; the RTX Remix Toolkit produces compatible mods.

## Optional plugins

Relight works without plugins. These optional runtimes add features when you install them:

| Plugin id | Adds | Files (Windows) | Minimum version | Folder | Variable |
|---|---|---|---|---|---|
| `dlss` | DLSS Super Resolution / DLAA | `fuse_nvplugin_streamline.dll`, `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll` | Streamline 2.x, DLSS 3.1 | `nvidia/` | `FUSE_NVIDIA_SDK_DIR` |
| `dlss_rr` | DLSS Ray Reconstruction (denoise + upscale for path tracing) | as `dlss`, plus `sl.dlss_d.dll`, `nvngx_dlssd.dll` | DLSS-RR 3.5 | `nvidia/` | `FUSE_NVIDIA_SDK_DIR` |
| `reflex` | NVIDIA Reflex latency reduction | the Streamline set plus `sl.reflex.dll`, `sl.pcl.dll` | Streamline 2.x | `nvidia/` | `FUSE_NVIDIA_SDK_DIR` |
| `xess` | Intel XeSS upscaling | `libxess.dll` | 1.x or 2.x | `xess/` | `FUSE_XESS_SDK_DIR` |
| `nrd` | NVIDIA NRD denoiser | `fuse_nrdplugin_nri.dll`, `NRD.dll` | NRD 4.x | `nrd/` | `FUSE_NRD_SDK_DIR` |

Get the vendor runtimes from the vendors, under their licences; FUSE never redistributes them. The
`fuse_nvplugin_*` and `fuse_nrdplugin_*` providers are FUSE's own MIT adapters, built from the FUSE
sources with `-DFUSE_ENABLE_NVIDIA_PLUGIN=ON` / `-DFUSE_ENABLE_NRD_PLUGIN=ON` (see
[nvidia-plugin.md](nvidia-plugin.md)).

Where Relight looks, for each plugin (the first folder that holds any of its files wins):

1. the plugin's variable (`FUSE_NVIDIA_SDK_DIR`, `FUSE_XESS_SDK_DIR`, `FUSE_NRD_SDK_DIR`);
2. `FUSE_RELIGHT_PLUGIN_DIR/<folder>` (one root for all plugins);
3. `fuse_relight_plugins/<folder>` next to the Relight `d3d9.dll` (for 32-bit games that is
   `fuse_relight/fuse_relight_plugins/`).

Run `fuse_relight_plugins.exe` (next to the Relight `d3d9.dll`) to see the result. It prints one line per
plugin, for example:

```
fuse-relight plugins: dlss: available: C:\Games\X\fuse_relight_plugins\nvidia (sl.interposer.dll 2.14.1, nvngx_dlss.dll 3.7.10)
fuse-relight plugins: dlss_rr: incomplete: found in C:\Games\X\fuse_relight_plugins\nvidia but missing sl.dlss_d.dll, nvngx_dlssd.dll; the plugin stays off
fuse-relight plugins: xess: version too old: libxess.dll 0.9.0 is older than the minimum 1.0.0 (in ...); the plugin stays off, install a supported version
fuse-relight plugins: nrd: absent: not installed (searched: ...); optional; install the runtime in ...\fuse_relight_plugins/nrd or set FUSE_NRD_SDK_DIR
```

`--json` prints the same as JSON, `--require <id>` exits with 1 when that plugin is not usable, and
`--file-version <dll>` prints a DLL's version. Versions are read from the files without loading them. A
plugin whose DLL has no version information is reported as `version unknown` and still tried; the
renderer's loader then checks the plugin's ABI when it loads it.

## Troubleshooting

| Symptom | What to do |
|---|---|
| The game looks exactly as before | Check that the DLLs are next to the game's `.exe` (or use the launcher), that `FUSE_RELIGHT` is not `0`, and that `rtx.conf` in the working directory sets `relight.tap.mode = capture` and a `relight.frame.mode`. On Wine, check the `d3d9,d3d8=n,b` override. |
| The game does not start or crashes at start-up | Set `FUSE_RELIGHT=0`. If it still fails, the problem is in DXVK or the driver: update the GPU driver and read the DXVK log. If it works, file a Relight bug with the logs. |
| 32-bit game: "host ... not available" in the bridge log | `fuse_relight\fuse_relight_host.exe` and `fuse_relight\d3d9.dll` must be next to the bridge `d3d9.dll`. The game then runs on the passthrough DXVK. |
| The HUD disappears or is lit | Its textures are not marked as UI: list their hashes in `rtx.uiTextures`, or set `relight.frame.injectAtUi = False`. |
| A mod does not load | The mod folder must be `rtx-remix/mods/<name>/` under the game's working directory; check the Relight log for USD errors. `relight.replace.modPaths` changes the roots. |
| DLSS / XeSS / NRD missing from the menu | Run `fuse_relight_plugins.exe`; its line says what is missing or which version is wrong. |

Logs:

- `DXVK_LOG_PATH=<folder>` writes the DXVK and Relight logs there; `DXVK_LOG_LEVEL=info|debug` makes them
  more detailed.
- `FUSE_RELIGHT_BRIDGE_LOG=<file>` writes the 32-bit bridge's log.
- `FUSE_RELIGHT_FRAME_STATS=<file>` writes one JSON record per frame (injection point, errors).
- `FUSE_RELIGHT_VK_VALIDATION=1` enables the Vulkan validation layer on FUSE's Vulkan instance when it is
  installed (slow; for bug reports).

When you report a bug, include the logs, `manifest.json` from the package, the output of
`fuse_relight_plugins.exe`, and your `rtx.conf`.

## Licences

FUSE and FUSE Relight are AGPL-3.0-licensed. `THIRD_PARTY_NOTICES.txt` and `licenses/` in the package hold the
licence of every component compiled into the shipped files: DXVK (zlib, with FUSE's modifications marked
in the source), dxbc-spirv, libdisplay-info, the Khronos Vulkan and SPIR-V headers, the OpenVR headers
(BSD-3), the MIT-licensed parts of dxvk-remix that Relight ports, xxHash (BSD-2), the GDeflate codec (MIT
and Apache-2.0), TinyUSDZ (Apache-2.0) and its bundled libraries, and the FUSE renderer's meshoptimizer,
Vulkan Memory Allocator, volk, FidelityFX and NVIDIA Image Scaling shaders (MIT). "RTX Remix" is named
only to say which mods and configuration files Relight is compatible with.

## For packagers

The package is built by the `relight_package` target of a MinGW-w64 build tree
(`Source/FUSE/Relight/cmake/relight_package.cmake`) into `<build>/relight/package/FUSE-Relight`. The 32-bit
bridge comes from the i686 tree named by `FUSE_RELIGHT_BRIDGE_X86_DIR` (default `<build>/../relight-mingw32`);
without it the package holds the 64-bit runtime and the host payload only, and `manifest.json` says
`"x86_bridge": false`. `-DFUSE_RELIGHT_PACKAGE_REQUIRE_X86=ON` makes a missing i686 tree an error.

Gates (ctest):

| Test | Checks |
|---|---|
| `rl_package_selftest` | the package gates catch seeded violations in placeholder packages (runs in every tree) |
| `rl_package_plugin_discovery` | plugin discovery against mock plugin folders: present, absent, incomplete, wrong or unknown versions, search order |
| `rl_package_layout` | expected files, no forbidden files (DLSS/NGX/Streamline/XeSS/NRD runtimes, debug symbols, test helpers, binaries in plugin folders), PE machines, verbatim licence texts, notices, both manifests' hashes |
| `rl_package_licence_gates` | the RL-0.1 text and binary licence gates run over the package |
| `rl_package_wine_smoke` | apps run from the packaged layout under Wine: a 64-bit game with the `x64/` files, and the bridge layout with the packaged host folder |
