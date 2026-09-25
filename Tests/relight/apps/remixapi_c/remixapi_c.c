/* FUSE Relight RL-6.2 test app: a D3D9 application that drives Relight's C APIs from d3d9.dll (plan §6.2 remixapi_c;
 * our own app, written against the documented contract of the Remix API 0.6 header, which is MIT).
 *
 *   1. d3d9.dll is loaded from the app directory; remixapi_InitializeLibrary and fuse_relight_GetFrameRecord /
 *      fuse_relight_GetOption must be exported (Relight's DLL, not a system d3d9).
 *   2. Version negotiation: 0.7 and 1.6 are refused, 0.6.5 gives the interface.
 *   3. Startup, dxvk_CreateD3D9 (this DLL's Direct3DCreate9Ex), CreateDeviceEx, dxvk_RegisterD3D9Device.
 *   4. A material (opaque + emissive), a quad mesh (HardcodedVertex), a sphere light; SetConfigVariable once.
 *   5. kFrames frames: the device clears (D3D9 work in the same frame), SetupCamera, two DrawInstance, one
 *      DrawLightInstance, Present (ends the API frame and presents the registered device through PresentEx).
 *      After every Present the frame record (fuse_relight_GetFrameRecord) must show the expected scene: frame index,
 *      2 instances / surfaces, 2 RL-1.7 instances (created in frame 0 only), 1 authored light + 4 emissive triangles
 *      in the light set, a valid camera, and the same scene / light digests in every frame (static scene).
 *   6. The option written by SetConfigVariable reads back through fuse_relight_GetOption.
 * Writes remixapi_c.json (the checks and the last record) into the working directory. Exit 0 when every check holds,
 * 1 otherwise, 77 when d3d9.dll cannot create a device (no Vulkan).
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>

#include <stdio.h>
#include <string.h>

#include <fuse/relight/api/fuse_relight_api.h>
#include <fuse/relight/api/remixapi_compat.h>

enum { kFrames = 8, kWidth = 128, kHeight = 96 };

static int g_failures = 0;
static char g_log[16384];
static size_t g_logLen = 0;

static void check(int ok, const char* what) {
    int n;
    if (!ok) {
        ++g_failures;
        printf("FAIL: %s\n", what);
    }
    n = snprintf(g_log + g_logLen, sizeof(g_log) - g_logLen, "%s    {\"check\": \"%s\", \"ok\": %s}",
                 g_logLen == 0 ? "" : ",\n", what, ok ? "true" : "false");
    if (n > 0 && (size_t)n < sizeof(g_log) - g_logLen) {
        g_logLen += (size_t)n;
    }
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void quad(remixapi_HardcodedVertex* v) {
    static const float p[4][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    static const float t[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    int i;
    memset(v, 0, sizeof(remixapi_HardcodedVertex) * 4);
    for (i = 0; i < 4; ++i) {
        memcpy(v[i].position, p[i], sizeof(p[i]));
        v[i].normal[2] = -1.f;
        memcpy(v[i].texcoord, t[i], sizeof(t[i]));
        v[i].color = 0xFFFFFFFFu;
    }
}

int main(void) {
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    HMODULE dll;
    PFN_remixapi_InitializeLibrary initLib;
    PFN_fuse_relight_GetFrameRecord getRecord;
    PFN_fuse_relight_GetOption getOption;
    remixapi_InitializeLibraryInfo ii;
    remixapi_Interface api;
    remixapi_ErrorCode e;
    WNDCLASSA wc;
    HWND hwnd;
    RECT rc = {0, 0, kWidth, kHeight};
    remixapi_StartupInfo si;
    IDirect3D9Ex* d3d = NULL;
    IDirect3DDevice9Ex* dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    remixapi_MaterialInfoOpaqueEXT opaque;
    remixapi_MaterialInfo mi;
    remixapi_MaterialHandle material = NULL;
    remixapi_HardcodedVertex verts[4];
    remixapi_MeshInfoSurfaceTriangles surf;
    remixapi_MeshInfo meshInfo;
    remixapi_MeshHandle mesh = NULL;
    remixapi_LightInfoSphereEXT sphere;
    remixapi_LightInfo li;
    remixapi_LightHandle light = NULL;
    fuse_relight_FrameRecord first, rec;
    char value[64] = {0};
    int frame;
    FILE* f;
    HRESULT hr;

    dll = LoadLibraryA("d3d9.dll");
    if (dll == NULL) {
        printf("FAIL: cannot load d3d9.dll\n");
        return 1;
    }
    initLib = (PFN_remixapi_InitializeLibrary)(void*)GetProcAddress(dll, "remixapi_InitializeLibrary");
    getRecord = (PFN_fuse_relight_GetFrameRecord)(void*)GetProcAddress(dll, "fuse_relight_GetFrameRecord");
    getOption = (PFN_fuse_relight_GetOption)(void*)GetProcAddress(dll, "fuse_relight_GetOption");
    check(initLib != NULL && getRecord != NULL && getOption != NULL, "d3d9.dll exports the Relight C APIs");
    if (initLib == NULL || getRecord == NULL || getOption == NULL) {
        return 1;
    }

    memset(&ii, 0, sizeof(ii));
    memset(&api, 0, sizeof(api));
    ii.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    ii.version = REMIXAPI_VERSION_MAKE(0, 7, 0);
    check(initLib(&ii, &api) == REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION, "0.7 is refused");
    ii.version = REMIXAPI_VERSION_MAKE(1, 6, 0);
    check(initLib(&ii, &api) == REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION, "1.6 is refused");
    ii.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    e = initLib(&ii, &api);
    check(e == REMIXAPI_ERROR_CODE_SUCCESS && api.Present != NULL && api.SetCameraMediumMaterial != NULL,
          "0.6.5 gives the full interface");
    if (e != REMIXAPI_ERROR_CODE_SUCCESS) {
        return 1;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "fuse_relight_remixapi_c";
    RegisterClassA(&wc);
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA(wc.lpszClassName, "remixapi_c", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left,
                         rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);

    memset(&si, 0, sizeof(si));
    si.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    si.hwnd = (remixapi_HWND)hwnd;
    check(api.Startup(&si) == REMIXAPI_ERROR_CODE_SUCCESS, "Startup");

    e = api.dxvk_CreateD3D9(0, &d3d);
    check(e == REMIXAPI_ERROR_CODE_SUCCESS && d3d != NULL, "dxvk_CreateD3D9 gives this DLL's IDirect3D9Ex");
    if (d3d == NULL) {
        return 1;
    }
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9Ex_CreateDeviceEx(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, NULL, &dev);
    if (FAILED(hr)) {
        printf("SKIP: CreateDeviceEx failed (0x%08lx): no Vulkan device\n", (unsigned long)hr);
        IDirect3D9Ex_Release(d3d);
        return 77;
    }
    check(api.dxvk_RegisterD3D9Device(dev) == REMIXAPI_ERROR_CODE_SUCCESS, "dxvk_RegisterD3D9Device");

    memset(&opaque, 0, sizeof(opaque));
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant.x = 0.8f;
    opaque.albedoConstant.y = 0.4f;
    opaque.albedoConstant.z = 0.2f;
    opaque.opacityConstant = 1.f;
    opaque.roughnessConstant = 0.5f;
    opaque.alphaTestType = 7;
    memset(&mi, 0, sizeof(mi));
    mi.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    mi.pNext = &opaque;
    mi.hash = 0x1111;
    mi.emissiveIntensity = 3.f;
    mi.emissiveColorConstant.x = mi.emissiveColorConstant.y = mi.emissiveColorConstant.z = 1.f;
    mi.filterMode = 1;
    mi.wrapModeU = mi.wrapModeV = 1;
    check(api.CreateMaterial(&mi, &material) == REMIXAPI_ERROR_CODE_SUCCESS && material != NULL, "CreateMaterial");

    quad(verts);
    memset(&surf, 0, sizeof(surf));
    surf.vertices_values = verts;
    surf.vertices_count = 4;
    surf.indices_values = indices;
    surf.indices_count = 6;
    surf.material = material;
    memset(&meshInfo, 0, sizeof(meshInfo));
    meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    meshInfo.hash = 0x2222;
    meshInfo.surfaces_values = &surf;
    meshInfo.surfaces_count = 1;
    check(api.CreateMesh(&meshInfo, &mesh) == REMIXAPI_ERROR_CODE_SUCCESS && mesh != NULL, "CreateMesh");

    memset(&sphere, 0, sizeof(sphere));
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position.y = 3.f;
    sphere.radius = 0.25f;
    sphere.volumetricRadianceScale = 1.f;
    memset(&li, 0, sizeof(li));
    li.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    li.pNext = &sphere;
    li.hash = 0x3333;
    li.radiance.x = li.radiance.y = li.radiance.z = 10.f;
    check(api.CreateLight(&li, &light) == REMIXAPI_ERROR_CODE_SUCCESS && light != NULL, "CreateLight");
    check(api.SetConfigVariable("rtx.numFramesToKeepInstances", "3") == REMIXAPI_ERROR_CODE_SUCCESS,
          "SetConfigVariable");
    check(api.SetConfigVariable("rtx.notAnOptionOfAnyKind", "1") == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "SetConfigVariable refuses unknown keys");

    memset(&first, 0, sizeof(first));
    for (frame = 0; frame < kFrames; ++frame) {
        remixapi_CameraInfoParameterizedEXT cp;
        remixapi_CameraInfo ci;
        remixapi_InstanceInfo inst;
        remixapi_PresentInfo pi;
        char what[128];
        int ok;

        IDirect3DDevice9Ex_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(32, 64, 96), 1.f, 0);
        IDirect3DDevice9Ex_BeginScene(dev);
        IDirect3DDevice9Ex_EndScene(dev);

        memset(&cp, 0, sizeof(cp));
        cp.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
        cp.position.z = -5.f;
        cp.forward.z = 1.f;
        cp.up.y = 1.f;
        cp.right.x = 1.f;
        cp.fovYInDegrees = 60.f;
        cp.aspect = (float)kWidth / (float)kHeight;
        cp.nearPlane = 0.1f;
        cp.farPlane = 100.f;
        memset(&ci, 0, sizeof(ci));
        ci.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
        ci.pNext = &cp;
        ci.type = REMIXAPI_CAMERA_TYPE_WORLD;
        ok = api.SetupCamera(&ci) == REMIXAPI_ERROR_CODE_SUCCESS;

        memset(&inst, 0, sizeof(inst));
        inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        inst.mesh = mesh;
        inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.f;
        inst.transform.matrix[0][3] = -1.5f;
        ok = ok && api.DrawInstance(&inst) == REMIXAPI_ERROR_CODE_SUCCESS;
        inst.transform.matrix[0][3] = 1.5f;
        inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE;
        ok = ok && api.DrawInstance(&inst) == REMIXAPI_ERROR_CODE_SUCCESS;
        ok = ok && api.DrawLightInstance(light) == REMIXAPI_ERROR_CODE_SUCCESS;
        memset(&pi, 0, sizeof(pi));
        pi.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
        e = api.Present(&pi);
        ok = ok && e == REMIXAPI_ERROR_CODE_SUCCESS;

        memset(&rec, 0, sizeof(rec));
        rec.structSize = sizeof(rec);
        ok = ok && getRecord(&rec) == FUSE_RELIGHT_SUCCESS;
        ok = ok && rec.frame == (uint64_t)frame && rec.instancesDrawn == 2 && rec.surfacesDrawn == 2 &&
             rec.sceneInstances == 2 && rec.createdInstances == (frame == 0 ? 2u : 0u) && rec.authoredLights == 1 &&
             rec.emissiveTriangles == 4 && rec.lights == 5 && rec.cameraValid == 1 && rec.liveMeshes == 1 &&
             rec.liveMaterials == 1 && rec.liveLights == 1 && rec.optionWrites == (frame == 0 ? 1u : 0u);
        if (frame == 0) {
            first = rec;
        } else {
            ok = ok && rec.sceneDigest == first.sceneDigest && rec.lightDigest == first.lightDigest;
        }
        snprintf(what, sizeof(what), "frame %d: Present (0x%x) and the frame record", frame, (unsigned)e);
        check(ok, what);
    }
    check(getOption("rtx.numFramesToKeepInstances", value, sizeof(value)) == FUSE_RELIGHT_SUCCESS &&
              strcmp(value, "3") == 0,
          "the SetConfigVariable value reads back");

    check(api.DestroyMesh(mesh) == REMIXAPI_ERROR_CODE_SUCCESS, "DestroyMesh");
    check(api.DestroyMaterial(material) == REMIXAPI_ERROR_CODE_SUCCESS, "DestroyMaterial");
    check(api.DestroyLight(light) == REMIXAPI_ERROR_CODE_SUCCESS, "DestroyLight");
    check(api.Shutdown() == REMIXAPI_ERROR_CODE_SUCCESS, "Shutdown");
    IDirect3DDevice9Ex_Release(dev);
    IDirect3D9Ex_Release(d3d);
    DestroyWindow(hwnd);

    f = fopen("remixapi_c.json", "w");
    if (f != NULL) {
        fprintf(f,
                "{\n  \"app\": \"remixapi_c\",\n  \"ok\": %s,\n  \"frames\": %d,\n  \"last_record\": {\"frame\": %llu, "
                "\"instances_drawn\": %u, \"surfaces_drawn\": %u, \"scene_instances\": %u, \"lights\": %u, "
                "\"authored_lights\": %u, \"emissive_triangles\": %u, \"scene_digest\": \"%016llx\", "
                "\"light_digest\": \"%016llx\"},\n  \"checks\": [\n%s\n  ]\n}\n",
                g_failures == 0 ? "true" : "false", kFrames, (unsigned long long)rec.frame, rec.instancesDrawn,
                rec.surfacesDrawn, rec.sceneInstances, rec.lights, rec.authoredLights, rec.emissiveTriangles,
                (unsigned long long)rec.sceneDigest, (unsigned long long)rec.lightDigest, g_log);
        fclose(f);
    }
    printf("remixapi_c: %s (%d failure(s))\n", g_failures == 0 ? "pass" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
