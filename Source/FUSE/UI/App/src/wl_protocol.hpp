// Wayland wire-protocol tables for the runtime-loaded libwayland backends (client: wayland.cpp; test compositor).
// The core interfaces (wl_surface, wl_seat, ...) are data symbols of libwayland-client / -server and are looked up at
// run time; xdg-shell is not part of libwayland, so its tables are written out here (stable xdg-shell, version 1
// subset) and linked to the loaded core tables by XdgTables::init. No wayland headers or scanner are needed.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fuse::app::wl {
struct Interface;
struct Message {
    const char* name;
    const char* signature;
    const Interface** types;
};
struct Interface {
    const char* name;
    int version;
    int method_count;
    const Message* methods;
    int event_count;
    const Message* events;
};
struct Array {
    std::size_t size, alloc;
    void* data;
};
using Fixed = std::int32_t;
inline double fromFixed(Fixed f) { return double(f) / 256.0; }
inline Fixed toFixed(double d) { return Fixed(d * 256.0); }

/// xdg_wm_base / xdg_positioner / xdg_surface / xdg_toplevel / xdg_popup (requests + events, v1).
struct XdgTables {
    Interface wmBase{}, positioner{}, surface{}, toplevel{}, popup{};
    const Interface* nul = nullptr;
    const Interface* tGetXdgSurface[2]{};  // n o
    const Interface* tGetToplevel[1]{};    // n
    const Interface* tGetPopup[3]{};       // n ?o o
    const Interface* tPositioner[1]{};     // n
    const Interface* tSetParent[1]{};      // ?o
    const Interface* tSeatSerial[1]{};     // o (seat) ...
    const Interface* tFullscreen[1]{};     // ?o (output)
    const Interface* none[8]{};
    Message wmReq[4]{}, wmEv[1]{}, surfReq[5]{}, surfEv[1]{}, topReq[14]{}, topEv[2]{}, popReq[2]{}, popEv[2]{}, posReq[7]{};

    void init(const Interface* wlSurface, const Interface* wlSeat, const Interface* wlOutput) {
        tGetXdgSurface[0] = &surface; tGetXdgSurface[1] = wlSurface;
        tGetToplevel[0] = &toplevel;
        tGetPopup[0] = &popup; tGetPopup[1] = &surface; tGetPopup[2] = &positioner;
        tPositioner[0] = &positioner;
        tSetParent[0] = &toplevel;
        tSeatSerial[0] = wlSeat;
        tFullscreen[0] = wlOutput;
        wmReq[0] = {"destroy", "", none};
        wmReq[1] = {"create_positioner", "n", tPositioner};
        wmReq[2] = {"get_xdg_surface", "no", tGetXdgSurface};
        wmReq[3] = {"pong", "u", none};
        wmEv[0] = {"ping", "u", none};
        surfReq[0] = {"destroy", "", none};
        surfReq[1] = {"get_toplevel", "n", tGetToplevel};
        surfReq[2] = {"get_popup", "n?oo", tGetPopup};
        surfReq[3] = {"set_window_geometry", "iiii", none};
        surfReq[4] = {"ack_configure", "u", none};
        surfEv[0] = {"configure", "u", none};
        topReq[0] = {"destroy", "", none};
        topReq[1] = {"set_parent", "?o", tSetParent};
        topReq[2] = {"set_title", "s", none};
        topReq[3] = {"set_app_id", "s", none};
        topReq[4] = {"show_window_menu", "ouii", tSeatSerial};
        topReq[5] = {"move", "ou", tSeatSerial};
        topReq[6] = {"resize", "ouu", tSeatSerial};
        topReq[7] = {"set_max_size", "ii", none};
        topReq[8] = {"set_min_size", "ii", none};
        topReq[9] = {"set_maximized", "", none};
        topReq[10] = {"unset_maximized", "", none};
        topReq[11] = {"set_fullscreen", "?o", tFullscreen};
        topReq[12] = {"unset_fullscreen", "", none};
        topReq[13] = {"set_minimized", "", none};
        topEv[0] = {"configure", "iia", none};
        topEv[1] = {"close", "", none};
        popReq[0] = {"destroy", "", none};
        popReq[1] = {"grab", "ou", tSeatSerial};
        popEv[0] = {"configure", "iiii", none};
        popEv[1] = {"popup_done", "", none};
        wmBase = {"xdg_wm_base", 1, 4, wmReq, 1, wmEv};
        posReq[0] = {"destroy", "", none};
        posReq[1] = {"set_size", "ii", none};
        posReq[2] = {"set_anchor_rect", "iiii", none};
        posReq[3] = {"set_anchor", "u", none};
        posReq[4] = {"set_gravity", "u", none};
        posReq[5] = {"set_constraint_adjustment", "u", none};
        posReq[6] = {"set_offset", "ii", none};
        positioner = {"xdg_positioner", 1, 7, posReq, 0, nullptr};
        surface = {"xdg_surface", 1, 5, surfReq, 1, surfEv};
        toplevel = {"xdg_toplevel", 1, 14, topReq, 2, topEv};
        popup = {"xdg_popup", 1, 2, popReq, 2, popEv};
    }
};
/// zwp_text_input_manager_v3 / zwp_text_input_v3 (text-input-unstable-v3, version 1).
struct TextInputTables {
    Interface manager{}, input{};
    const Interface* tGet[2]{};     // n o(seat)
    const Interface* tSurface[1]{}; // o(surface)
    const Interface* none[4]{};
    Message mgrReq[2]{}, inReq[8]{}, inEv[6]{};
    void init(const Interface* wlSurface, const Interface* wlSeat) {
        tGet[0] = &input; tGet[1] = wlSeat;
        tSurface[0] = wlSurface;
        mgrReq[0] = {"destroy", "", none};
        mgrReq[1] = {"get_text_input", "no", tGet};
        inReq[0] = {"destroy", "", none};
        inReq[1] = {"enable", "", none};
        inReq[2] = {"disable", "", none};
        inReq[3] = {"set_surrounding_text", "sii", none};
        inReq[4] = {"set_text_change_cause", "u", none};
        inReq[5] = {"set_content_type", "uu", none};
        inReq[6] = {"set_cursor_rectangle", "iiii", none};
        inReq[7] = {"commit", "", none};
        inEv[0] = {"enter", "o", tSurface};
        inEv[1] = {"leave", "o", tSurface};
        inEv[2] = {"preedit_string", "?sii", none};
        inEv[3] = {"commit_string", "?s", none};
        inEv[4] = {"delete_surrounding_text", "uu", none};
        inEv[5] = {"done", "u", none};
        manager = {"zwp_text_input_manager_v3", 1, 2, mgrReq, 0, nullptr};
        input = {"zwp_text_input_v3", 1, 8, inReq, 6, inEv};
    }
};
/// wp_viewporter / wp_viewport (viewporter, stable v1) + wp_fractional_scale_manager_v1 / wp_fractional_scale_v1
/// (fractional-scale-v1, staging v1): the compositor sends preferred_scale in 120ths; the client renders a buffer at
/// logical * scale and maps it back to the logical size with wp_viewport.set_destination (buffer scale stays 1).
struct ScaleTables {
    Interface viewporter{}, viewport{}, fracManager{}, frac{};
    const Interface* tGetViewport[2]{}; // n o(surface)
    const Interface* tGetFrac[2]{};     // n o(surface)
    const Interface* none[4]{};
    Message vpMgrReq[2]{}, vpReq[3]{}, fmReq[2]{}, frReq[1]{}, frEv[1]{};
    void init(const Interface* wlSurface) {
        tGetViewport[0] = &viewport; tGetViewport[1] = wlSurface;
        tGetFrac[0] = &frac; tGetFrac[1] = wlSurface;
        vpMgrReq[0] = {"destroy", "", none};
        vpMgrReq[1] = {"get_viewport", "no", tGetViewport};
        vpReq[0] = {"destroy", "", none};
        vpReq[1] = {"set_source", "ffff", none};
        vpReq[2] = {"set_destination", "ii", none};
        fmReq[0] = {"destroy", "", none};
        fmReq[1] = {"get_fractional_scale", "no", tGetFrac};
        frReq[0] = {"destroy", "", none};
        frEv[0] = {"preferred_scale", "u", none};
        viewporter = {"wp_viewporter", 1, 2, vpMgrReq, 0, nullptr};
        viewport = {"wp_viewport", 1, 3, vpReq, 0, nullptr};
        fracManager = {"wp_fractional_scale_manager_v1", 1, 2, fmReq, 0, nullptr};
        frac = {"wp_fractional_scale_v1", 1, 1, frReq, 1, frEv};
    }
};
} // namespace fuse::app::wl
