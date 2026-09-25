FUSE Relight @RL_PKG_VERSION@
=============================

FUSE Relight is a drop-in Direct3D 8 / Direct3D 9 runtime that remasters classic games with the FUSE
renderer. It is built on DXVK (Direct3D to Vulkan) and is compatible with RTX Remix mods and rtx.conf
files. It needs a Vulkan 1.3 GPU driver. It is MIT-licensed; see LICENSE.txt and THIRD_PARTY_NOTICES.txt.

Build: @RL_PKG_BUILD@

Install
-------
64-bit games:  copy everything inside x64/ into the folder that holds the game's .exe.
32-bit games:  copy everything inside x86/ into the folder that holds the game's .exe. The 32-bit
               d3d9.dll / d3d8.dll there forward the game's rendering to the 64-bit Relight host in
               the fuse_relight/ subfolder, which must stay next to them.
Not sure?      the game's .exe is 32-bit if it runs from "Program Files (x86)" or if Task Manager
               shows "(32 bit)"; most D3D8 / D3D9 games are 32-bit.

Start the game as usual. To undo, delete the copied files (d3d9.dll, d3d8.dll, rtx.conf,
fuse_relight*, fuse_relight/).

Games that load d3d9.dll from the system folder only: start them with fuse_relight_launcher.exe
<game.exe>, which loads Relight into the game. The launcher refuses games with known anti-cheat.

Contents
--------
  x64/d3d9.dll, x64/d3d8.dll              Relight runtime for 64-bit games
  x64/fuse_relight_launcher.exe            optional launcher (64-bit games)
  x64/fuse_relight_plugins.exe             optional-plugin report (see below)
  x64/fuse_relight_plugins/                where optional plugins go for 64-bit games
  x86/d3d9.dll, x86/d3d8.dll              bridge for 32-bit games (present when built)
  x86/fuse_relight_launcher.exe            optional launcher (32-bit games; present when built)
  x86/fuse_relight/fuse_relight_host.exe   64-bit Relight host the bridge starts
  x86/fuse_relight/d3d9.dll                Relight runtime used by the host
  x86/fuse_relight/fuse_relight_plugins.exe, x86/fuse_relight/fuse_relight_plugins/
                                           plugin report and plugin folder for 32-bit games
  x64/rtx.conf, x86/rtx.conf               default settings (edit freely)
  docs/relight.md                          user guide: options, mods, troubleshooting
  licenses/, THIRD_PARTY_NOTICES.txt       third-party licences
  MANIFEST.sha256, manifest.json           file list with SHA-256 hashes

Verify the download: "sha256sum -c MANIFEST.sha256" (Linux / Wine) or compare with
"certutil -hashfile <file> SHA256" (Windows).

Optional plugins
----------------
NVIDIA DLSS / Reflex, Intel XeSS and NVIDIA NRD are optional and are NOT included. Put the vendors'
runtimes in the fuse_relight_plugins/ folder (see the README.txt there) and run fuse_relight_plugins.exe to check
them.

Troubleshooting
---------------
  Logs: set DXVK_LOG_PATH=<folder> (DXVK and Relight logs) and FUSE_RELIGHT_BRIDGE_LOG=<file> (bridge).
  Turn Relight off without uninstalling: set FUSE_RELIGHT=0 (plain DXVK) or relight.frame.mode = off.
  More in docs/relight.md.
