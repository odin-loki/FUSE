// fuse_app Wayland client: see wayland_internal.hpp. Everything Wayland is reached through dlopen'd entry points
// and the hand-written protocol tables below (the wire format only needs names, signatures and opcodes).
#include "wayland_internal.hpp"

#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
#include "native_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <string_view>
#include <thread>
#include <type_traits>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fuse::app::wl {
using namespace fuse::ui;

// ---- libwayland-client ABI (wayland-util.h / wayland-client-core.h) -------------------------------------------------
struct wl_interface;
struct wl_message {
    const char* name;
    const char* signature;
    const wl_interface** types;
};
struct wl_interface {
    const char* name;
    int version;
    int method_count;
    const wl_message* methods;
    int event_count;
    const wl_message* events;
};
struct wl_array {
    std::size_t size, alloc;
    void* data;
};
struct wl_proxy;
struct wl_display;
using Fn = void (*)(void);

namespace {
struct Lib {
    void* so = nullptr;
    wl_display* (*display_connect)(const char*) = nullptr;
    void (*display_disconnect)(wl_display*) = nullptr;
    int (*display_get_fd)(wl_display*) = nullptr;
    int (*display_dispatch_pending)(wl_display*) = nullptr;
    int (*display_roundtrip)(wl_display*) = nullptr;
    int (*display_flush)(wl_display*) = nullptr;
    int (*display_prepare_read)(wl_display*) = nullptr;
    int (*display_read_events)(wl_display*) = nullptr;
    void (*display_cancel_read)(wl_display*) = nullptr;
    int (*display_get_error)(wl_display*) = nullptr;
    wl_proxy* (*proxy_marshal_constructor)(wl_proxy*, uint32_t, const wl_interface*, ...) = nullptr;
    wl_proxy* (*proxy_marshal_constructor_versioned)(wl_proxy*, uint32_t, const wl_interface*, uint32_t, ...) = nullptr;
    void (*proxy_marshal)(wl_proxy*, uint32_t, ...) = nullptr;
    int (*proxy_add_listener)(wl_proxy*, Fn*, void*) = nullptr;
    void (*proxy_destroy)(wl_proxy*) = nullptr;
    uint32_t (*proxy_get_version)(wl_proxy*) = nullptr;
    // core protocol interfaces (data symbols of libwayland-client)
    const wl_interface *registry = nullptr, *compositor = nullptr, *surface = nullptr, *shm = nullptr, *shmPool = nullptr,
                       *buffer = nullptr, *seat = nullptr, *pointer = nullptr, *keyboard = nullptr, *touch = nullptr,
                       *output = nullptr, *callback = nullptr, *ddManager = nullptr, *dataDevice = nullptr, *dataSource = nullptr,
                       *dataOffer = nullptr;
    bool load(std::string& err) {
        if (so) return true;
        so = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) so = dlopen("libwayland-client.so", RTLD_NOW | RTLD_LOCAL);
        if (!so) {
            err = "libwayland-client.so.0 not found";
            return false;
        }
        bool all = true;
        auto get = [&](auto& f, const char* n) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(so, n));
            all = all && f;
        };
        get(display_connect, "wl_display_connect");
        get(display_disconnect, "wl_display_disconnect");
        get(display_get_fd, "wl_display_get_fd");
        get(display_dispatch_pending, "wl_display_dispatch_pending");
        get(display_roundtrip, "wl_display_roundtrip");
        get(display_flush, "wl_display_flush");
        get(display_prepare_read, "wl_display_prepare_read");
        get(display_read_events, "wl_display_read_events");
        get(display_cancel_read, "wl_display_cancel_read");
        get(display_get_error, "wl_display_get_error");
        get(proxy_marshal_constructor, "wl_proxy_marshal_constructor");
        get(proxy_marshal_constructor_versioned, "wl_proxy_marshal_constructor_versioned");
        get(proxy_marshal, "wl_proxy_marshal");
        get(proxy_add_listener, "wl_proxy_add_listener");
        get(proxy_destroy, "wl_proxy_destroy");
        get(proxy_get_version, "wl_proxy_get_version");
        if (!all) {
            err = "libwayland-client.so.0 lacks an entry point";
            dlclose(so);
            so = nullptr;
            return false;
        }
        auto data = [&](const wl_interface*& i, const char* n) {
            i = static_cast<const wl_interface*>(dlsym(so, n));
            all = all && i;
        };
        data(registry, "wl_registry_interface");
        data(compositor, "wl_compositor_interface");
        data(surface, "wl_surface_interface");
        data(shm, "wl_shm_interface");
        data(shmPool, "wl_shm_pool_interface");
        data(buffer, "wl_buffer_interface");
        data(seat, "wl_seat_interface");
        data(pointer, "wl_pointer_interface");
        data(keyboard, "wl_keyboard_interface");
        data(touch, "wl_touch_interface");
        data(output, "wl_output_interface");
        data(callback, "wl_callback_interface");
        data(ddManager, "wl_data_device_manager_interface");
        data(dataDevice, "wl_data_device_interface");
        data(dataSource, "wl_data_source_interface");
        data(dataOffer, "wl_data_offer_interface");
        if (!all) {
            err = "libwayland-client.so.0 lacks a core interface";
            dlclose(so);
            so = nullptr;
        }
        return all;
    }
};
Lib L;

// ---- xkbcommon (optional: without it a US evdev table maps keys, no text) -----------------------------------------
struct xkb_context;
struct xkb_keymap;
struct xkb_state;
struct RuleNames { const char *rules, *model, *layout, *variant, *options; };
struct Xkb {
    void* so = nullptr;
    xkb_context* (*context_new)(int) = nullptr;
    void (*context_unref)(xkb_context*) = nullptr;
    xkb_keymap* (*keymap_new_from_string)(xkb_context*, const char*, int, int) = nullptr;
    xkb_keymap* (*keymap_new_from_names)(xkb_context*, const RuleNames*, int) = nullptr;
    void (*keymap_unref)(xkb_keymap*) = nullptr;
    int (*keymap_key_repeats)(xkb_keymap*, uint32_t) = nullptr;
    xkb_state* (*state_new)(xkb_keymap*) = nullptr;
    void (*state_unref)(xkb_state*) = nullptr;
    int (*state_update_mask)(xkb_state*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    int (*state_update_key)(xkb_state*, uint32_t, int) = nullptr;
    uint32_t (*state_key_get_one_sym)(xkb_state*, uint32_t) = nullptr;
    int (*state_key_get_utf8)(xkb_state*, uint32_t, char*, std::size_t) = nullptr;
    int (*state_mod_name_is_active)(xkb_state*, const char*, int) = nullptr;
    bool tried = false, ok = false;
    bool load() {
        if (tried) return ok;
        tried = true;
        so = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) so = dlopen("libxkbcommon.so", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& f, const char* n) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(so, n));
            all = all && f;
        };
        get(context_new, "xkb_context_new");
        get(context_unref, "xkb_context_unref");
        get(keymap_new_from_string, "xkb_keymap_new_from_string");
        get(keymap_new_from_names, "xkb_keymap_new_from_names");
        get(keymap_unref, "xkb_keymap_unref");
        get(keymap_key_repeats, "xkb_keymap_key_repeats");
        get(state_new, "xkb_state_new");
        get(state_unref, "xkb_state_unref");
        get(state_update_mask, "xkb_state_update_mask");
        get(state_update_key, "xkb_state_update_key");
        get(state_key_get_one_sym, "xkb_state_key_get_one_sym");
        get(state_key_get_utf8, "xkb_state_key_get_utf8");
        get(state_mod_name_is_active, "xkb_state_mod_name_is_active");
        ok = all;
        return ok;
    }
};
Xkb X;
constexpr int kXkbStateModsEffective = 8;

// ---- libwayland-cursor (optional: the compositor's default cursor without it) --------------------------------------
struct wl_cursor_theme;
struct wl_cursor_image { uint32_t width, height, hotspot_x, hotspot_y, delay; };
struct wl_cursor { unsigned image_count; wl_cursor_image** images; char* name; };
struct Cur {
    void* so = nullptr;
    wl_cursor_theme* (*theme_load)(const char*, int, wl_proxy*) = nullptr;
    void (*theme_destroy)(wl_cursor_theme*) = nullptr;
    wl_cursor* (*theme_get_cursor)(wl_cursor_theme*, const char*) = nullptr;
    wl_proxy* (*image_get_buffer)(wl_cursor_image*) = nullptr;
    bool tried = false, ok = false;
    bool load() {
        if (tried) return ok;
        tried = true;
        so = dlopen("libwayland-cursor.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) so = dlopen("libwayland-cursor.so", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& f, const char* n) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(so, n));
            all = all && f;
        };
        get(theme_load, "wl_cursor_theme_load");
        get(theme_destroy, "wl_cursor_theme_destroy");
        get(theme_get_cursor, "wl_cursor_theme_get_cursor");
        get(image_get_buffer, "wl_cursor_image_get_buffer");
        ok = all;
        return ok;
    }
};
Cur C;

// ---- protocol tables: xdg-shell (stable, version 1 messages), xdg-decoration-unstable-v1, viewporter,
// fractional-scale-v1, cursor-shape-v1, text-input-unstable-v3, xdg-toplevel-icon-v1, primary-selection-unstable-v1 ----
// Written by hand from the protocol descriptions of wayland-protocols (interface names, message names, signatures and
// opcode order; no generated code and no protocol XML is in this file). The descriptions are:
//   xdg-shell                      Copyright (c) 2008-2013 Kristian Hogsberg, 2013 Rafael Antognolli, 2013 Jasper St. Pierre,
//                                  2010-2013 Intel Corporation, 2015-2017 Samsung Electronics Co., Ltd, 2015-2017 Red Hat Inc.
//   xdg-decoration-unstable-v1     Copyright (c) 2018 Simon Ser
//   viewporter                     Copyright (c) 2013-2016 Collabora, Ltd.
//   fractional-scale-v1            Copyright (c) 2022 Kenny Levinsen
//   cursor-shape-v1                Copyright 2018 The Chromium Authors, 2023 Simon Ser
//   xdg-toplevel-icon-v1           Copyright (c) 2023-2024 Matthias Klumpp, 2024 David Edmundson
//   primary-selection-unstable-v1  Copyright (c) 2015, 2016 Red Hat
//     Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//     documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//     the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
//     to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above
//     copyright notice and this permission notice (including the next paragraph) shall be included in all copies or
//     substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//     IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//     NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//     LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//     SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//   text-input-unstable-v3         Copyright (c) 2012, 2013 Intel Corporation, 2015, 2016 Jan Arne Petersen,
//                                  2017, 2018 Red Hat, Inc., 2018 Purism SPC
//     Permission to use, copy, modify, distribute, and sell this software and its documentation for any purpose is
//     hereby granted without fee, provided that the above copyright notice appear in all copies and that both that
//     copyright notice and this permission notice appear in supporting documentation, and that the name of the
//     copyright holders not be used in advertising or publicity pertaining to distribution of the software without
//     specific, written prior permission. The copyright holders make no representations about the suitability of this
//     software for any purpose. It is provided "as is" without express or implied warranty. THE COPYRIGHT HOLDERS
//     DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
//     FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
//     ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
//     OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
// (THIRD_PARTY.md lists them.)
extern const wl_interface xdgWmBase, xdgPositioner, xdgSurface, xdgToplevel, xdgPopup, decoManager, decoToplevel, viewporterI,
    viewportI, fracMgrI, fracI, shapeMgrI, shapeDevI, tabletToolI, tiMgrI, tiI, iconMgrI, iconI, psMgrI, psDevI, psSrcI, psOfferI;
const wl_interface* tNone[8] = {};
const wl_interface* tCreatePositioner[] = {&xdgPositioner};
const wl_interface* tGetXdgSurface[] = {&xdgSurface, nullptr /* wl_surface, set at load */};
const wl_interface* tGetToplevel[] = {&xdgToplevel};
const wl_interface* tGetPopup[] = {&xdgPopup, &xdgSurface, &xdgPositioner};
const wl_interface* tSetParent[] = {&xdgToplevel};
const wl_interface* tSeatFirst[] = {nullptr /* wl_seat */, nullptr, nullptr, nullptr};
const wl_interface* tOutput[] = {nullptr /* wl_output */};
const wl_interface* tGetDeco[] = {&decoToplevel, &xdgToplevel};
const wl_interface* tSurfaceArg[] = {nullptr /* wl_surface */};
const wl_interface* tGetViewport[] = {&viewportI, nullptr /* wl_surface */};
const wl_interface* tGetFrac[] = {&fracI, nullptr /* wl_surface */};
const wl_interface* tGetShapePointer[] = {&shapeDevI, nullptr /* wl_pointer */};
const wl_interface* tGetShapeTablet[] = {&shapeDevI, &tabletToolI};
const wl_interface* tGetTextInput[] = {&tiI, nullptr /* wl_seat */};
const wl_interface* tCreateIcon[] = {&iconI};
const wl_interface* tSetIcon[] = {&xdgToplevel, &iconI};
const wl_interface* tAddBuffer[] = {nullptr /* wl_buffer */, nullptr};
const wl_interface* tPsCreateSource[] = {&psSrcI};
const wl_interface* tPsGetDevice[] = {&psDevI, nullptr /* wl_seat */};
const wl_interface* tPsSetSelection[] = {&psSrcI, nullptr};
const wl_interface* tPsOffer[] = {&psOfferI};

const wl_message wmBaseRequests[] = {
    {"destroy", "", tNone}, {"create_positioner", "n", tCreatePositioner}, {"get_xdg_surface", "no", tGetXdgSurface}, {"pong", "u", tNone}};
const wl_message wmBaseEvents[] = {{"ping", "u", tNone}};
const wl_message positionerRequests[] = {
    {"destroy", "", tNone}, {"set_size", "ii", tNone}, {"set_anchor_rect", "iiii", tNone}, {"set_anchor", "u", tNone},
    {"set_gravity", "u", tNone}, {"set_constraint_adjustment", "u", tNone}, {"set_offset", "ii", tNone}};
const wl_message xdgSurfaceRequests[] = {
    {"destroy", "", tNone}, {"get_toplevel", "n", tGetToplevel}, {"get_popup", "n?oo", tGetPopup},
    {"set_window_geometry", "iiii", tNone}, {"ack_configure", "u", tNone}};
const wl_message xdgSurfaceEvents[] = {{"configure", "u", tNone}};
const wl_message toplevelRequests[] = {
    {"destroy", "", tNone}, {"set_parent", "?o", tSetParent}, {"set_title", "s", tNone}, {"set_app_id", "s", tNone},
    {"show_window_menu", "ouii", tSeatFirst}, {"move", "ou", tSeatFirst}, {"resize", "ouu", tSeatFirst},
    {"set_max_size", "ii", tNone}, {"set_min_size", "ii", tNone}, {"set_maximized", "", tNone}, {"unset_maximized", "", tNone},
    {"set_fullscreen", "?o", tOutput}, {"unset_fullscreen", "", tNone}, {"set_minimized", "", tNone}};
const wl_message toplevelEvents[] = {{"configure", "iia", tNone}, {"close", "", tNone}};
const wl_message popupRequests[] = {{"destroy", "", tNone}, {"grab", "ou", tSeatFirst}};
const wl_message popupEvents[] = {{"configure", "iiii", tNone}, {"popup_done", "", tNone}};
const wl_message decoManagerRequests[] = {{"destroy", "", tNone}, {"get_toplevel_decoration", "no", tGetDeco}};
const wl_message decoToplevelRequests[] = {{"destroy", "", tNone}, {"set_mode", "u", tNone}, {"unset_mode", "", tNone}};
const wl_message decoToplevelEvents[] = {{"configure", "u", tNone}};
const wl_message viewporterRequests[] = {{"destroy", "", tNone}, {"get_viewport", "no", tGetViewport}};
const wl_message viewportRequests[] = {{"destroy", "", tNone}, {"set_source", "ffff", tNone}, {"set_destination", "ii", tNone}};
const wl_message fracMgrRequests[] = {{"destroy", "", tNone}, {"get_fractional_scale", "no", tGetFrac}};
const wl_message fracRequests[] = {{"destroy", "", tNone}};
const wl_message fracEvents[] = {{"preferred_scale", "u", tNone}};
const wl_message shapeMgrRequests[] = {
    {"destroy", "", tNone}, {"get_pointer", "no", tGetShapePointer}, {"get_tablet_tool_v2", "no", tGetShapeTablet}};
const wl_message shapeDevRequests[] = {{"destroy", "", tNone}, {"set_shape", "uu", tNone}};
const wl_message tiMgrRequests[] = {{"destroy", "", tNone}, {"get_text_input", "no", tGetTextInput}};
const wl_message tiRequests[] = {{"destroy", "", tNone}, {"enable", "", tNone}, {"disable", "", tNone},
                                 {"set_surrounding_text", "sii", tNone}, {"set_text_change_cause", "u", tNone},
                                 {"set_content_type", "uu", tNone}, {"set_cursor_rectangle", "iiii", tNone}, {"commit", "", tNone}};
const wl_message tiEvents[] = {{"enter", "o", tSurfaceArg}, {"leave", "o", tSurfaceArg}, {"preedit_string", "?sii", tNone},
                               {"commit_string", "?s", tNone}, {"delete_surrounding_text", "uu", tNone}, {"done", "u", tNone}};
const wl_message iconMgrRequests[] = {{"destroy", "", tNone}, {"create_icon", "n", tCreateIcon}, {"set_icon", "o?o", tSetIcon}};
const wl_message iconMgrEvents[] = {{"icon_size", "i", tNone}, {"done", "", tNone}};
const wl_message iconRequests[] = {{"destroy", "", tNone}, {"set_name", "s", tNone}, {"add_buffer", "oi", tAddBuffer}};
const wl_message psMgrRequests[] = {{"create_source", "n", tPsCreateSource}, {"get_device", "no", tPsGetDevice}, {"destroy", "", tNone}};
const wl_message psDevRequests[] = {{"set_selection", "?ou", tPsSetSelection}, {"destroy", "", tNone}};
const wl_message psDevEvents[] = {{"data_offer", "n", tPsOffer}, {"selection", "?o", tPsOffer}};
const wl_message psOfferRequests[] = {{"receive", "sh", tNone}, {"destroy", "", tNone}};
const wl_message psOfferEvents[] = {{"offer", "s", tNone}};
const wl_message psSrcRequests[] = {{"offer", "s", tNone}, {"destroy", "", tNone}};
const wl_message psSrcEvents[] = {{"send", "sh", tNone}, {"cancelled", "", tNone}};

template <std::size_t N>
constexpr int count(const wl_message (&)[N]) { return int(N); }
const wl_interface xdgWmBase{"xdg_wm_base", 1, count(wmBaseRequests), wmBaseRequests, count(wmBaseEvents), wmBaseEvents};
const wl_interface xdgPositioner{"xdg_positioner", 1, count(positionerRequests), positionerRequests, 0, nullptr};
const wl_interface xdgSurface{"xdg_surface", 1, count(xdgSurfaceRequests), xdgSurfaceRequests, count(xdgSurfaceEvents), xdgSurfaceEvents};
const wl_interface xdgToplevel{"xdg_toplevel", 1, count(toplevelRequests), toplevelRequests, count(toplevelEvents), toplevelEvents};
const wl_interface xdgPopup{"xdg_popup", 1, count(popupRequests), popupRequests, count(popupEvents), popupEvents};
const wl_interface decoManager{"zxdg_decoration_manager_v1", 1, count(decoManagerRequests), decoManagerRequests, 0, nullptr};
const wl_interface decoToplevel{"zxdg_toplevel_decoration_v1", 1, count(decoToplevelRequests), decoToplevelRequests,
                                          count(decoToplevelEvents), decoToplevelEvents};
const wl_interface viewporterI{"wp_viewporter", 1, count(viewporterRequests), viewporterRequests, 0, nullptr};
const wl_interface viewportI{"wp_viewport", 1, count(viewportRequests), viewportRequests, 0, nullptr};
const wl_interface fracMgrI{"wp_fractional_scale_manager_v1", 1, count(fracMgrRequests), fracMgrRequests, 0, nullptr};
const wl_interface fracI{"wp_fractional_scale_v1", 1, count(fracRequests), fracRequests, count(fracEvents), fracEvents};
const wl_interface shapeMgrI{"wp_cursor_shape_manager_v1", 1, count(shapeMgrRequests), shapeMgrRequests, 0, nullptr};
const wl_interface shapeDevI{"wp_cursor_shape_device_v1", 1, count(shapeDevRequests), shapeDevRequests, 0, nullptr};
const wl_interface tabletToolI{"zwp_tablet_tool_v2", 1, 0, nullptr, 0, nullptr}; // only named (never created here)
const wl_interface tiMgrI{"zwp_text_input_manager_v3", 1, count(tiMgrRequests), tiMgrRequests, 0, nullptr};
const wl_interface tiI{"zwp_text_input_v3", 1, count(tiRequests), tiRequests, count(tiEvents), tiEvents};
const wl_interface iconMgrI{"xdg_toplevel_icon_manager_v1", 1, count(iconMgrRequests), iconMgrRequests, count(iconMgrEvents), iconMgrEvents};
const wl_interface iconI{"xdg_toplevel_icon_v1", 1, count(iconRequests), iconRequests, 0, nullptr};
const wl_interface psMgrI{"zwp_primary_selection_device_manager_v1", 1, count(psMgrRequests), psMgrRequests, 0, nullptr};
const wl_interface psDevI{"zwp_primary_selection_device_v1", 1, count(psDevRequests), psDevRequests, count(psDevEvents), psDevEvents};
const wl_interface psSrcI{"zwp_primary_selection_source_v1", 1, count(psSrcRequests), psSrcRequests, count(psSrcEvents), psSrcEvents};
const wl_interface psOfferI{"zwp_primary_selection_offer_v1", 1, count(psOfferRequests), psOfferRequests, count(psOfferEvents), psOfferEvents};
void fillProtocolTypes() {
    tGetXdgSurface[1] = L.surface;
    tSeatFirst[0] = L.seat;
    tOutput[0] = L.output;
    tSurfaceArg[0] = L.surface;
    tGetViewport[1] = L.surface;
    tGetFrac[1] = L.surface;
    tGetShapePointer[1] = L.pointer;
    tGetTextInput[1] = L.seat;
    tAddBuffer[0] = L.buffer;
    tPsGetDevice[1] = L.seat;
}

// opcodes
enum : uint32_t {
    kDisplayGetRegistry = 1, kRegistryBind = 0, kCompositorCreateSurface = 0,
    kSurfaceDestroy = 0, kSurfaceAttach = 1, kSurfaceDamage = 2, kSurfaceFrame = 3, kSurfaceCommit = 6, kSurfaceSetBufferScale = 8, kSurfaceDamageBuffer = 9,
    kShmCreatePool = 0, kPoolCreateBuffer = 0, kPoolDestroy = 1, kBufferDestroy = 0,
    kSeatGetPointer = 0, kSeatGetKeyboard = 1, kSeatGetTouch = 2, kPointerSetCursor = 0, kPointerRelease = 1, kKeyboardRelease = 0,
    kWmBaseDestroy = 0, kWmBaseCreatePositioner = 1, kWmBaseGetXdgSurface = 2, kWmBasePong = 3,
    kPosDestroy = 0, kPosSetSize = 1, kPosSetAnchorRect = 2, kPosSetAnchor = 3, kPosSetGravity = 4, kPosSetConstraint = 5, kPosSetOffset = 6,
    kXdgSurfaceDestroy = 0, kXdgSurfaceGetToplevel = 1, kXdgSurfaceGetPopup = 2, kXdgSurfaceAck = 4,
    kToplevelDestroy = 0, kToplevelSetParent = 1, kToplevelSetTitle = 2, kToplevelSetAppId = 3, kToplevelMove = 5, kToplevelResize = 6,
    kToplevelSetMaxSize = 7, kToplevelSetMinSize = 8, kToplevelSetMaximized = 9, kToplevelUnsetMaximized = 10,
    kToplevelSetFullscreen = 11, kToplevelUnsetFullscreen = 12, kToplevelSetMinimized = 13,
    kPopupDestroy = 0, kPopupGrab = 1,
    kDecoManagerDestroy = 0, kDecoManagerGet = 1, kDecoDestroy = 0, kDecoSetMode = 1,
    kViewporterDestroy = 0, kViewporterGet = 1, kViewportDestroy = 0, kViewportSetDestination = 2,
    kFracMgrDestroy = 0, kFracMgrGet = 1, kFracDestroy = 0,
    kShapeMgrDestroy = 0, kShapeMgrGetPointer = 1, kShapeDevDestroy = 0, kShapeDevSetShape = 1,
    kTiMgrDestroy = 0, kTiMgrGet = 1, kTiDestroy = 0, kTiEnable = 1, kTiDisable = 2, kTiSetContentType = 5, kTiSetCursorRect = 6, kTiCommit = 7,
    kIconMgrDestroy = 0, kIconMgrCreate = 1, kIconMgrSetIcon = 2, kIconDestroy = 0, kIconAddBuffer = 2,
    // wl_data_device_manager / wl_data_device / wl_data_source / wl_data_offer (core)
    kDdmCreateSource = 0, kDdmGetDevice = 1, kDdSetSelection = 1, kDdRelease = 2,
    kSrcOffer = 0, kSrcDestroy = 1, kOfferAccept = 0, kOfferReceive = 1, kOfferDestroy = 2, kOfferFinish = 3, kOfferSetActions = 4,
    // zwp_primary_selection_*_v1
    kPsmCreateSource = 0, kPsmGetDevice = 1, kPsmDestroy = 2, kPsdSetSelection = 0, kPsdDestroy = 1,
    kPsoReceive = 0, kPsoDestroy = 1, kPssOffer = 0, kPssDestroy = 1,
};
constexpr uint32_t kShmArgb8888 = 0, kShmXrgb8888 = 1;
constexpr uint32_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112;

template <class F>
Fn fn(F f) { return reinterpret_cast<Fn>(f); }
void destroyRequest(wl_proxy* p, uint32_t op) {
    if (!p) return;
    L.proxy_marshal(p, op);
    L.proxy_destroy(p);
}
f32 fixedToFloat(int32_t v) { return f32(v) / 256.f; }

// US layout evdev -> X keysym (no xkbcommon)
unsigned long evdevSym(unsigned code, bool shift) {
    static const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const unsigned first[] = {2, 16, 30, 44};
    for (int r = 0; r < 4; ++r) {
        const unsigned n = unsigned(std::strlen(rows[r]));
        if (code >= first[r] && code < first[r] + n) {
            const char c = rows[r][code - first[r]];
            return shift && c >= 'a' && c <= 'z' ? unsigned(c - 32) : unsigned(c);
        }
    }
    switch (code) {
    case 1: return 0xff1b; case 14: return 0xff08; case 15: return 0xff09; case 28: return 0xff0d; case 57: return ' ';
    case 26: return '['; case 27: return ']'; case 102: return 0xff50; case 103: return 0xff52; case 104: return 0xff55;
    case 105: return 0xff51; case 106: return 0xff53; case 107: return 0xff57; case 108: return 0xff54; case 109: return 0xff56;
    case 110: return 0xff63; case 111: return 0xffff; case 56: return 0xffe9; case 100: return 0xffea;
    case 87: return 0xffc8; case 88: return 0xffc9;
    default: break;
    }
    if (code >= 59 && code <= 68) return 0xffbe + (code - 59); // F1..F10
    return 0;
}
} // namespace

// ---- backend state ----------------------------------------------------------------------------------------------------
struct Output {
    wl_proxy* proxy = nullptr;
    uint32_t name = 0;
    int x = 0, y = 0, w = 0, h = 0, refresh = 0, scale = 1;
    std::string make, model;
};
struct ShmBuf {
    wl_proxy* buffer = nullptr;
    void* data = nullptr;
    std::size_t bytes = 0;
    int w = 0, h = 0;
    bool busy = false;
};
/// A wl_surface with rotating wl_shm buffers (main windows' software path, popups, tool windows).
struct ShmSurface {
    ShmBuf bufs[3];
    int last = -1;
};
inline constexpr Cursor kNoCursor = Cursor(255);
struct Window {
    Backend* b = nullptr;
    AppWindow* win = nullptr;
    wl_proxy *surface = nullptr, *xdgSurf = nullptr, *toplevel = nullptr, *deco = nullptr, *viewport = nullptr, *frac = nullptr;
    bool configured = false;
    int pendingW = 0, pendingH = 0;
    bool pendingMax = false, pendingFull = false, pendingActive = false;
    bool maximized = false, fullscreen = false, activated = false, kbdFocus = false, serverDeco = false;
    bool vulkanCommitted = false;
    bool restoring = false, unmaximizeAfterCommit = false; ///< setMode(Normal) asked: see xdgSurfaceConfigure
    unsigned long long acks = 0, throttled = 0;
    int scale = 1, committedScale = 1; ///< integer buffer scale (1 when a wp_viewport scales the buffer)
    f32 fracScale = 0.f;              ///< wp_fractional_scale_v1 preferred scale (0: none yet)
    std::vector<Output*> entered;
    ShmSurface shm;
    wl_proxy* frameCb = nullptr;      ///< wl_surface.frame of the last wl_shm commit, not yet done
    std::chrono::steady_clock::time_point frameSent{};
    bool frameDeferred = false;       ///< a present was skipped while waiting: redraw when the callback arrives
    Cursor cursor = kNoCursor;        ///< shape last sent while the pointer is inside (kNoCursor: none)
    Cursor cursorWanted = Cursor::Arrow;
    unsigned shapeSent = 0;
    Size minSize{}, maxSize{};        ///< logical; re-sent when the surface is mapped again
    Size floatingSize{};              ///< the size before the compositor maximised / fullscreened the window
    std::string title;
    std::vector<ShmBuf> iconBufs;
    std::vector<int> iconWidths;
    bool dragOver = false;
};
struct Child {
    Backend* b = nullptr;
    Window* owner = nullptr;
    Child* parent = nullptr;
    bool popup = true;
    wl_proxy *surface = nullptr, *xdgSurf = nullptr, *role = nullptr, *viewport = nullptr;
    Vec2 origin{};       ///< context position of the surface's top-left (input mapping)
    Size size{};         ///< logical
    Rect placed{};       ///< popup configure: relative to the parent
    bool configured = false, done = false, grabbed = false;
    ShmSurface shm;
    int bufScale = 1;
    bool pressed = false;
    uint32_t pressSerial = 0;
    Vec2 pressPos{};     ///< context position of the press
};
/// A wl_data_offer / zwp_primary_selection_offer_v1 and the MIME types it announced.
struct Offer {
    wl_proxy* proxy = nullptr;
    bool primary = false;
    std::vector<std::string> mimes;
};
struct Selection {
    wl_proxy* source = nullptr; ///< ours while we own the selection
    std::vector<std::string> mimes;
    SelectionProvider fn = nullptr;
    void* user = nullptr;
    Offer* offer = nullptr;     ///< the current selection (anyone's)
};
struct Backend {
    wl_display* dpy = nullptr;
    wl_proxy *registry = nullptr, *compositor = nullptr, *shm = nullptr, *seat = nullptr, *wmBase = nullptr, *decoMgr = nullptr;
    wl_proxy *viewporter = nullptr, *fracMgr = nullptr, *shapeMgr = nullptr, *shapeDev = nullptr, *tiMgr = nullptr, *ti = nullptr,
             *iconMgr = nullptr, *ddMgr = nullptr, *dd = nullptr, *psMgr = nullptr, *psDev = nullptr;
    uint32_t compositorVersion = 1, seatName = 0, ddVersion = 1;
    std::vector<std::string> globals;
    std::vector<Output*> outputs;
    wl_proxy *pointer = nullptr, *keyboard = nullptr, *touch = nullptr;
    uint32_t seatVersion = 1;
    std::vector<Window*> windows;
    std::vector<Child*> children;
    Window *ptrFocus = nullptr, *kbdFocus = nullptr;
    Child* ptrChild = nullptr;
    uint32_t ptrSerial = 0, enterSerial = 0, pressSerial = 0, inputSerial = 0;
    Vec2 ptrPos{};
    // one wl_pointer frame of scrolling (delivered at wl_pointer.frame; at once for a seat older than version 5)
    f32 axisValue[2] = {}, axisSteps[2] = {};
    bool axisHasSteps[2] = {}, axisPending = false, axisContinuous = false;
    // wl_touch: the surface each touch point went down on (ids are the compositor's)
    struct TouchPoint { int32_t id; Window* win; Child* child; Vec2 pos; };
    std::vector<TouchPoint> touches;
    f32 fixedScale = 0.f; ///< NativeOptions::scale, else $FUSE_UI_SCALE (0: follow the outputs)
    bool broken = false;  ///< the connection failed
    xkb_context* xctx = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* xstate = nullptr;
    u8 mods = 0;
    bool shiftRaw = false; // no-xkb fallback
    int32_t repeatRate = 25, repeatDelay = 600;
    uint32_t repeatKey = 0;
    std::chrono::steady_clock::time_point repeatAt{};
    wl_proxy* cursorSurface = nullptr;
    wl_cursor_theme* theme = nullptr;
    int themeScale = 0;
    bool readPrepared = false;
    std::vector<int> iconSizesPref; ///< xdg_toplevel_icon_manager_v1 icon_size
    void (*dismiss)(void* user, Window* owner) = nullptr;
    void* dismissUser = nullptr;
    // clipboard / primary selection / drag and drop
    std::vector<Offer*> offers; ///< announced, not yet destroyed
    Selection sel[2];           ///< 0 clipboard, 1 primary
    Offer* dragOffer = nullptr;
    Window* dragWin = nullptr;
    Vec2 dragPos{};
    std::string dragMime;
    DropHandler dropFn = nullptr;
    void* dropUser = nullptr;
    // text-input-v3
    Window* tiFocus = nullptr;
    Window* tiEnabledFor = nullptr;
    Rect tiCaret{-1.f, -1.f, 0.f, 0.f};
    std::string tiPreedit, tiCommit;
    int32_t tiCursor = 0;
    bool tiHasPreedit = false, tiHasCommit = false, tiShown = false;
};

namespace {
Backend* gBackend = nullptr; // output events reach windows through it (one backend per process)

Window* windowOf(Backend* b, wl_proxy* surface) {
    for (Window* w : b->windows)
        if (w->surface == surface) return w;
    return nullptr;
}
Child* childOf(Backend* b, wl_proxy* surface) {
    for (Child* c : b->children)
        if (c->surface == surface) return c;
    return nullptr;
}
/// The window a surface belongs to: itself, or the owner of a popup / tool window (keyboard focus while grabbed).
Window* ownerOf(Backend* b, wl_proxy* surface) {
    if (Window* w = windowOf(b, surface)) return w;
    if (Child* c = childOf(b, surface)) return c->owner;
    return nullptr;
}
int outputScale(const Window& w) {
    int s = 0;
    for (const Output* o : w.entered) s = std::max(s, o->scale);
    if (s == 0)
        for (const Output* o : w.b->outputs) s = std::max(s, o->scale); // not on an output yet: the largest known
    return std::max(1, s);
}
/// The UI scale `s`: any value when a wp_viewport maps the buffer onto the logical size (fractional scaling), else an
/// integer buffer scale.
void applyScale(Window& w, f32 s) {
    if (w.viewport) {
        s = std::clamp(s, 0.25f, 8.f);
        w.scale = 1;
    } else {
        s = f32(std::clamp(int(std::lround(s)), 1, 8));
        w.scale = int(s);
    }
    if (s == w.win->scale()) return;
    w.win->setScale(s); // keeps the logical size; the next buffer is logical * s pixels
    w.win->requestRedraw();
}
void rescale(Window& w) {
    if (w.b->fixedScale > 0.f) applyScale(w, w.b->fixedScale);
    else if (w.frac && w.fracScale > 0.f) applyScale(w, w.fracScale);
    else applyScale(w, f32(outputScale(w)));
}
void sendBufferScale(Backend* b, wl_proxy* surface, int scale, int& committed) {
    if (committed == scale || b->compositorVersion < 3) return;
    L.proxy_marshal(surface, kSurfaceSetBufferScale, int32_t(scale));
    committed = scale;
}
void sendViewport(Window& w) {
    if (!w.viewport) return;
    const Size s = w.win->size();
    L.proxy_marshal(w.viewport, kViewportSetDestination, int32_t(std::max(1L, std::lround(s.w))), int32_t(std::max(1L, std::lround(s.h))));
}

void freeBuf(ShmBuf& s) {
    if (s.buffer) destroyRequest(s.buffer, kBufferDestroy);
    if (s.data) ::munmap(s.data, s.bytes);
    s = {};
}
void bufferRelease(void* data, wl_proxy*) { static_cast<ShmBuf*>(data)->busy = false; }
const Fn* bufferListener() {
    static const Fn l[] = {fn(&bufferRelease)};
    return l;
}
/// An unlinked shared-memory file (the pool the compositor maps).
int anonFile() {
#if defined(__linux__)
    return ::memfd_create("fuse-wl-shm", MFD_CLOEXEC);
#elif defined(SHM_ANON)
    return ::shm_open(SHM_ANON, O_RDWR | O_CREAT | O_CLOEXEC, 0600); // FreeBSD
#else
    char name[] = "/tmp/fuse-wl-shm-XXXXXX";
    const int fd = ::mkstemp(name);
    if (fd >= 0) {
        ::unlink(name);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
#endif
}
bool makeBuf(Backend* b, ShmBuf& s, int w, int h, uint32_t format = kShmXrgb8888, bool listen = true) {
    const std::size_t stride = std::size_t(w) * 4, bytes = stride * std::size_t(h);
    const int fd = anonFile();
    if (fd < 0) return false;
    if (::ftruncate(fd, off_t(bytes)) != 0) {
        ::close(fd);
        return false;
    }
    void* data = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    wl_proxy* pool = L.proxy_marshal_constructor(b->shm, kShmCreatePool, L.shmPool, nullptr, fd, int32_t(bytes));
    wl_proxy* buf = pool ? L.proxy_marshal_constructor(pool, kPoolCreateBuffer, L.buffer, nullptr, int32_t(0), int32_t(w), int32_t(h),
                                                       int32_t(stride), format)
                         : nullptr;
    destroyRequest(pool, kPoolDestroy); // the buffer keeps the memory
    ::close(fd);
    if (!buf) {
        ::munmap(data, bytes);
        return false;
    }
    s.buffer = buf;
    s.data = data;
    s.bytes = bytes;
    s.w = w;
    s.h = h;
    s.busy = false;
    if (listen) L.proxy_add_listener(buf, const_cast<Fn*>(bufferListener()), &s);
    return true;
}
/// The CPU raster `img` (padded to `bw` x `bh` with its edge colour) into a released buffer of `s`, attached and damaged
/// (not committed). False when every buffer is still busy after a round trip.
bool attachImage(Backend* b, wl_proxy* surface, ShmSurface& s, const Image& img, int bw, int bh) {
    auto released = [&] {
        for (int i = 0; i < 3; ++i)
            if (s.bufs[i].buffer && !s.bufs[i].busy && s.bufs[i].w == bw && s.bufs[i].h == bh) return i;
        return -1;
    };
    int pick = released();
    for (int i = 0; i < 3 && pick < 0; ++i)
        if (!s.bufs[i].buffer || !s.bufs[i].busy) {
            freeBuf(s.bufs[i]);
            if (makeBuf(b, s.bufs[i], bw, bh)) pick = i;
            break;
        }
    if (pick < 0) { // all busy: wait for a release
        L.display_roundtrip(b->dpy);
        pick = released();
        if (pick < 0) return false;
    }
    ShmBuf& buf = s.bufs[std::size_t(pick)];
    u32* dst = static_cast<u32*>(buf.data);
    for (int y = 0; y < bh; ++y) { // 0xAABBGGRR -> XRGB8888 0x00RRGGBB
        const int sy = std::min(y, img.height - 1);
        for (int x = 0; x < bw; ++x) {
            const u32 q = img.pixels[std::size_t(sy) * std::size_t(img.width) + std::size_t(std::min(x, img.width - 1))];
            dst[std::size_t(y) * std::size_t(bw) + std::size_t(x)] = 0xFF000000u | ((q & 0xFFu) << 16) | (q & 0xFF00u) | ((q >> 16) & 0xFFu);
        }
    }
    L.proxy_marshal(surface, kSurfaceAttach, buf.buffer, int32_t(0), int32_t(0));
    if (b->compositorVersion >= 4) L.proxy_marshal(surface, kSurfaceDamageBuffer, int32_t(0), int32_t(0), int32_t(bw), int32_t(bh));
    else L.proxy_marshal(surface, kSurfaceDamage, int32_t(0), int32_t(0), int32_t(INT_MAX), int32_t(INT_MAX));
    buf.busy = true;
    s.last = pick;
    return true;
}
bool readShm(const ShmSurface& s, Image& out, int w = 0, int h = 0) {
    if (s.last < 0) return false;
    const ShmBuf& b = s.bufs[std::size_t(s.last)];
    if (w <= 0 || w > b.w) w = b.w;
    if (h <= 0 || h > b.h) h = b.h;
    out.resize(w, h);
    const u32* src = static_cast<const u32*>(b.data);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u32 p = src[std::size_t(y) * std::size_t(b.w) + std::size_t(x)];
            out.pixels[std::size_t(y) * std::size_t(w) + std::size_t(x)] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
        }
    return true;
}

// ---- listeners ----
void outputGeometry(void* data, wl_proxy*, int32_t x, int32_t y, int32_t, int32_t, int32_t, const char* make, const char* model, int32_t) {
    auto* o = static_cast<Output*>(data);
    o->x = x;
    o->y = y;
    o->make = make ? make : "";
    o->model = model ? model : "";
}
void outputMode(void* data, wl_proxy*, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    auto* o = static_cast<Output*>(data);
    if (flags & 1u) { // current
        o->w = w;
        o->h = h;
        o->refresh = refresh;
    }
}
void outputDone(void*, wl_proxy*) {
    if (gBackend)
        for (Window* w : gBackend->windows) rescale(*w);
}
void outputScaleEv(void* data, wl_proxy*, int32_t factor) { static_cast<Output*>(data)->scale = std::max(1, int(factor)); }
void outputName(void*, wl_proxy*, const char*) {}
const Fn* outputListener() {
    static const Fn l[] = {fn(&outputGeometry), fn(&outputMode), fn(&outputDone), fn(&outputScaleEv), fn(&outputName), fn(&outputName)};
    return l;
}

void shmFormat(void*, wl_proxy*, uint32_t) {}
void pingEv(void*, wl_proxy* base, uint32_t serial) { L.proxy_marshal(base, kWmBasePong, serial); }

// surface enter / leave: output scale (integer path)
void surfaceEnter(void* data, wl_proxy*, wl_proxy* output) {
    auto* w = static_cast<Window*>(data);
    for (Output* o : w->b->outputs)
        if (o->proxy == output && std::find(w->entered.begin(), w->entered.end(), o) == w->entered.end()) w->entered.push_back(o);
    rescale(*w);
}
void surfaceLeave(void* data, wl_proxy*, wl_proxy* output) {
    auto* w = static_cast<Window*>(data);
    std::erase_if(w->entered, [&](const Output* o) { return o->proxy == output; });
    rescale(*w);
}
void surfaceObj(void*, wl_proxy*, wl_proxy*) {}
void surfaceInt(void*, wl_proxy*, int32_t) {}
void surfaceUint(void*, wl_proxy*, uint32_t) {}
void fracPreferred(void* data, wl_proxy*, uint32_t scale120) {
    auto* w = static_cast<Window*>(data);
    w->fracScale = f32(scale120) / 120.f;
    rescale(*w);
}

void xdgSurfaceConfigure(void* data, wl_proxy* xs, uint32_t serial) {
    auto* w = static_cast<Window*>(data);
    L.proxy_marshal(xs, kXdgSurfaceAck, serial);
    ++w->acks;
    AppWindow& a = *w->win;
    const bool wasFloating = !w->maximized && !w->fullscreen, floating = !w->pendingMax && !w->pendingFull;
    if (wasFloating && !floating) w->floatingSize = a.size(); // what to come back to
    if (w->pendingW > 0 && w->pendingH > 0) {
        const Size s = a.size();
        if (u32(w->pendingW) != u32(s.w) || u32(w->pendingH) != u32(s.h)) a.resize(u32(w->pendingW), u32(w->pendingH));
    } else if (floating && !wasFloating && w->floatingSize.w > 0.f) {
        // restored with "pick a size" (0 x 0): the size from before maximise / fullscreen
        a.resize(u32(w->floatingSize.w), u32(w->floatingSize.h));
    }
    w->maximized = w->pendingMax;
    w->fullscreen = w->pendingFull;
    w->activated = w->pendingActive;
    w->configured = true;
    if (w->restoring && !w->fullscreen) {
        // weston returns from fullscreen to the maximised state it had before and ignores an unset_maximized sent
        // before that state is current: ask again once the buffer for this configure is committed
        w->unmaximizeAfterCommit = w->maximized;
        w->restoring = false;
    }
    a.notifyMode(w->fullscreen ? WindowMode::Fullscreen : w->maximized ? WindowMode::Maximized : WindowMode::Normal);
    a.notifyActive(w->activated);
    a.requestRedraw();
}
void toplevelConfigure(void* data, wl_proxy*, int32_t width, int32_t height, wl_array* states) {
    auto* w = static_cast<Window*>(data);
    w->pendingW = width;
    w->pendingH = height;
    w->pendingMax = w->pendingFull = w->pendingActive = false;
    if (states)
        for (std::size_t i = 0; i + 4 <= states->size; i += 4) {
            uint32_t s = 0;
            std::memcpy(&s, static_cast<const char*>(states->data) + i, 4);
            if (s == 1) w->pendingMax = true;         // XDG_TOPLEVEL_STATE_MAXIMIZED
            else if (s == 2) w->pendingFull = true;   // FULLSCREEN
            else if (s == 4) w->pendingActive = true; // ACTIVATED
        }
}
void toplevelClose(void* data, wl_proxy*) { static_cast<Window*>(data)->win->tryClose(); } // asks the close guard
void decoConfigure(void* data, wl_proxy*, uint32_t mode) { static_cast<Window*>(data)->serverDeco = mode == 2; }
void frameDone(void* data, wl_proxy* cb, uint32_t) {
    auto* w = static_cast<Window*>(data);
    L.proxy_destroy(cb);
    if (w->frameCb == cb) w->frameCb = nullptr;
    if (w->frameDeferred) { // a frame was held back for this: draw it now
        w->frameDeferred = false;
        w->win->requestRedraw();
    }
}
const Fn* frameListener() {
    static const Fn l[] = {fn(&frameDone)};
    return l;
}

// ---- child surfaces ----
void childSurfaceConfigure(void* data, wl_proxy* xs, uint32_t serial) {
    auto* c = static_cast<Child*>(data);
    L.proxy_marshal(xs, kXdgSurfaceAck, serial);
    c->configured = true;
}
void popupConfigure(void* data, wl_proxy*, int32_t x, int32_t y, int32_t w, int32_t h) {
    static_cast<Child*>(data)->placed = {f32(x), f32(y), f32(w), f32(h)};
}
void popupDone(void* data, wl_proxy*) {
    auto* c = static_cast<Child*>(data);
    if (c->done) return;
    c->done = true;
    if (c->b->dismiss) c->b->dismiss(c->b->dismissUser, c->owner);
}
void childToplevelConfigure(void*, wl_proxy*, int32_t, int32_t, wl_array*) {}
void childToplevelClose(void*, wl_proxy*) {} // tool windows close from their own UI

// ---- pointer ----
Vec2 localPos(int32_t sx, int32_t sy) { return {fixedToFloat(sx), fixedToFloat(sy)}; } // surface-local = UI units
void applyCursor(Backend* b, Window* w);
void applyCursorShape(Backend* b, Cursor c, Window* w);
Vec2 childPos(const Child& c, Vec2 local) { return {c.origin.x + local.x, c.origin.y + local.y}; }
void pointerEnter(void* data, wl_proxy*, uint32_t serial, wl_proxy* surface, int32_t sx, int32_t sy) {
    auto* b = static_cast<Backend*>(data);
    b->ptrFocus = windowOf(b, surface);
    b->ptrChild = b->ptrFocus ? nullptr : childOf(b, surface);
    b->enterSerial = serial;
    b->ptrPos = localPos(sx, sy);
    if (Window* w = b->ptrFocus) {
        w->cursor = kNoCursor; // the compositor forgets ours on leave
        w->win->ui().setModifiers(b->mods);
        w->win->ui().pointerMove(b->ptrPos);
        applyCursor(b, w);
    } else if (Child* c = b->ptrChild) {
        c->owner->cursor = kNoCursor;
        c->owner->win->ui().setModifiers(b->mods);
        c->owner->win->ui().pointerMove(childPos(*c, b->ptrPos));
        const Cursor cs = c->owner->win->ui().cursor();
        applyCursorShape(b, cs == Cursor::Auto ? Cursor::Arrow : cs, c->owner);
        c->owner->win->requestRedraw();
    }
}
void pointerLeave(void* data, wl_proxy*, uint32_t, wl_proxy* surface) {
    auto* b = static_cast<Backend*>(data);
    if (Child* c = childOf(b, surface); c && c->pressed) { // a compositor move took the pointer: end the UI's press
        c->pressed = false;
        c->owner->win->ui().pointerUp(c->pressPos);
        c->owner->win->requestRedraw();
    }
    b->ptrFocus = nullptr;
    b->ptrChild = nullptr;
}
void pointerMotion(void* data, wl_proxy*, uint32_t, int32_t sx, int32_t sy) {
    auto* b = static_cast<Backend*>(data);
    b->ptrPos = localPos(sx, sy);
    if (Window* w = b->ptrFocus) {
        w->win->ui().setModifiers(b->mods);
        w->win->ui().pointerMove(b->ptrPos);
        applyCursor(b, w);
    } else if (Child* c = b->ptrChild) {
        UiContext& ui = c->owner->win->ui();
        ui.setModifiers(b->mods);
        ui.pointerMove(childPos(*c, b->ptrPos));
        const Cursor cs = ui.cursor();
        applyCursorShape(b, cs == Cursor::Auto ? Cursor::Arrow : cs, c->owner);
        c->owner->win->requestRedraw();
    }
}
MouseButton mouseButton(uint32_t button) {
    return button == kBtnRight ? MouseButton::Right : button == kBtnMiddle ? MouseButton::Middle : MouseButton::Left;
}
void deliverButton(Backend* b, Window* w, uint32_t button, bool down) {
    UiContext& ui = w->win->ui();
    ui.setModifiers(b->mods);
    if (down) ui.pointerDown(b->ptrPos, mouseButton(button));
    else ui.pointerUp(b->ptrPos, mouseButton(button));
    applyCursor(b, w);
}
void deliverChildButton(Backend* b, Child* c, uint32_t serial, uint32_t button, bool down) {
    UiContext& ui = c->owner->win->ui();
    ui.setModifiers(b->mods);
    const Vec2 pos = childPos(*c, b->ptrPos);
    if (down && button == kBtnLeft) {
        c->pressed = true;
        c->pressSerial = serial;
        c->pressPos = pos;
    } else if (!down) {
        c->pressed = false;
    }
    if (down) ui.pointerDown(pos, mouseButton(button));
    else ui.pointerUp(pos, mouseButton(button));
    c->owner->win->requestRedraw();
}
void pointerButton(void* data, wl_proxy*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
    auto* b = static_cast<Backend*>(data);
    b->ptrSerial = serial;
    b->inputSerial = serial;
    if (state == 1) b->pressSerial = serial;
    if (Window* w = b->ptrFocus) deliverButton(b, w, button, state == 1);
    else if (Child* c = b->ptrChild) deliverChildButton(b, c, serial, button, state == 1);
}
/// Delivers the scrolling gathered since the last wl_pointer.frame as one wheel event.
void flushAxis(Backend* b) {
    if (!b->axisPending) return;
    const Vec2 d = wheelNotches({b->axisValue[1], b->axisValue[0]}, {b->axisSteps[1], b->axisSteps[0]}, b->axisHasSteps[1],
                                b->axisHasSteps[0], b->axisContinuous);
    b->axisValue[0] = b->axisValue[1] = b->axisSteps[0] = b->axisSteps[1] = 0.f;
    b->axisHasSteps[0] = b->axisHasSteps[1] = b->axisPending = b->axisContinuous = false;
    if (d.x == 0.f && d.y == 0.f) return;
    if (Window* w = b->ptrFocus) {
        w->win->ui().setModifiers(b->mods);
        w->win->ui().wheel(b->ptrPos, d);
    } else if (Child* c = b->ptrChild) {
        c->owner->win->ui().setModifiers(b->mods);
        c->owner->win->ui().wheel(childPos(*c, b->ptrPos), d);
        c->owner->win->requestRedraw();
    }
}
void pointerAxis(void* data, wl_proxy*, uint32_t, uint32_t axis, int32_t value) {
    auto* b = static_cast<Backend*>(data);
    b->axisValue[axis & 1u] += fixedToFloat(value); // axis 0 vertical (positive = down), 1 horizontal (positive = right)
    b->axisPending = true;
    if (b->seatVersion < 5) flushAxis(b); // no frame events before wl_seat 5
}
void pointerFrame(void* data, wl_proxy*) { flushAxis(static_cast<Backend*>(data)); }
void pointerAxisSource(void* data, wl_proxy*, uint32_t source) {
    static_cast<Backend*>(data)->axisContinuous = source == 1 || source == 2; // finger / continuous: pixel scrolling
}
void pointerAxisStop(void*, wl_proxy*, uint32_t, uint32_t) {}
void pointerAxisDiscrete(void* data, wl_proxy*, uint32_t axis, int32_t steps) {
    auto* b = static_cast<Backend*>(data);
    b->axisSteps[axis & 1u] += f32(steps);
    b->axisHasSteps[axis & 1u] = true;
    b->axisPending = true;
}
const Fn* pointerListener() {
    static const Fn l[] = {fn(&pointerEnter), fn(&pointerLeave), fn(&pointerMotion), fn(&pointerButton), fn(&pointerAxis),
                           fn(&pointerFrame), fn(&pointerAxisSource), fn(&pointerAxisStop), fn(&pointerAxisDiscrete)};
    return l;
}

// ---- keyboard ----
void keyboardKeymap(void* data, wl_proxy*, uint32_t format, int32_t fd, uint32_t size) {
    auto* b = static_cast<Backend*>(data);
    if (format != 1 /*xkb_v1*/ || !X.load()) {
        ::close(fd);
        return;
    }
    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) return;
    if (!b->xctx) b->xctx = X.context_new(0);
    xkb_keymap* km = b->xctx ? X.keymap_new_from_string(b->xctx, static_cast<const char*>(map), 1, 0) : nullptr;
    ::munmap(map, size);
    if (!km) return;
    if (b->xstate) X.state_unref(b->xstate);
    if (b->keymap) X.keymap_unref(b->keymap);
    b->keymap = km;
    b->xstate = X.state_new(km);
}
void keyboardEnter(void* data, wl_proxy*, uint32_t serial, wl_proxy* surface, wl_array*) {
    auto* b = static_cast<Backend*>(data);
    b->kbdFocus = ownerOf(b, surface); // a grabbing popup takes the keyboard: its keys drive the owner's menu
    b->inputSerial = serial;
    if (b->kbdFocus) b->kbdFocus->kbdFocus = true;
}
void keyboardLeave(void* data, wl_proxy*, uint32_t, wl_proxy* surface) {
    auto* b = static_cast<Backend*>(data);
    Window* w = ownerOf(b, surface);
    if (w && w != b->kbdFocus) return; // a popup gave the keyboard back to its owner already
    if (b->kbdFocus) b->kbdFocus->kbdFocus = false;
    b->kbdFocus = nullptr;
    b->repeatKey = 0;
}
void deliverKey(Backend* b, Window* w, uint32_t key, bool down, [[maybe_unused]] bool repeat) {
    UiContext& ui = w->win->ui();
    unsigned long sym = 0;
    char text[32] = {};
    if (b->xstate) {
        sym = X.state_key_get_one_sym(b->xstate, key + 8);
        if (down) X.state_key_get_utf8(b->xstate, key + 8, text, sizeof text);
    } else {
        if (key == 42 || key == 54) b->shiftRaw = down;
        sym = evdevSym(key, b->shiftRaw);
        if (down && sym >= 0x20 && sym < 0x7f) text[0] = char(sym);
    }
    const Key k = detail::keysymKey(sym);
    if (k != Key::Other) { // printable keys too (viewport WASD); their text arrives separately
        if (down) {
            if (!ui.keyDown(k, b->mods)) w->win->menuShortcut(k, b->mods); // unconsumed: a menu shortcut (AppWindow::setMenu)
        } else {
            ui.keyUp(k, b->mods);
        }
    }
    if (down && !(b->mods & (kModCtrl | kModAlt))) {
        std::string s(text);
        std::erase_if(s, [](char ch) { return u8(ch) < 0x20 || u8(ch) == 0x7F; });
        if (!s.empty()) ui.textInput(s);
    }
    w->win->requestRedraw();
}
void keyboardKey(void* data, wl_proxy*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
    auto* b = static_cast<Backend*>(data);
    b->inputSerial = serial;
    if (state == 1) b->pressSerial = serial;
    Window* w = b->kbdFocus;
    if (!w) return;
    const bool down = state == 1;
    deliverKey(b, w, key, down, false);
    if (down && b->repeatRate > 0 && (!b->keymap || X.keymap_key_repeats(b->keymap, key + 8))) {
        b->repeatKey = key;
        b->repeatAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(b->repeatDelay);
    } else if (!down && key == b->repeatKey) {
        b->repeatKey = 0;
    }
}
void keyboardModifiers(void* data, wl_proxy*, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
    auto* b = static_cast<Backend*>(data);
    b->inputSerial = serial;
    if (!b->xstate) return;
    X.state_update_mask(b->xstate, dep, lat, lock, 0, 0, group);
    auto on = [&](const char* n) { return X.state_mod_name_is_active(b->xstate, n, kXkbStateModsEffective) > 0; };
    b->mods = u8((on("Shift") ? kModShift : 0) | (on("Control") ? kModCtrl : 0) | (on("Mod1") ? kModAlt : 0) | (on("Mod4") ? kModSuper : 0));
}
void keyboardRepeat(void* data, wl_proxy*, int32_t rate, int32_t delay) {
    auto* b = static_cast<Backend*>(data);
    b->repeatRate = rate;
    b->repeatDelay = delay;
}
const Fn* keyboardListener() {
    static const Fn l[] = {fn(&keyboardKeymap), fn(&keyboardEnter), fn(&keyboardLeave), fn(&keyboardKey), fn(&keyboardModifiers),
                           fn(&keyboardRepeat)};
    return l;
}

// ---- touch (UiContext's touch path: tap = click, drag = pan + fling, two fingers = pinch / rotate) ----
Backend::TouchPoint* touchPoint(Backend* b, int32_t id) {
    for (auto& t : b->touches)
        if (t.id == id) return &t;
    return nullptr;
}
AppWindow& touchWindow(const Backend::TouchPoint& t) { return *(t.win ? t.win : t.child->owner)->win; }
void touchDown(void* data, wl_proxy*, uint32_t serial, uint32_t time, wl_proxy* surface, int32_t id, int32_t x, int32_t y) {
    auto* b = static_cast<Backend*>(data);
    b->inputSerial = b->pressSerial = serial;
    Window* w = windowOf(b, surface);
    Child* c = w ? nullptr : childOf(b, surface);
    if (!w && !c) return;
    std::erase_if(b->touches, [&](const Backend::TouchPoint& t) { return t.id == id; });
    const Vec2 p = c ? childPos(*c, localPos(x, y)) : localPos(x, y);
    b->touches.push_back({id, w, c, p});
    AppWindow& a = touchWindow(b->touches.back());
    a.ui().setModifiers(b->mods);
    a.ui().touchDown(u32(id), p, double(time) / 1000.0);
}
void touchUp(void* data, wl_proxy*, uint32_t serial, uint32_t time, int32_t id) {
    auto* b = static_cast<Backend*>(data);
    b->inputSerial = serial;
    if (Backend::TouchPoint* t = touchPoint(b, id)) {
        touchWindow(*t).ui().touchUp(u32(id), t->pos, double(time) / 1000.0);
        std::erase_if(b->touches, [&](const Backend::TouchPoint& o) { return o.id == id; });
    }
}
void touchMotion(void* data, wl_proxy*, uint32_t time, int32_t id, int32_t x, int32_t y) {
    auto* b = static_cast<Backend*>(data);
    if (Backend::TouchPoint* t = touchPoint(b, id)) {
        t->pos = t->child ? childPos(*t->child, localPos(x, y)) : localPos(x, y);
        touchWindow(*t).ui().touchMove(u32(id), t->pos, double(time) / 1000.0);
    }
}
void touchFrame(void*, wl_proxy*) {}
void touchCancelEv(void* data, wl_proxy*) { // the compositor took the gesture: no click, no fling
    auto* b = static_cast<Backend*>(data);
    for (const auto& t : b->touches) touchWindow(t).ui().touchCancel(); // also forgets the pointer ids
    b->touches.clear();
}
const Fn* touchListener() {
    static const Fn l[] = {fn(&touchDown), fn(&touchUp), fn(&touchMotion), fn(&touchFrame), fn(&touchCancelEv)};
    return l;
}

// ---- data offers (clipboard, primary selection, drag and drop) ----
Offer* findOffer(Backend* b, wl_proxy* p) {
    for (Offer* o : b->offers)
        if (o->proxy == p) return o;
    return nullptr;
}
void destroyOffer(Backend* b, Offer* o) {
    if (!o) return;
    for (Selection& s : b->sel)
        if (s.offer == o) s.offer = nullptr;
    if (b->dragOffer == o) b->dragOffer = nullptr;
    destroyRequest(o->proxy, o->primary ? kPsoDestroy : kOfferDestroy);
    std::erase(b->offers, o);
    delete o;
}
void offerMime(void* data, wl_proxy*, const char* mime) {
    auto* o = static_cast<Offer*>(data);
    if (mime && std::find(o->mimes.begin(), o->mimes.end(), mime) == o->mimes.end()) o->mimes.emplace_back(mime);
}
void offerUint(void*, wl_proxy*, uint32_t) {}
void newOffer(Backend* b, wl_proxy* id, bool primary) {
    auto* o = new Offer{};
    o->proxy = id;
    o->primary = primary;
    b->offers.push_back(o);
    static const Fn dl[] = {fn(&offerMime), fn(&offerUint), fn(&offerUint)};
    static const Fn pl[] = {fn(&offerMime)};
    L.proxy_add_listener(id, const_cast<Fn*>(primary ? pl : dl), o);
}
void setSelectionOffer(Backend* b, int which, wl_proxy* offer) {
    Selection& s = b->sel[which];
    Offer* o = offer ? findOffer(b, offer) : nullptr;
    if (s.offer && s.offer != o && s.offer != b->dragOffer) destroyOffer(b, s.offer);
    s.offer = o;
}
/// Reads everything from `fd` (closed afterwards) for up to `ms` milliseconds.
bool readAll(int fd, std::vector<unsigned char>& out, int ms) {
    out.clear();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    bool ok = false;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, int(std::min<long long>(left, 100))) <= 0) continue;
        unsigned char buf[16384];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) out.insert(out.end(), buf, buf + n);
        else if (n == 0) { ok = true; break; } // the source closed its end: complete
        else if (errno != EINTR && errno != EAGAIN) break;
    }
    ::close(fd);
    return ok;
}
/// Asks the offer for `mime` and reads it (another client writes it; our own offers are never read this way).
bool receiveOffer(Backend* b, Offer* o, const std::string& mime, std::vector<unsigned char>& out, int ms) {
    int fds[2];
    if (!o || ::pipe2(fds, O_CLOEXEC) != 0) return false;
    L.proxy_marshal(o->proxy, o->primary ? kPsoReceive : kOfferReceive, mime.c_str(), fds[1]);
    ::close(fds[1]);
    L.display_flush(b->dpy);
    return readAll(fds[0], out, ms);
}
void ddDataOffer(void* data, wl_proxy*, wl_proxy* id) { newOffer(static_cast<Backend*>(data), id, false); }
void ddSelection(void* data, wl_proxy*, wl_proxy* offer) { setSelectionOffer(static_cast<Backend*>(data), 0, offer); }
std::string dropMime(const Offer* o) {
    for (const char* want : {"text/uri-list", "text/plain;charset=utf-8", "UTF8_STRING", "text/plain"})
        if (o && std::find(o->mimes.begin(), o->mimes.end(), want) != o->mimes.end()) return want;
    return {};
}
void ddEnter(void* data, wl_proxy*, uint32_t serial, wl_proxy* surface, int32_t x, int32_t y, wl_proxy* offer) {
    auto* b = static_cast<Backend*>(data);
    if (b->dragOffer && b->dragOffer->proxy != offer && !(b->sel[0].offer == b->dragOffer)) destroyOffer(b, b->dragOffer);
    b->dragOffer = offer ? findOffer(b, offer) : nullptr;
    b->dragWin = windowOf(b, surface);
    b->dragPos = localPos(x, y);
    b->dragMime = dropMime(b->dragOffer);
    if (b->dragWin) b->dragWin->dragOver = true;
    if (!b->dragOffer) return;
    L.proxy_marshal(b->dragOffer->proxy, kOfferAccept, serial, b->dragMime.empty() || !b->dragWin ? nullptr : b->dragMime.c_str());
    if (b->ddVersion >= 3) L.proxy_marshal(b->dragOffer->proxy, kOfferSetActions, uint32_t(1), uint32_t(1)); // copy
    if (b->dragWin && !b->dragMime.empty() && b->dropFn) b->dropFn(b->dropUser, b->dragWin, 0, b->dragPos, b->dragMime, {});
}
void ddLeave(void* data, wl_proxy*) {
    auto* b = static_cast<Backend*>(data);
    if (b->dragWin && !b->dragMime.empty() && b->dropFn) b->dropFn(b->dropUser, b->dragWin, 1, b->dragPos, b->dragMime, {});
    if (b->dragWin) b->dragWin->dragOver = false;
    if (b->dragOffer && b->sel[0].offer != b->dragOffer) destroyOffer(b, b->dragOffer);
    b->dragOffer = nullptr;
    b->dragWin = nullptr;
}
void ddMotion(void* data, wl_proxy*, uint32_t, int32_t x, int32_t y) {
    auto* b = static_cast<Backend*>(data);
    b->dragPos = localPos(x, y);
    if (b->dragWin && !b->dragMime.empty() && b->dropFn) b->dropFn(b->dropUser, b->dragWin, 0, b->dragPos, b->dragMime, {});
}
void ddDrop(void* data, wl_proxy*) {
    auto* b = static_cast<Backend*>(data);
    Offer* o = b->dragOffer;
    Window* w = b->dragWin;
    std::vector<unsigned char> bytes;
    const bool got = o && w && !b->dragMime.empty() && receiveOffer(b, o, b->dragMime, bytes, 2000);
    if (o && b->ddVersion >= 3 && got) L.proxy_marshal(o->proxy, kOfferFinish);
    if (b->dropFn && w && !b->dragMime.empty()) {
        if (got) b->dropFn(b->dropUser, w, 2, b->dragPos, b->dragMime, std::string(bytes.begin(), bytes.end()));
        else b->dropFn(b->dropUser, w, 1, b->dragPos, b->dragMime, {}); // the source went away: ends the hover
    }
    if (w) {
        w->dragOver = false;
        w->win->requestRedraw();
    }
    if (o && b->sel[0].offer != o) destroyOffer(b, o);
    b->dragOffer = nullptr;
    b->dragWin = nullptr;
}
const Fn* dataDeviceListener() {
    static const Fn l[] = {fn(&ddDataOffer), fn(&ddEnter), fn(&ddLeave), fn(&ddMotion), fn(&ddDrop), fn(&ddSelection)};
    return l;
}
void psDataOffer(void* data, wl_proxy*, wl_proxy* id) { newOffer(static_cast<Backend*>(data), id, true); }
void psSelection(void* data, wl_proxy*, wl_proxy* offer) { setSelectionOffer(static_cast<Backend*>(data), 1, offer); }
const Fn* psDeviceListener() {
    static const Fn l[] = {fn(&psDataOffer), fn(&psSelection)};
    return l;
}
/// Writes `data` to `fd` and closes it, off the UI thread (the reader may be this process's own UI thread later).
void writeAsync(int fd, std::vector<unsigned char> data) {
    std::thread([fd, d = std::move(data)] {
        const int fl = ::fcntl(fd, F_GETFL);
        if (fl >= 0) ::fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        std::size_t at = 0;
        while (at < d.size()) {
            const ssize_t n = ::write(fd, d.data() + at, d.size() - at);
            if (n > 0) at += std::size_t(n);
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        ::close(fd);
    }).detach();
}
template <int Which>
void sourceSend(void* data, wl_proxy*, const char* mime, int32_t fd) {
    auto* b = static_cast<Backend*>(data);
    Selection& s = b->sel[Which];
    std::vector<unsigned char> bytes;
    if (!mime || !s.fn || !s.fn(s.user, mime, bytes)) {
        ::close(fd);
        return;
    }
    writeAsync(fd, std::move(bytes));
}
template <int Which>
void sourceCancelled(void* data, wl_proxy* src) {
    auto* b = static_cast<Backend*>(data);
    Selection& s = b->sel[Which];
    if (s.source == src) {
        s.source = nullptr;
        s.mimes.clear();
        s.fn = nullptr;
    }
    destroyRequest(src, Which ? kPssDestroy : kSrcDestroy);
}
void sourceTarget(void*, wl_proxy*, const char*) {}
void sourceUint(void*, wl_proxy*, uint32_t) {}
void sourceVoid(void*, wl_proxy*) {}

// ---- text-input-v3 ----
void tiEnter(void* data, wl_proxy*, wl_proxy* surface) {
    auto* b = static_cast<Backend*>(data);
    b->tiFocus = ownerOf(b, surface);
    b->tiEnabledFor = nullptr; // re-enabled by the next sync when a text field has focus
    b->tiCaret = {-1.f, -1.f, 0.f, 0.f};
    if (b->tiFocus) b->tiFocus->win->requestRedraw();
}
void tiLeave(void* data, wl_proxy* ti, wl_proxy*) {
    auto* b = static_cast<Backend*>(data);
    if (b->tiEnabledFor) {
        L.proxy_marshal(ti, kTiDisable);
        L.proxy_marshal(ti, kTiCommit);
    }
    if (b->tiFocus && b->tiShown) b->tiFocus->win->ui().textComposition({}, 0);
    b->tiShown = false;
    b->tiFocus = nullptr;
    b->tiEnabledFor = nullptr;
}
void tiPreedit(void* data, wl_proxy*, const char* text, int32_t begin, int32_t) {
    auto* b = static_cast<Backend*>(data);
    b->tiPreedit = text ? text : "";
    b->tiCursor = begin < 0 ? int32_t(b->tiPreedit.size()) : std::min(begin, int32_t(b->tiPreedit.size()));
    b->tiHasPreedit = true;
}
void tiCommitString(void* data, wl_proxy*, const char* text) {
    auto* b = static_cast<Backend*>(data);
    b->tiCommit = text ? text : "";
    b->tiHasCommit = true;
}
void tiDelete(void*, wl_proxy*, uint32_t, uint32_t) {} // no surrounding text is sent, so nothing is asked to be deleted
void tiDone(void* data, wl_proxy*, uint32_t) {
    auto* b = static_cast<Backend*>(data);
    Window* w = b->tiFocus;
    const bool pre = b->tiHasPreedit, com = b->tiHasCommit;
    b->tiHasPreedit = b->tiHasCommit = false;
    if (!w) return;
    UiContext& ui = w->win->ui();
    // text-input-v3 "done": the old pre-edit goes, the commit string is inserted, then the new pre-edit is shown
    if (b->tiShown) {
        ui.textComposition({}, 0);
        b->tiShown = false;
    }
    if (com && !b->tiCommit.empty()) ui.textInput(b->tiCommit);
    if (pre && !b->tiPreedit.empty()) {
        ui.textComposition(b->tiPreedit, b->tiCursor);
        b->tiShown = true;
    }
    b->tiCommit.clear();
    if (!pre) b->tiPreedit.clear();
    w->win->requestRedraw();
}
const Fn* textInputListener() {
    static const Fn l[] = {fn(&tiEnter), fn(&tiLeave), fn(&tiPreedit), fn(&tiCommitString), fn(&tiDelete), fn(&tiDone)};
    return l;
}

void iconSizeEv(void* data, wl_proxy*, int32_t size) {
    auto* b = static_cast<Backend*>(data);
    if (size > 0 && std::find(b->iconSizesPref.begin(), b->iconSizesPref.end(), size) == b->iconSizesPref.end()) b->iconSizesPref.push_back(size);
}
void iconDoneEv(void*, wl_proxy*) {}

// ---- seat / registry ----
void ensureSeatObjects(Backend* b) {
    if (b->seat && b->ddMgr && !b->dd) {
        b->dd = L.proxy_marshal_constructor(b->ddMgr, kDdmGetDevice, L.dataDevice, nullptr, b->seat);
        if (b->dd) L.proxy_add_listener(b->dd, const_cast<Fn*>(dataDeviceListener()), b);
    }
    if (b->seat && b->psMgr && !b->psDev) {
        b->psDev = L.proxy_marshal_constructor(b->psMgr, kPsmGetDevice, &psDevI, nullptr, b->seat);
        if (b->psDev) L.proxy_add_listener(b->psDev, const_cast<Fn*>(psDeviceListener()), b);
    }
    if (b->seat && b->tiMgr && !b->ti) {
        b->ti = L.proxy_marshal_constructor(b->tiMgr, kTiMgrGet, &tiI, nullptr, b->seat);
        if (b->ti) L.proxy_add_listener(b->ti, const_cast<Fn*>(textInputListener()), b);
    }
    if (b->pointer && b->shapeMgr && !b->shapeDev) b->shapeDev = L.proxy_marshal_constructor(b->shapeMgr, kShapeMgrGetPointer, &shapeDevI, nullptr, b->pointer);
}
void seatCaps(void* data, wl_proxy* seat, uint32_t caps) {
    auto* b = static_cast<Backend*>(data);
    if ((caps & 1u) && !b->pointer) {
        b->pointer = L.proxy_marshal_constructor(seat, kSeatGetPointer, L.pointer, nullptr);
        if (b->pointer) L.proxy_add_listener(b->pointer, const_cast<Fn*>(pointerListener()), b);
    } else if (!(caps & 1u) && b->pointer) {
        if (b->shapeDev) destroyRequest(b->shapeDev, kShapeDevDestroy);
        b->shapeDev = nullptr;
        L.proxy_destroy(b->pointer);
        b->pointer = nullptr;
        b->ptrFocus = nullptr;
        b->ptrChild = nullptr;
    }
    if ((caps & 2u) && !b->keyboard) {
        b->keyboard = L.proxy_marshal_constructor(seat, kSeatGetKeyboard, L.keyboard, nullptr);
        if (b->keyboard) L.proxy_add_listener(b->keyboard, const_cast<Fn*>(keyboardListener()), b);
    } else if (!(caps & 2u) && b->keyboard) {
        L.proxy_destroy(b->keyboard);
        b->keyboard = nullptr;
        if (b->kbdFocus) b->kbdFocus->kbdFocus = false;
        b->kbdFocus = nullptr;
        b->repeatKey = 0;
    }
    if ((caps & 4u) && !b->touch) {
        b->touch = L.proxy_marshal_constructor(seat, kSeatGetTouch, L.touch, nullptr);
        if (b->touch) L.proxy_add_listener(b->touch, const_cast<Fn*>(touchListener()), b);
    } else if (!(caps & 4u) && b->touch) {
        L.proxy_destroy(b->touch);
        b->touch = nullptr;
        touchCancelEv(b, nullptr);
    }
    ensureSeatObjects(b);
}
void seatName(void*, wl_proxy*, const char*) {}
const Fn* seatListener() {
    static const Fn l[] = {fn(&seatCaps), fn(&seatName)};
    return l;
}
wl_proxy* bindGlobal(wl_proxy* reg, uint32_t name, const wl_interface* iface, uint32_t version) {
    return L.proxy_marshal_constructor_versioned(reg, kRegistryBind, iface, version, name, iface->name, version, nullptr);
}
void registryGlobal(void* data, wl_proxy* reg, uint32_t name, const char* iface, uint32_t version) {
    auto* b = static_cast<Backend*>(data);
    const std::string_view n(iface);
    b->globals.emplace_back(n);
    if (n == "wl_compositor" && !b->compositor) {
        b->compositorVersion = std::min(version, 4u);
        b->compositor = bindGlobal(reg, name, L.compositor, b->compositorVersion);
    } else if (n == "wl_shm" && !b->shm) {
        b->shm = bindGlobal(reg, name, L.shm, 1);
        static const Fn l[] = {fn(&shmFormat)};
        L.proxy_add_listener(b->shm, const_cast<Fn*>(l), b);
    } else if (n == "xdg_wm_base" && !b->wmBase) {
        b->wmBase = bindGlobal(reg, name, &xdgWmBase, 1);
        static const Fn l[] = {fn(&pingEv)};
        L.proxy_add_listener(b->wmBase, const_cast<Fn*>(l), b);
    } else if (n == "zxdg_decoration_manager_v1" && !b->decoMgr) {
        b->decoMgr = bindGlobal(reg, name, &decoManager, 1);
    } else if (n == "wl_seat" && !b->seat) {
        b->seatVersion = std::min(version, 5u); // 5: wl_pointer.frame / axis_discrete
        b->seat = bindGlobal(reg, name, L.seat, b->seatVersion);
        b->seatName = name;
        L.proxy_add_listener(b->seat, const_cast<Fn*>(seatListener()), b);
    } else if (n == "wl_output") {
        auto* o = new Output{};
        o->name = name;
        o->proxy = bindGlobal(reg, name, L.output, std::min(version, 2u));
        L.proxy_add_listener(o->proxy, const_cast<Fn*>(outputListener()), o);
        b->outputs.push_back(o);
    } else if (n == "wp_viewporter" && !b->viewporter) {
        b->viewporter = bindGlobal(reg, name, &viewporterI, 1);
    } else if (n == "wp_fractional_scale_manager_v1" && !b->fracMgr) {
        b->fracMgr = bindGlobal(reg, name, &fracMgrI, 1);
    } else if (n == "wp_cursor_shape_manager_v1" && !b->shapeMgr) {
        b->shapeMgr = bindGlobal(reg, name, &shapeMgrI, 1);
    } else if (n == "zwp_text_input_manager_v3" && !b->tiMgr) {
        b->tiMgr = bindGlobal(reg, name, &tiMgrI, 1);
    } else if (n == "xdg_toplevel_icon_manager_v1" && !b->iconMgr) {
        b->iconMgr = bindGlobal(reg, name, &iconMgrI, 1);
        static const Fn l[] = {fn(&iconSizeEv), fn(&iconDoneEv)};
        L.proxy_add_listener(b->iconMgr, const_cast<Fn*>(l), b);
    } else if (n == "wl_data_device_manager" && !b->ddMgr) {
        b->ddVersion = std::min(version, 3u);
        b->ddMgr = bindGlobal(reg, name, L.ddManager, b->ddVersion);
    } else if (n == "zwp_primary_selection_device_manager_v1" && !b->psMgr) {
        b->psMgr = bindGlobal(reg, name, &psMgrI, 1);
    }
    ensureSeatObjects(b);
}
void registryRemove(void* data, wl_proxy*, uint32_t name) {
    auto* b = static_cast<Backend*>(data);
    for (std::size_t i = 0; i < b->outputs.size(); ++i)
        if (b->outputs[i]->name == name) {
            Output* o = b->outputs[i];
            for (Window* w : b->windows) std::erase(w->entered, o);
            L.proxy_destroy(o->proxy);
            delete o;
            b->outputs.erase(b->outputs.begin() + std::ptrdiff_t(i));
            for (Window* w : b->windows) rescale(*w);
            return;
        }
}
const Fn* registryListener() {
    static const Fn l[] = {fn(&registryGlobal), fn(&registryRemove)};
    return l;
}

// ---- cursor ----
uint32_t shapeOf(Cursor c) { // wp_cursor_shape_device_v1.shape, in fuse::ui::Cursor order (Hidden has none)
    static constexpr uint32_t kShape[] = {1 /*default*/, 1, 9 /*text*/, 4 /*pointer*/, 26 /*ew-resize*/, 27 /*ns-resize*/,
                                          29 /*nwse-resize*/, 28 /*nesw-resize*/, 13 /*move*/, 8 /*crosshair*/,
                                          15 /*not-allowed*/, 6 /*wait*/, 0};
    static_assert(sizeof(kShape) / sizeof(kShape[0]) == std::size_t(kCursorCount));
    return kShape[std::size_t(c)];
}
void applyCursorShape(Backend* b, Cursor c, Window* w) {
    if (!b->pointer || c == w->cursor || int(c) >= kCursorCount) return;
    w->cursor = c;
    if (c == Cursor::Hidden) { // no cursor image while the pointer is over the surface
        L.proxy_marshal(b->pointer, kPointerSetCursor, b->enterSerial, static_cast<wl_proxy*>(nullptr), int32_t(0), int32_t(0));
        w->shapeSent = 0;
        return;
    }
    if (b->shapeDev) { // the compositor draws its own cursor in the shape asked for
        L.proxy_marshal(b->shapeDev, kShapeDevSetShape, b->enterSerial, shapeOf(c));
        w->shapeSent = shapeOf(c);
        return;
    }
    w->shapeSent = 0;
    if (!C.load() || !b->shm) return;
    const int scale = std::max(1, int(std::ceil(w->win->scale())));
    if (!b->theme || b->themeScale != scale) {
        if (b->theme) C.theme_destroy(b->theme);
        int size = 24;
        if (const char* e = std::getenv("XCURSOR_SIZE"); e && std::atoi(e) > 0) size = std::atoi(e);
        b->theme = C.theme_load(std::getenv("XCURSOR_THEME"), size * scale, b->shm);
        b->themeScale = scale;
    }
    if (!b->theme) return;
    static constexpr const char* kNames[][2] = {{"default", "left_ptr"}, {"default", "left_ptr"}, {"text", "xterm"},
        {"pointer", "hand2"}, {"ew-resize", "sb_h_double_arrow"}, {"ns-resize", "sb_v_double_arrow"},
        {"nwse-resize", "bottom_right_corner"}, {"nesw-resize", "bottom_left_corner"}, {"move", "fleur"},
        {"crosshair", "cross"}, {"not-allowed", "crossed_circle"}, {"wait", "watch"}, {"default", "left_ptr"}};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == std::size_t(kCursorCount));
    wl_cursor* cur = nullptr;
    for (int k = 0; !cur && k < 2; ++k) cur = C.theme_get_cursor(b->theme, kNames[std::size_t(c)][k]);
    if (!cur) cur = C.theme_get_cursor(b->theme, "left_ptr");
    if (!cur || !cur->image_count) return;
    wl_cursor_image* img = cur->images[0];
    wl_proxy* buf = C.image_get_buffer(img);
    if (!buf) return;
    if (!b->cursorSurface) b->cursorSurface = L.proxy_marshal_constructor(b->compositor, kCompositorCreateSurface, L.surface, nullptr);
    if (b->compositorVersion >= 3) L.proxy_marshal(b->cursorSurface, kSurfaceSetBufferScale, int32_t(scale));
    L.proxy_marshal(b->cursorSurface, kSurfaceAttach, buf, int32_t(0), int32_t(0));
    L.proxy_marshal(b->cursorSurface, kSurfaceDamage, int32_t(0), int32_t(0), int32_t(img->width), int32_t(img->height));
    L.proxy_marshal(b->cursorSurface, kSurfaceCommit);
    L.proxy_marshal(b->pointer, kPointerSetCursor, b->enterSerial, b->cursorSurface, int32_t(img->hotspot_x / unsigned(scale)),
                    int32_t(img->hotspot_y / unsigned(scale)));
}
void applyCursor(Backend* b, Window* w) {
    Cursor c = w->win->ui().cursor();
    if (c == Cursor::Auto) c = Cursor::Arrow;
    w->cursorWanted = c;
    if (!b->pointer || b->ptrFocus != w) return; // sent when the pointer enters (set_cursor needs the enter serial)
    applyCursorShape(b, c, w);
}

/// Waits (round trips) for a child's first configure.
bool waitConfigured(Backend* b, const bool& configured) {
    for (int i = 0; i < 50 && !configured; ++i)
        if (L.display_roundtrip(b->dpy) < 0) return false;
    return configured;
}
wl_proxy* parentXdgSurface(const Child* c) { return c->parent ? c->parent->xdgSurf : c->owner->xdgSurf; }
} // namespace

// ---- API -----------------------------------------------------------------------------------------------------------
NativeBackend chooseBackend(NativeBackend option, std::string_view env, std::string_view waylandDisplay) {
    if (option != NativeBackend::Auto) return option;
    if (env == "wayland") return NativeBackend::Wayland;
    if (env == "x11") return NativeBackend::X11;
    return waylandDisplay.empty() ? NativeBackend::X11 : NativeBackend::Wayland;
}
bool backendForced(NativeBackend option, std::string_view env) {
    return option != NativeBackend::Auto || env == "wayland" || env == "x11";
}
Vec2 wheelNotches(Vec2 value, Vec2 steps, bool hasStepsX, bool hasStepsY, bool continuous) {
    // a wheel click is 10 units on most compositors when no step count came with it; finger / continuous scrolling is
    // in surface pixels and a notch scrolls 40 px (UiContext::wheel), so it follows the fingers 1:1
    const f32 unit = continuous ? 40.f : 10.f;
    const f32 x = hasStepsX ? steps.x : value.x / unit, y = hasStepsY ? steps.y : value.y / unit;
    return {x, -y};
}
std::string uriListToPaths(std::string_view list) {
    std::string out;
    while (!list.empty()) {
        const std::size_t nl = list.find('\n');
        std::string_view line = list.substr(0, nl);
        list = nl == std::string_view::npos ? std::string_view{} : list.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#' || !line.starts_with("file://")) continue;
        line.remove_prefix(7);
        const std::size_t slash = line.find('/'); // file://host/path: the host part (empty or "localhost") goes
        if (slash == std::string_view::npos) continue;
        line.remove_prefix(slash);
        if (!out.empty()) out += '\n';
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '%' && i + 2 < line.size() + 0 && hex(line[i + 1]) >= 0 && hex(line[i + 2]) >= 0) {
                out += char(hex(line[i + 1]) * 16 + hex(line[i + 2]));
                i += 2;
            } else {
                out += line[i];
            }
        }
    }
    return out;
}
bool loadLibrary(std::string& err) {
    if (!L.load(err)) return false;
    fillProtocolTypes();
    return true;
}

Backend* open(std::string& err, f32 scaleOption) {
    if (gBackend) {
        err = "one Wayland backend per process";
        return nullptr;
    }
    if (!loadLibrary(err)) return nullptr;
    wl_display* d = L.display_connect(nullptr);
    if (!d) {
        err = "cannot connect to the Wayland compositor";
        return nullptr;
    }
    auto* b = new Backend{};
    b->dpy = d;
    if (scaleOption > 0.f) b->fixedScale = std::clamp(scaleOption, 0.25f, 8.f);
    else if (const char* e = std::getenv("FUSE_UI_SCALE"); e && *e) b->fixedScale = std::clamp(std::strtof(e, nullptr), 0.f, 8.f);
    gBackend = b;
    b->registry = L.proxy_marshal_constructor(reinterpret_cast<wl_proxy*>(d), kDisplayGetRegistry, L.registry, nullptr);
    L.proxy_add_listener(b->registry, const_cast<Fn*>(registryListener()), b);
    L.display_roundtrip(d); // globals
    L.display_roundtrip(d); // output modes / scales, seat capabilities
    if (!b->compositor || !b->shm || !b->wmBase) {
        err = !b->wmBase ? "the compositor has no xdg_wm_base" : "the compositor lacks wl_compositor / wl_shm";
        close(b);
        return nullptr;
    }
    ensureSeatObjects(b);
    return b;
}

void close(Backend* b) {
    if (!b) return;
    while (!b->children.empty()) destroyChild(b, b->children.back());
    while (!b->windows.empty()) destroyWindow(b, b->windows.back());
    for (int i = 0; i < 2; ++i)
        if (b->sel[i].source) destroyRequest(b->sel[i].source, i ? kPssDestroy : kSrcDestroy);
    while (!b->offers.empty()) destroyOffer(b, b->offers.back());
    if (b->theme) C.theme_destroy(b->theme);
    if (b->cursorSurface) destroyRequest(b->cursorSurface, kSurfaceDestroy);
    if (b->xstate) X.state_unref(b->xstate);
    if (b->keymap) X.keymap_unref(b->keymap);
    if (b->xctx) X.context_unref(b->xctx);
    if (b->ti) destroyRequest(b->ti, kTiDestroy);
    if (b->shapeDev) destroyRequest(b->shapeDev, kShapeDevDestroy);
    if (b->dd) {
        if (b->ddVersion >= 2) destroyRequest(b->dd, kDdRelease);
        else L.proxy_destroy(b->dd);
    }
    if (b->psDev) destroyRequest(b->psDev, kPsdDestroy);
    if (b->pointer) L.proxy_destroy(b->pointer);
    if (b->keyboard) L.proxy_destroy(b->keyboard);
    if (b->touch) L.proxy_destroy(b->touch);
    for (Output* o : b->outputs) {
        L.proxy_destroy(o->proxy);
        delete o;
    }
    if (b->seat) L.proxy_destroy(b->seat);
    if (b->decoMgr) destroyRequest(b->decoMgr, kDecoManagerDestroy);
    if (b->viewporter) destroyRequest(b->viewporter, kViewporterDestroy);
    if (b->fracMgr) destroyRequest(b->fracMgr, kFracMgrDestroy);
    if (b->shapeMgr) destroyRequest(b->shapeMgr, kShapeMgrDestroy);
    if (b->tiMgr) destroyRequest(b->tiMgr, kTiMgrDestroy);
    if (b->iconMgr) destroyRequest(b->iconMgr, kIconMgrDestroy);
    if (b->psMgr) destroyRequest(b->psMgr, kPsmDestroy);
    if (b->ddMgr) L.proxy_destroy(b->ddMgr);
    if (b->wmBase) destroyRequest(b->wmBase, kWmBaseDestroy);
    if (b->shm) L.proxy_destroy(b->shm);
    if (b->compositor) L.proxy_destroy(b->compositor);
    if (b->registry) L.proxy_destroy(b->registry);
    L.display_flush(b->dpy);
    L.display_disconnect(b->dpy);
    if (gBackend == b) gBackend = nullptr;
    delete b;
}

Backend* current() { return gBackend; }
int fd(const Backend* b) { return L.display_get_fd(b->dpy); }
void* display(const Backend* b) { return b->dpy; }
bool hasGlobal(const Backend* b, const char* iface) {
    return b && std::find(b->globals.begin(), b->globals.end(), iface) != b->globals.end();
}

int beforePoll(Backend* b) {
    b->readPrepared = true;
    while (L.display_prepare_read(b->dpy) != 0)
        if (L.display_dispatch_pending(b->dpy) < 0) { // a dead connection dispatches nothing: do not spin on it
            b->broken = true;
            b->readPrepared = false;
            break;
        }
    L.display_flush(b->dpy);
    return L.display_get_fd(b->dpy);
}
constexpr auto kFrameWait = std::chrono::milliseconds(100); // a frame callback later than this is not waited for
void afterPoll(Backend* b, bool readable) {
    if (b->readPrepared) {
        if (readable) {
            if (L.display_read_events(b->dpy) < 0) b->broken = true;
        } else {
            L.display_cancel_read(b->dpy);
        }
        b->readPrepared = false;
    }
    if (L.display_dispatch_pending(b->dpy) < 0 || L.display_get_error(b->dpy) != 0) b->broken = true;
    for (Window* w : b->windows) // an occluded surface gets no frame callback: draw the held-back frame anyway
        if (w->frameDeferred && std::chrono::steady_clock::now() - w->frameSent >= kFrameWait) {
            w->frameDeferred = false;
            w->win->requestRedraw();
        }
    if (b->repeatKey && b->kbdFocus && b->repeatRate > 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto step = std::chrono::microseconds(1000000 / b->repeatRate);
        for (int n = 0; now >= b->repeatAt && n < 8; ++n) { // a long stall repeats a few times, not hundreds
            deliverKey(b, b->kbdFocus, b->repeatKey, true, true);
            b->repeatAt += step;
        }
        if (now >= b->repeatAt) b->repeatAt = now + step;
    }
    L.display_flush(b->dpy);
}
f32 nextTimeout(const Backend* b) {
    f32 t = std::numeric_limits<f32>::infinity();
    const auto now = std::chrono::steady_clock::now();
    if (b->repeatKey && b->kbdFocus) t = std::max(0.f, std::chrono::duration<f32>(b->repeatAt - now).count());
    for (const Window* w : b->windows)
        if (w->frameDeferred) t = std::min(t, std::max(0.f, std::chrono::duration<f32>(w->frameSent + kFrameWait - now).count()));
    return t;
}
bool roundtrip(Backend* b) { return L.display_roundtrip(b->dpy) >= 0; }
bool alive(const Backend* b) { return b && !b->broken; }

Window* createWindow(Backend* b, AppWindow& a, std::string& err) {
    auto* w = new Window{};
    w->b = b;
    w->win = &a;
    w->surface = L.proxy_marshal_constructor(b->compositor, kCompositorCreateSurface, L.surface, nullptr);
    if (!w->surface) {
        err = "wl_compositor.create_surface failed";
        delete w;
        return nullptr;
    }
    static const Fn surfaceL[] = {fn(&surfaceEnter), fn(&surfaceLeave), fn(&surfaceInt), fn(&surfaceUint)};
    L.proxy_add_listener(w->surface, const_cast<Fn*>(surfaceL), w);
    // with a viewport the buffer (logical size x any scale) is mapped onto the logical size: fractional scales work
    if (b->viewporter) w->viewport = L.proxy_marshal_constructor(b->viewporter, kViewporterGet, &viewportI, nullptr, w->surface);
    if (b->fracMgr && w->viewport && b->fixedScale <= 0.f) {
        w->frac = L.proxy_marshal_constructor(b->fracMgr, kFracMgrGet, &fracI, nullptr, w->surface);
        static const Fn fL[] = {fn(&fracPreferred)};
        if (w->frac) L.proxy_add_listener(w->frac, const_cast<Fn*>(fL), w);
    }
    w->xdgSurf = L.proxy_marshal_constructor(b->wmBase, kWmBaseGetXdgSurface, &xdgSurface, nullptr, w->surface);
    static const Fn xsL[] = {fn(&xdgSurfaceConfigure)};
    L.proxy_add_listener(w->xdgSurf, const_cast<Fn*>(xsL), w);
    w->toplevel = L.proxy_marshal_constructor(w->xdgSurf, kXdgSurfaceGetToplevel, &xdgToplevel, nullptr);
    static const Fn tL[] = {fn(&toplevelConfigure), fn(&toplevelClose)};
    L.proxy_add_listener(w->toplevel, const_cast<Fn*>(tL), w);
    w->title = a.title();
    L.proxy_marshal(w->toplevel, kToplevelSetTitle, w->title.c_str());
    L.proxy_marshal(w->toplevel, kToplevelSetAppId, a.app().name().empty() ? "fuse" : a.app().name().c_str());
    if (b->decoMgr) { // server-side frame where the compositor draws one (KDE, sway, labwc); none otherwise
        w->deco = L.proxy_marshal_constructor(b->decoMgr, kDecoManagerGet, &decoToplevel, nullptr, w->toplevel);
        static const Fn dL[] = {fn(&decoConfigure)};
        L.proxy_add_listener(w->deco, const_cast<Fn*>(dL), w);
        L.proxy_marshal(w->deco, kDecoSetMode, 2u /*server_side*/);
    }
    b->windows.push_back(w);
    rescale(*w); // until the surface enters an output / gets a preferred scale
    sendBufferScale(b, w->surface, w->scale, w->committedScale);
    L.proxy_marshal(w->surface, kSurfaceCommit); // no buffer: asks for the first configure
    for (int i = 0; i < 50 && !w->configured; ++i)
        if (L.display_roundtrip(b->dpy) < 0) break;
    if (!w->configured) {
        err = "no xdg_surface.configure from the compositor";
        destroyWindow(b, w);
        return nullptr;
    }
    return w;
}

void destroyWindow(Backend* b, Window* w) {
    if (!w) return;
    for (std::size_t i = b->children.size(); i-- > 0;)
        if (b->children[i]->owner == w) destroyChild(b, b->children[i]);
    std::erase_if(b->touches, [&](const Backend::TouchPoint& t) { return t.win == w; });
    if (w->frameCb) L.proxy_destroy(w->frameCb);
    if (b->ptrFocus == w) b->ptrFocus = nullptr;
    if (b->kbdFocus == w) {
        b->kbdFocus = nullptr;
        b->repeatKey = 0;
    }
    if (b->tiFocus == w) b->tiFocus = nullptr;
    if (b->tiEnabledFor == w) b->tiEnabledFor = nullptr;
    if (b->dragWin == w) b->dragWin = nullptr;
    if (w->deco) destroyRequest(w->deco, kDecoDestroy);
    if (w->viewport) destroyRequest(w->viewport, kViewportDestroy);
    if (w->frac) destroyRequest(w->frac, kFracDestroy);
    destroyRequest(w->toplevel, kToplevelDestroy);
    destroyRequest(w->xdgSurf, kXdgSurfaceDestroy);
    destroyRequest(w->surface, kSurfaceDestroy);
    for (ShmBuf& s : w->shm.bufs) freeBuf(s);
    for (ShmBuf& s : w->iconBufs) freeBuf(s);
    std::erase(b->windows, w);
    L.display_flush(b->dpy);
    delete w;
}

void* surface(const Window* w) { return w ? w->surface : nullptr; }
int bufferScale(const Window* w) { return w ? w->scale : 1; }
unsigned long long serialOf(const Window* w) { return w ? w->acks : 0; }
unsigned long long framesThrottled(const Window* w) { return w ? w->throttled : 0; }
bool fractional(const Window* w) { return w && w->frac; }

namespace {
/// Requests that only take once the state of the last configure is current (a buffer for it was committed).
void afterCommit(Window* w) {
    if (w->unmaximizeAfterCommit) {
        w->unmaximizeAfterCommit = false;
        L.proxy_marshal(w->toplevel, kToplevelUnsetMaximized);
    }
}
} // namespace
Window* findWindow(const Backend* b, const AppWindow& a) {
    for (Window* w : b->windows)
        if (w->win == &a) return w;
    return nullptr;
}
bool frameReady(Backend*, Window* w) {
    if (!w->frameCb || std::chrono::steady_clock::now() - w->frameSent >= kFrameWait) return true;
    w->frameDeferred = true; // frameDone (or the deadline in afterPoll) asks for the redraw
    ++w->throttled;
    return false;
}
bool presentShm(Backend* b, Window* w, const Image& img) {
    if (!w->configured || img.pixels.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    sendBufferScale(b, w->surface, w->scale, w->committedScale);
    sendViewport(*w);
    if (!attachImage(b, w->surface, w->shm, img, img.width, img.height)) {
        w->win->requestRedraw();
        return false;
    }
    if (w->frameCb) L.proxy_destroy(w->frameCb); // overdue: its done event (if it ever comes) is dropped
    w->frameCb = L.proxy_marshal_constructor(w->surface, kSurfaceFrame, L.callback, nullptr);
    if (w->frameCb) L.proxy_add_listener(w->frameCb, const_cast<Fn*>(frameListener()), w);
    w->frameSent = now;
    w->frameDeferred = false;
    L.proxy_marshal(w->surface, kSurfaceCommit);
    afterCommit(w);
    L.display_flush(b->dpy);
    return true;
}

void beforeVulkanPresent(Backend* b, Window* w) {
    if (w->vulkanCommitted) afterCommit(w); // the swapchain committed at least one buffer since the configure
    w->vulkanCommitted = !w->unmaximizeAfterCommit;
    w->shm.last = -1; // the swapchain owns the surface's buffers now
    sendBufferScale(b, w->surface, w->scale, w->committedScale);
    sendViewport(*w); // applies with the swapchain's commit
}

bool capture(const Window* w, Image& out) { return w && readShm(w->shm, out); }

void syncCursor(Backend* b, Window* w) { applyCursor(b, w); }
Cursor cursorShown(const Window* w) { return w ? w->cursorWanted : Cursor::Arrow; }
void resize(Backend*, Window* w, unsigned width, unsigned height) {
    w->win->resize(std::max(1u, width), std::max(1u, height));
    w->win->requestRedraw();
}
void setTitle(Backend* b, Window* w, const std::string& title) {
    if (!w || title == w->title) return;
    w->title = title;
    L.proxy_marshal(w->toplevel, kToplevelSetTitle, title.c_str());
    L.display_flush(b->dpy);
}
namespace {
void sendSizeLimits(Window* w) {
    auto px = [](f32 v) { return int32_t(std::max(0L, std::lround(v))); };
    L.proxy_marshal(w->toplevel, kToplevelSetMinSize, px(w->minSize.w), px(w->minSize.h));
    L.proxy_marshal(w->toplevel, kToplevelSetMaxSize, px(w->maxSize.w), px(w->maxSize.h));
}
} // namespace
void setSizeLimits(Backend* b, Window* w, Size mn, Size mx) {
    if (!w) return;
    w->minSize = mn;
    w->maxSize = mx;
    sendSizeLimits(w);
    if (w->configured) L.proxy_marshal(w->surface, kSurfaceCommit); // the limits are double-buffered surface state
    L.display_flush(b->dpy);
}
bool mapped(const Window* w) { return w && w->configured; }
bool setVisible(Backend* b, Window* w, bool on) {
    if (!w) return false;
    if (on == w->configured) return true;
    if (!on) { // a null buffer unmaps an xdg_surface and resets it (title, limits, state)
        if (w->frameCb) L.proxy_destroy(w->frameCb);
        w->frameCb = nullptr;
        w->frameDeferred = false;
        w->configured = false;
        w->shm.last = -1;
        L.proxy_marshal(w->surface, kSurfaceAttach, static_cast<wl_proxy*>(nullptr), int32_t(0), int32_t(0));
        L.proxy_marshal(w->surface, kSurfaceCommit);
        L.display_flush(b->dpy);
        return true;
    }
    L.proxy_marshal(w->toplevel, kToplevelSetTitle, w->title.c_str());
    L.proxy_marshal(w->toplevel, kToplevelSetAppId, w->win->app().name().empty() ? "fuse" : w->win->app().name().c_str());
    sendSizeLimits(w);
    L.proxy_marshal(w->surface, kSurfaceCommit); // no buffer: the initial commit again, answered by a configure
    for (int i = 0; i < 10 && !w->configured; ++i)
        if (L.display_roundtrip(b->dpy) < 0) return false;
    // weston keeps a toplevel configured through an unmap and sends nothing: the next buffer maps it again
    w->configured = true;
    w->win->requestRedraw();
    return true;
}
void setMode(Backend* b, Window* w, WindowMode m) {
    if (!w) return;
    w->restoring = m == WindowMode::Normal;
    if (m == WindowMode::Minimized) {
        L.proxy_marshal(w->toplevel, kToplevelSetMinimized);
    } else if (m == WindowMode::Fullscreen) {
        L.proxy_marshal(w->toplevel, kToplevelSetFullscreen, static_cast<wl_proxy*>(nullptr)); // the compositor picks the output
    } else {
        // unset both first: a fullscreen window may be maximised underneath without the configure saying so
        L.proxy_marshal(w->toplevel, kToplevelUnsetFullscreen);
        if (m == WindowMode::Maximized) L.proxy_marshal(w->toplevel, kToplevelSetMaximized);
        else L.proxy_marshal(w->toplevel, kToplevelUnsetMaximized);
    }
    L.display_flush(b->dpy);
}
WindowMode mode(const Window* w) {
    return !w ? WindowMode::Normal : w->fullscreen ? WindowMode::Fullscreen : w->maximized ? WindowMode::Maximized : WindowMode::Normal;
}
bool activated(const Window* w) { return w && w->activated; }
bool keyboardFocus(const Window* w) { return w && w->kbdFocus; }
bool serverDecorated(const Window* w) { return w && w->serverDeco; }
bool beginMove(Backend* b, Window* w) {
    if (!w || !b->seat || !b->pressSerial) return false;
    L.proxy_marshal(w->toplevel, kToplevelMove, b->seat, b->pressSerial);
    L.display_flush(b->dpy);
    return true;
}
bool beginResize(Backend* b, Window* w, unsigned edge) {
    if (!w || !b->seat || !b->pressSerial || edge == 0 || edge > 10) return false;
    L.proxy_marshal(w->toplevel, kToplevelResize, b->seat, b->pressSerial, uint32_t(edge));
    L.display_flush(b->dpy);
    return true;
}
unsigned cursorShapeSent(const Window* w) { return w ? w->shapeSent : 0; }

bool setIcon(Backend* b, Window* w, const std::vector<const Image*>& sizes) {
    if (!w || !b->iconMgr) return false;
    wl_proxy* icon = L.proxy_marshal_constructor(b->iconMgr, kIconMgrCreate, &iconI, nullptr);
    if (!icon) return false;
    std::vector<ShmBuf> bufs;
    std::vector<int> widths;
    for (const Image* img : sizes) {
        if (!img || img->width <= 0 || img->width != img->height) continue; // icons are square
        ShmBuf s;
        if (!makeBuf(b, s, img->width, img->height, kShmArgb8888, false)) continue;
        u32* dst = static_cast<u32*>(s.data);
        for (std::size_t i = 0; i < img->pixels.size(); ++i) { // straight 0xAABBGGRR -> premultiplied ARGB8888
            const u32 p = img->pixels[i], a = p >> 24;
            auto pm = [&](u32 c) { return (c * a + 127) / 255; };
            dst[i] = (a << 24) | (pm(p & 0xFFu) << 16) | (pm((p >> 8) & 0xFFu) << 8) | pm((p >> 16) & 0xFFu);
        }
        L.proxy_marshal(icon, kIconAddBuffer, s.buffer, int32_t(1));
        bufs.push_back(s);
        widths.push_back(img->width);
    }
    L.proxy_marshal(b->iconMgr, kIconMgrSetIcon, w->toplevel, bufs.empty() ? nullptr : icon);
    destroyRequest(icon, kIconDestroy); // the toplevel keeps the icon; the buffers stay until the next icon
    for (ShmBuf& s : w->iconBufs) freeBuf(s);
    w->iconBufs = std::move(bufs);
    w->iconWidths = std::move(widths);
    L.display_flush(b->dpy);
    return !w->iconWidths.empty();
}
std::vector<int> iconSizes(const Window* w) { return w ? w->iconWidths : std::vector<int>{}; }

std::vector<DisplayInfo> displays(const Backend* b) {
    std::vector<DisplayInfo> out;
    for (const Output* o : b->outputs) {
        DisplayInfo d;
        d.name = o->make.empty() && o->model.empty() ? "wayland" : o->make + " " + o->model;
        d.bounds = {f32(o->x), f32(o->y), f32(o->w), f32(o->h)};
        d.workArea = d.bounds; // the protocol does not tell clients about panels
        d.scale = f32(o->scale);
        d.refreshHz = f32(o->refresh) / 1000.f;
        d.primary = out.empty();
        out.push_back(std::move(d));
    }
    return out;
}

// ---- child surfaces ----
namespace {
Child* newChildSurface(Backend* b, Window* owner, Vec2 origin, Size size) {
    auto* c = new Child{};
    c->b = b;
    c->owner = owner;
    c->origin = origin;
    c->size = {std::max(1.f, size.w), std::max(1.f, size.h)};
    c->surface = L.proxy_marshal_constructor(b->compositor, kCompositorCreateSurface, L.surface, nullptr);
    if (!c->surface) {
        delete c;
        return nullptr;
    }
    static const Fn surfaceL[] = {fn(&surfaceObj), fn(&surfaceObj), fn(&surfaceInt), fn(&surfaceUint)};
    L.proxy_add_listener(c->surface, const_cast<Fn*>(surfaceL), c);
    if (b->viewporter) c->viewport = L.proxy_marshal_constructor(b->viewporter, kViewporterGet, &viewportI, nullptr, c->surface);
    c->xdgSurf = L.proxy_marshal_constructor(b->wmBase, kWmBaseGetXdgSurface, &xdgSurface, nullptr, c->surface);
    static const Fn xsL[] = {fn(&childSurfaceConfigure)};
    L.proxy_add_listener(c->xdgSurf, const_cast<Fn*>(xsL), c);
    return c;
}
} // namespace

Child* createPopup(Backend* b, Window* owner, Child* parent, Vec2 anchor, Size size, Vec2 origin, std::string& err) {
    Child* c = newChildSurface(b, owner, origin, size);
    if (!c) {
        err = "wl_compositor.create_surface failed";
        return nullptr;
    }
    c->parent = parent;
    // the anchor point lies inside the parent (xdg_positioner requires it); the rest of the way is the offset
    const Size ps = parent ? parent->size : owner->win->size();
    const int ax = std::clamp(int(std::lround(anchor.x)), 0, std::max(0, int(ps.w) - 1));
    const int ay = std::clamp(int(std::lround(anchor.y)), 0, std::max(0, int(ps.h) - 1));
    wl_proxy* pos = L.proxy_marshal_constructor(b->wmBase, kWmBaseCreatePositioner, &xdgPositioner, nullptr);
    L.proxy_marshal(pos, kPosSetSize, int32_t(std::ceil(c->size.w)), int32_t(std::ceil(c->size.h)));
    L.proxy_marshal(pos, kPosSetAnchorRect, int32_t(ax), int32_t(ay), int32_t(1), int32_t(1));
    L.proxy_marshal(pos, kPosSetAnchor, 5u /*top_left*/);
    L.proxy_marshal(pos, kPosSetGravity, 8u /*bottom_right*/);
    L.proxy_marshal(pos, kPosSetConstraint, 1u | 2u /*slide_x | slide_y*/);
    L.proxy_marshal(pos, kPosSetOffset, int32_t(std::lround(anchor.x)) - ax, int32_t(std::lround(anchor.y)) - ay);
    c->role = L.proxy_marshal_constructor(c->xdgSurf, kXdgSurfaceGetPopup, &xdgPopup, nullptr, parentXdgSurface(c), pos);
    destroyRequest(pos, kPosDestroy);
    static const Fn pL[] = {fn(&popupConfigure), fn(&popupDone)};
    L.proxy_add_listener(c->role, const_cast<Fn*>(pL), c);
    // Qt::Popup: grabbed with the press that opened it (an outside click then ends it: popup_done); a nested popup
    // may grab only while its parent holds the grab
    if (b->seat && b->pressSerial && (!parent || parent->grabbed)) {
        L.proxy_marshal(c->role, kPopupGrab, b->seat, b->pressSerial);
        c->grabbed = true;
    }
    c->placed = {f32(ax), f32(ay), c->size.w, c->size.h};
    b->children.push_back(c);
    L.proxy_marshal(c->surface, kSurfaceCommit);
    if (!waitConfigured(b, c->configured)) {
        err = "no configure for the xdg_popup";
        destroyChild(b, c);
        return nullptr;
    }
    return c;
}

Child* createTool(Backend* b, Window* owner, Size size, Vec2 origin, const std::string& title, std::string& err) {
    Child* c = newChildSurface(b, owner, origin, size);
    if (!c) {
        err = "wl_compositor.create_surface failed";
        return nullptr;
    }
    c->popup = false;
    c->role = L.proxy_marshal_constructor(c->xdgSurf, kXdgSurfaceGetToplevel, &xdgToplevel, nullptr);
    static const Fn tL[] = {fn(&childToplevelConfigure), fn(&childToplevelClose)};
    L.proxy_add_listener(c->role, const_cast<Fn*>(tL), c);
    L.proxy_marshal(c->role, kToplevelSetParent, owner->toplevel);
    L.proxy_marshal(c->role, kToplevelSetTitle, title.c_str());
    L.proxy_marshal(c->role, kToplevelSetAppId, "fuse");
    const int32_t w = int32_t(std::ceil(c->size.w)), h = int32_t(std::ceil(c->size.h));
    L.proxy_marshal(c->role, kToplevelSetMinSize, w, h); // the panel picks its size: tiling shells float it
    L.proxy_marshal(c->role, kToplevelSetMaxSize, w, h);
    b->children.push_back(c);
    L.proxy_marshal(c->surface, kSurfaceCommit);
    if (!waitConfigured(b, c->configured)) {
        err = "no configure for the tool window";
        destroyChild(b, c);
        return nullptr;
    }
    return c;
}

void destroyChild(Backend* b, Child* c) {
    if (!c) return;
    for (std::size_t i = b->children.size(); i-- > 0;) // nested popups first (protocol: topmost first)
        if (b->children[i]->parent == c) destroyChild(b, b->children[i]);
    if (b->ptrChild == c) b->ptrChild = nullptr;
    std::erase_if(b->touches, [&](const Backend::TouchPoint& t) { return t.child == c; });
    if (c->viewport) destroyRequest(c->viewport, kViewportDestroy);
    destroyRequest(c->role, c->popup ? kPopupDestroy : kToplevelDestroy);
    destroyRequest(c->xdgSurf, kXdgSurfaceDestroy);
    destroyRequest(c->surface, kSurfaceDestroy);
    for (ShmBuf& s : c->shm.bufs) freeBuf(s);
    std::erase(b->children, c);
    L.display_flush(b->dpy);
    delete c;
}
void setChildOrigin(Child* c, Vec2 origin) {
    if (c) c->origin = origin;
}
void resizeChild(Backend* b, Child* c, Size size) {
    if (!c || c->popup) return;
    size = {std::max(1.f, size.w), std::max(1.f, size.h)};
    if (size.w == c->size.w && size.h == c->size.h) return;
    c->size = size;
    const int32_t w = int32_t(std::ceil(size.w)), h = int32_t(std::ceil(size.h));
    L.proxy_marshal(c->role, kToplevelSetMinSize, w, h);
    L.proxy_marshal(c->role, kToplevelSetMaxSize, w, h);
    L.display_flush(b->dpy);
}
void presentChild(Backend* b, Child* c, const Image& img) {
    if (!c || !c->configured || img.pixels.empty()) return;
    int bw = img.width, bh = img.height;
    if (c->viewport) {
        L.proxy_marshal(c->viewport, kViewportSetDestination, int32_t(std::ceil(c->size.w)), int32_t(std::ceil(c->size.h)));
    } else { // integer buffer scale: the buffer is a whole multiple of it (padded with the edge pixels)
        const int s = std::max(1, int(std::lround(c->owner->win->scale())));
        c->bufScale = s;
        bw = (bw + s - 1) / s * s;
        bh = (bh + s - 1) / s * s;
        if (b->compositorVersion >= 3) L.proxy_marshal(c->surface, kSurfaceSetBufferScale, int32_t(s));
    }
    if (!attachImage(b, c->surface, c->shm, img, bw, bh)) {
        c->owner->win->requestRedraw();
        return;
    }
    L.proxy_marshal(c->surface, kSurfaceCommit);
    L.display_flush(b->dpy);
}
bool captureChild(const Child* c, Image& out) {
    if (!c || c->shm.last < 0) return false;
    // the raster as presented (without the padding of an integer buffer scale)
    const int w = int(std::ceil(c->size.w * c->owner->win->scale() - 0.01f)), h = int(std::ceil(c->size.h * c->owner->win->scale() - 0.01f));
    return readShm(c->shm, out, w, h);
}
Rect childPlacement(const Child* c) {
    Rect r = c ? c->placed : Rect{};
    for (const Child* p = c ? c->parent : nullptr; p; p = p->parent) {
        r.x += p->placed.x;
        r.y += p->placed.y;
    }
    return r;
}
bool childGrabbed(const Child* c) { return c && c->grabbed; }
bool childDismissed(const Child* c) { return c && c->done; }
bool childPressed(const Child* c) { return c && c->pressed; }
bool moveChild(Backend* b, Child* c) {
    if (!c || c->popup || !c->pressed || !b->seat) return false;
    L.proxy_marshal(c->role, kToplevelMove, b->seat, c->pressSerial);
    L.display_flush(b->dpy);
    c->pressed = false; // the compositor has the pointer now; the UI's press ends where it started
    c->owner->win->ui().pointerUp(c->pressPos);
    c->owner->win->requestRedraw();
    return true;
}
void setDismissHandler(Backend* b, void (*f)(void* user, Window* owner), void* user) {
    b->dismiss = f;
    b->dismissUser = user;
}

// ---- clipboard ----
bool hasDataDevice(const Backend* b, bool primary) { return b && (primary ? b->psDev != nullptr : b->dd != nullptr); }
bool setSelection(Backend* b, bool primary, const std::vector<std::string>& mimes, SelectionProvider f, void* user) {
    if (!hasDataDevice(b, primary)) return false;
    const int i = primary ? 1 : 0;
    Selection& s = b->sel[i];
    wl_proxy* src = primary ? L.proxy_marshal_constructor(b->psMgr, kPsmCreateSource, &psSrcI, nullptr)
                            : L.proxy_marshal_constructor(b->ddMgr, kDdmCreateSource, L.dataSource, nullptr);
    if (!src) return false;
    if (primary) {
        static const Fn l[] = {fn(&sourceSend<1>), fn(&sourceCancelled<1>)};
        L.proxy_add_listener(src, const_cast<Fn*>(l), b);
    } else {
        static const Fn l[] = {fn(&sourceTarget), fn(&sourceSend<0>), fn(&sourceCancelled<0>), fn(&sourceVoid), fn(&sourceVoid), fn(&sourceUint)};
        L.proxy_add_listener(src, const_cast<Fn*>(l), b);
    }
    for (const std::string& m : mimes) L.proxy_marshal(src, primary ? kPssOffer : kSrcOffer, m.c_str());
    if (s.source) destroyRequest(s.source, primary ? kPssDestroy : kSrcDestroy); // replaced (no cancelled for it then)
    s.source = src;
    s.mimes = mimes;
    s.fn = f;
    s.user = user;
    L.proxy_marshal(primary ? b->psDev : b->dd, primary ? kPsdSetSelection : kDdSetSelection, src, b->inputSerial);
    L.display_flush(b->dpy);
    return true;
}
bool ownsSelection(const Backend* b, bool primary) { return b && b->sel[primary ? 1 : 0].source != nullptr; }
std::vector<std::string> selectionMimes(Backend* b, bool primary) {
    const Selection& s = b->sel[primary ? 1 : 0];
    if (s.source) return s.mimes;
    return s.offer ? s.offer->mimes : std::vector<std::string>{};
}
bool receiveSelection(Backend* b, bool primary, const std::string& mime, std::vector<unsigned char>& out, int ms) {
    out.clear();
    Selection& s = b->sel[primary ? 1 : 0];
    if (!s.offer || std::find(s.offer->mimes.begin(), s.offer->mimes.end(), mime) == s.offer->mimes.end()) return false;
    return receiveOffer(b, s.offer, mime, out, ms);
}
void setDropHandler(Backend* b, DropHandler f, void* user) {
    b->dropFn = f;
    b->dropUser = user;
}
bool dragOver(const Window* w) { return w && w->dragOver; }

// ---- IME ----
bool hasTextInput(const Backend* b) { return b && b->ti; }
bool syncTextInput(Backend* b, Window* w, bool want, Rect caret) {
    if (!b->ti || !w || b->tiFocus != w) return false;
    bool dirty = false;
    if (want && b->tiEnabledFor != w) {
        L.proxy_marshal(b->ti, kTiEnable);
        L.proxy_marshal(b->ti, kTiSetContentType, 0u /*none*/, 0u /*normal*/);
        b->tiEnabledFor = w;
        b->tiCaret = {-1.f, -1.f, 0.f, 0.f};
        dirty = true;
    } else if (!want && b->tiEnabledFor == w) {
        L.proxy_marshal(b->ti, kTiDisable);
        b->tiEnabledFor = nullptr;
        if (b->tiShown) { // focus left the field: the pre-edit goes (nothing is committed)
            w->win->ui().textComposition({}, 0);
            b->tiShown = false;
        }
        dirty = true;
    }
    if (want) {
        const Rect r{std::round(caret.x), std::round(caret.y), std::max(1.f, std::round(caret.w)), std::max(1.f, std::round(caret.h))};
        if (r.x != b->tiCaret.x || r.y != b->tiCaret.y || r.w != b->tiCaret.w || r.h != b->tiCaret.h) {
            L.proxy_marshal(b->ti, kTiSetCursorRect, int32_t(r.x), int32_t(r.y), int32_t(r.w), int32_t(r.h));
            b->tiCaret = r;
            dirty = true;
        }
    }
    if (dirty) {
        L.proxy_marshal(b->ti, kTiCommit);
        L.display_flush(b->dpy);
    }
    return b->tiEnabledFor == w;
}

// ---- test injection ----
void injectPointer(Backend* b, Window* w, Vec2 at, int kind) {
    b->ptrFocus = w;
    b->ptrChild = nullptr;
    b->ptrPos = {at.x / w->win->scale(), at.y / w->win->scale()};
    if (kind == 0) {
        w->win->ui().setModifiers(b->mods);
        w->win->ui().pointerMove(b->ptrPos);
    } else {
        deliverButton(b, w, kBtnLeft, kind == 1);
    }
}
void injectChildPointer(Backend* b, Child* c, Vec2 at, int kind) {
    b->ptrFocus = nullptr;
    b->ptrChild = c;
    const f32 s = c->owner->win->scale();
    b->ptrPos = {at.x / s, at.y / s};
    if (kind == 0) {
        c->owner->win->ui().setModifiers(b->mods);
        c->owner->win->ui().pointerMove(childPos(*c, b->ptrPos));
        c->owner->win->requestRedraw();
    } else {
        deliverChildButton(b, c, 0, kBtnLeft, kind == 1);
    }
}
void injectKey(Backend* b, Window* w, unsigned code) {
    if (!b->xstate && X.load()) { // no keyboard from the compositor (headless): the default keymap
        if (!b->xctx) b->xctx = X.context_new(0);
        const RuleNames names{"evdev", "pc105", "us", "", ""};
        if (b->xctx && (b->keymap = X.keymap_new_from_names(b->xctx, &names, 0))) b->xstate = X.state_new(b->keymap);
    }
    deliverKey(b, w, code, true, false);
    deliverKey(b, w, code, false, false);
}
void injectClose(Window* w) { toplevelClose(w, w->toplevel); }

const void* protocolInterface(std::string_view name) {
    if (!L.so) return nullptr;
    const wl_interface* all[] = {&xdgWmBase, &xdgPositioner, &xdgSurface, &xdgToplevel, &xdgPopup, &decoManager, &decoToplevel,
                                 &viewporterI, &viewportI, &fracMgrI, &fracI, &shapeMgrI, &shapeDevI, &tiMgrI, &tiI, &iconMgrI,
                                 &iconI, &psMgrI, &psDevI, &psSrcI, &psOfferI};
    for (const wl_interface* i : all)
        if (name == i->name) return i;
    return nullptr;
}

std::string selfTest() {
    std::string err;
    if (!loadLibrary(err)) return err;
    // every message: one types entry per argument (wl_closure reads types[i] for each), opcodes as used above
    const wl_interface* all[] = {&xdgWmBase, &xdgPositioner, &xdgSurface, &xdgToplevel, &xdgPopup, &decoManager, &decoToplevel,
                                 &viewporterI, &viewportI, &fracMgrI, &fracI, &shapeMgrI, &shapeDevI, &tiMgrI, &tiI, &iconMgrI,
                                 &iconI, &psMgrI, &psDevI, &psSrcI, &psOfferI};
    for (const wl_interface* i : all) {
        for (int side = 0; side < 2; ++side) {
            const int n = side ? i->event_count : i->method_count;
            const wl_message* m = side ? i->events : i->methods;
            for (int k = 0; k < n; ++k) {
                int args = 0;
                for (const char* p = m[k].signature; *p; ++p)
                    if (*p != '?' && !(*p >= '0' && *p <= '9')) ++args;
                if (args > 8 || !m[k].types) return std::string(i->name) + "." + m[k].name + ": bad types";
                for (int a = 0; a < args; ++a) {
                    // object / new_id arguments of requests we send must name their interface (events: new_id)
                    int idx = 0;
                    char t = 0;
                    for (const char* p = m[k].signature; *p; ++p) {
                        if (*p == '?' || (*p >= '0' && *p <= '9')) continue;
                        if (idx++ == a) { t = *p; break; }
                    }
                    if (((t == 'n' || t == 'o') && !side && !m[k].types[a]) || (t == 'n' && side && !m[k].types[a]))
                        return std::string(i->name) + "." + m[k].name + ": untyped object";
                }
            }
        }
    }
    struct Op { const wl_interface* i; uint32_t op; const char* name; };
    const Op ops[] = {{&xdgWmBase, kWmBaseGetXdgSurface, "get_xdg_surface"}, {&xdgWmBase, kWmBasePong, "pong"},
                      {&xdgWmBase, kWmBaseCreatePositioner, "create_positioner"},
                      {&xdgPositioner, kPosSetSize, "set_size"}, {&xdgPositioner, kPosSetAnchorRect, "set_anchor_rect"},
                      {&xdgPositioner, kPosSetAnchor, "set_anchor"}, {&xdgPositioner, kPosSetGravity, "set_gravity"},
                      {&xdgPositioner, kPosSetConstraint, "set_constraint_adjustment"}, {&xdgPositioner, kPosSetOffset, "set_offset"},
                      {&xdgSurface, kXdgSurfaceGetToplevel, "get_toplevel"}, {&xdgSurface, kXdgSurfaceGetPopup, "get_popup"},
                      {&xdgSurface, kXdgSurfaceAck, "ack_configure"},
                      {&xdgToplevel, kToplevelSetParent, "set_parent"}, {&xdgToplevel, kToplevelSetTitle, "set_title"},
                      {&xdgToplevel, kToplevelMove, "move"}, {&xdgToplevel, kToplevelResize, "resize"},
                      {&xdgToplevel, kToplevelSetMaxSize, "set_max_size"}, {&xdgToplevel, kToplevelSetMinSize, "set_min_size"},
                      {&xdgToplevel, kToplevelSetMaximized, "set_maximized"}, {&xdgToplevel, kToplevelUnsetMaximized, "unset_maximized"},
                      {&xdgToplevel, kToplevelSetFullscreen, "set_fullscreen"}, {&xdgToplevel, kToplevelUnsetFullscreen, "unset_fullscreen"},
                      {&xdgToplevel, kToplevelSetMinimized, "set_minimized"}, {&xdgPopup, kPopupGrab, "grab"},
                      {&decoManager, kDecoManagerGet, "get_toplevel_decoration"}, {&decoToplevel, kDecoSetMode, "set_mode"},
                      {&viewporterI, kViewporterGet, "get_viewport"}, {&viewportI, kViewportSetDestination, "set_destination"},
                      {&fracMgrI, kFracMgrGet, "get_fractional_scale"}, {&shapeMgrI, kShapeMgrGetPointer, "get_pointer"},
                      {&shapeDevI, kShapeDevSetShape, "set_shape"}, {&tiMgrI, kTiMgrGet, "get_text_input"},
                      {&tiI, kTiEnable, "enable"}, {&tiI, kTiDisable, "disable"}, {&tiI, kTiSetContentType, "set_content_type"},
                      {&tiI, kTiSetCursorRect, "set_cursor_rectangle"}, {&tiI, kTiCommit, "commit"},
                      {&iconMgrI, kIconMgrCreate, "create_icon"}, {&iconMgrI, kIconMgrSetIcon, "set_icon"},
                      {&iconI, kIconAddBuffer, "add_buffer"}, {&psMgrI, kPsmCreateSource, "create_source"},
                      {&psMgrI, kPsmGetDevice, "get_device"}, {&psMgrI, kPsmDestroy, "destroy"},
                      {&psDevI, kPsdSetSelection, "set_selection"}, {&psDevI, kPsdDestroy, "destroy"},
                      {&psOfferI, kPsoReceive, "receive"}, {&psOfferI, kPsoDestroy, "destroy"},
                      {&psSrcI, kPssOffer, "offer"}, {&psSrcI, kPssDestroy, "destroy"}};
    for (const Op& o : ops)
        if (int(o.op) >= o.i->method_count || std::string_view(o.i->methods[o.op].name) != o.name)
            return std::string("opcode of ") + o.i->name + "." + o.name;
    // core interfaces from libwayland-client: the opcodes used here
    struct Core { const wl_interface* i; uint32_t op; const char* name; };
    const Core core[] = {{L.surface, kSurfaceAttach, "attach"}, {L.surface, kSurfaceCommit, "commit"},
                         {L.surface, kSurfaceFrame, "frame"}, {L.seat, kSeatGetTouch, "get_touch"},
                         {L.surface, kSurfaceSetBufferScale, "set_buffer_scale"}, {L.surface, kSurfaceDamageBuffer, "damage_buffer"},
                         {L.shm, kShmCreatePool, "create_pool"}, {L.shmPool, kPoolCreateBuffer, "create_buffer"},
                         {L.shmPool, kPoolDestroy, "destroy"}, {L.seat, kSeatGetPointer, "get_pointer"},
                         {L.seat, kSeatGetKeyboard, "get_keyboard"}, {L.pointer, kPointerSetCursor, "set_cursor"},
                         {L.registry, kRegistryBind, "bind"}, {L.compositor, kCompositorCreateSurface, "create_surface"},
                         {L.ddManager, kDdmCreateSource, "create_data_source"}, {L.ddManager, kDdmGetDevice, "get_data_device"},
                         {L.dataDevice, kDdSetSelection, "set_selection"}, {L.dataDevice, kDdRelease, "release"},
                         {L.dataSource, kSrcOffer, "offer"}, {L.dataSource, kSrcDestroy, "destroy"},
                         {L.dataOffer, kOfferAccept, "accept"}, {L.dataOffer, kOfferReceive, "receive"},
                         {L.dataOffer, kOfferDestroy, "destroy"}, {L.dataOffer, kOfferFinish, "finish"},
                         {L.dataOffer, kOfferSetActions, "set_actions"}};
    for (const Core& c : core)
        if (int(c.op) >= c.i->method_count || std::string_view(c.i->methods[c.op].name) != c.name)
            return std::string("core opcode ") + c.i->name + "." + c.name;
    // listener tables against the core interfaces' event counts (a listener shorter than the events would crash)
    if (L.dataDevice->event_count != 6 || L.dataOffer->event_count > 3 || L.dataSource->event_count > 6 || L.pointer->event_count < 9 ||
        L.touch->event_count < 5 || L.keyboard->event_count < 6)
        return "core event counts";
    // keys: xkbcommon's default US keymap (evdev codes) and the no-xkb fallback both reach the same FUSE keys
    struct K { unsigned code; Key key; const char* text; };
    const K ks[] = {{30, Key::A, "a"}, {28, Key::Enter, ""}, {105, Key::Left, ""}, {2, Key::Num1, "1"}, {57, Key::Space, " "}, {1, Key::Escape, ""}};
    for (int pass = 0; pass < 2; ++pass) {
        xkb_context* ctx = nullptr;
        xkb_keymap* km = nullptr;
        xkb_state* st = nullptr;
        if (pass == 0) {
            if (!X.load()) continue; // optional
            ctx = X.context_new(0);
            const RuleNames names{"evdev", "pc105", "us", "", ""};
            km = ctx ? X.keymap_new_from_names(ctx, &names, 0) : nullptr;
            st = km ? X.state_new(km) : nullptr;
            if (!st) return "xkbcommon could not compile the default keymap";
        }
        for (const K& k : ks) {
            unsigned long sym = 0;
            char text[16] = {};
            if (st) {
                sym = X.state_key_get_one_sym(st, k.code + 8);
                X.state_key_get_utf8(st, k.code + 8, text, sizeof text);
            } else {
                sym = evdevSym(k.code, false);
                if (sym >= 0x20 && sym < 0x7f) text[0] = char(sym);
            }
            if (detail::keysymKey(sym) != k.key) return std::string(pass ? "fallback" : "xkb") + " key " + std::to_string(k.code);
            if (*k.text && std::string_view(text) != k.text) return std::string(pass ? "fallback" : "xkb") + " text " + std::to_string(k.code);
        }
        if (st) X.state_unref(st);
        if (km) X.keymap_unref(km);
        if (ctx) X.context_unref(ctx);
    }
    return {};
}

} // namespace fuse::app::wl
#endif
