/* FUSE: force-included into loslib.c on iOS / tvOS / watchOS, where system() is marked unavailable.
 * os.execute() then reports "no shell" (os.execute() -> false) and fails every command; Lua sources stay unmodified. */
#include <stdlib.h>
#define system(cmd) ((cmd) ? -1 : 0)
