/* FUSE Relight RL-4.1 gate (rl_frame_adopt, rl_frame_run.py adopt): no Vulkan loader is opened while d3d9.dll loads.
 *
 * d3d9.dll links fuse_rhi (the frame orchestration adopts DXVK's device into the FUSE renderer). fuse_rhi's volk would
 * open vulkan-1.dll from a static initialiser, i.e. inside DllMain under the loader lock, unless the image opts out
 * (FUSE_RHI_VOLK_NO_AUTO_INIT). This probe loads the d3d9.dll next to it and checks that neither vulkan-1.dll nor
 * winevulkan.dll is mapped after LoadLibrary returned, and that one is after the first use (Direct3DCreate9: DXVK
 * creates its instance). Prints one "probe" line; exit 0 pass, 1 fail. C99, no third-party code. */
#include <stdio.h>
#include <windows.h>

typedef void* (WINAPI* CreateFn)(UINT);
typedef ULONG (WINAPI* ReleaseFn)(void*);

static int vulkanMapped(void) {
    return GetModuleHandleA("vulkan-1.dll") != NULL || GetModuleHandleA("winevulkan.dll") != NULL;
}

int main(void) {
    char path[MAX_PATH];
    const int before = vulkanMapped();
    HMODULE dll = LoadLibraryA(".\\d3d9.dll");
    if (dll == NULL) {
        printf("probe FAIL LoadLibrary(d3d9.dll) error %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    const int afterLoad = vulkanMapped();
    GetModuleFileNameA(dll, path, MAX_PATH);
    CreateFn create = (CreateFn)(void (*)(void))GetProcAddress(dll, "Direct3DCreate9");
    void* d3d = create != NULL ? create(32u /* D3D_SDK_VERSION */) : NULL;
    const int afterCreate = vulkanMapped();
    if (d3d != NULL) {
        /* IDirect3D9::Release: IUnknown vtable slot 2. */
        ReleaseFn release = (ReleaseFn)(*(void***)d3d)[2];
        release(d3d);
    }
    printf("probe dll=%s before=%d after_load=%d after_create=%d d3d=%d\n", path, before, afterLoad, afterCreate,
           d3d != NULL ? 1 : 0);
    fflush(stdout);
    FreeLibrary(dll);
    return (before == 0 && afterLoad == 0 && afterCreate == 1 && d3d != NULL) ? 0 : 1;
}
