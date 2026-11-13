// Compatibility header: the clipboard formats of the `claude/magical-bassi-815825` branch (images, MIME types, lazy
// provision, primary selection) are methods of NativePlatform on main (fuse/app/native.hpp: setClipboard,
// setClipboardImage, clipboardImage, clipboardData, clipboardFormats, clipboardOwned, setPrimarySelectionText,
// primarySelectionText) with the types in fuse/app/services.hpp (ClipboardItem, kClipboardText, kClipboardImage).
// The branch's free functions took no platform; main's clipboard belongs to the NativePlatform that owns the display.
#pragma once

#include "fuse/app/native.hpp"
