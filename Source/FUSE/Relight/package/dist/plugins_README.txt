FUSE Relight optional plugins
=============================

FUSE Relight works without any plugin. Optional third-party runtimes add upscalers and a denoiser.
They are NOT included in this package and are never redistributed by FUSE: download them yourself from
their vendors, under the vendors' licences, and put them in these folders (or point the environment
variables below at the folders where you keep them):

  nvidia/  NVIDIA DLSS Super Resolution, DLSS Ray Reconstruction and Reflex, through Streamline 2.x:
           fuse_nvplugin_streamline.dll (built from FUSE's MIT sources), sl.interposer.dll, sl.common.dll,
           sl.dlss.dll, sl.dlss_d.dll, sl.reflex.dll, sl.pcl.dll, nvngx_dlss.dll (3.1 or newer),
           nvngx_dlssd.dll (3.5 or newer).                          Variable: FUSE_NVIDIA_SDK_DIR
  xess/    Intel XeSS: libxess.dll (1.x or 2.x).                  Variable: FUSE_XESS_SDK_DIR
  nrd/     NVIDIA NRD denoiser: fuse_nrdplugin_nri.dll (FUSE MIT provider), NRD.dll (4.x).
                                                                   Variable: FUSE_NRD_SDK_DIR

FUSE_RELIGHT_PLUGIN_DIR=<dir> makes Relight look in <dir>/nvidia, <dir>/xess and <dir>/nrd instead.

Run fuse_relight_plugins.exe (next to the Relight d3d9.dll) to see what was found, where, which
version, and why a plugin is off. A missing plugin is normal and only means that feature is
unavailable; FUSE then uses its own upscalers and denoiser.
