// Compatibility header: the desktop platform services of the `claude/magical-bassi-815825` branch are part of
// fuse/app/services.hpp (types: PlatformError, FileFilter, FileDialogDesc, ColorDialogDesc, Message*, DisplayInfo) and
// fuse/app/native.hpp (openFileDialog, saveFileDialog, pickFolderDialog, colorDialog, messageBox, revealInFileManager,
// openUrl, isOpenableUrl) on main. Code written against the branch's header (the FUSE-UI editor shell) includes this.
// Differences from the branch: openUrl returns bool (main's signature), DisplayInfo::workArea is the branch's `work`.
#pragma once

#include "fuse/app/native.hpp"
#include "fuse/app/services.hpp"
