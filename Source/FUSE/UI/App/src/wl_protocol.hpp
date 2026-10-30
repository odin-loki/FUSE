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
    Message wmReq[4]{}, wmEv[1]{}, surfReq[5]{}, surfEv[1]{}, topReq[14]{}, topEv[2]{}, popReq[2]{}, popEv[2]{};

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
        positioner = {"xdg_positioner", 1, 0, nullptr, 0, nullptr}; // requests unused by FUSE yet
        surface = {"xdg_surface", 1, 5, surfReq, 1, surfEv};
        toplevel = {"xdg_toplevel", 1, 14, topReq, 2, topEv};
        popup = {"xdg_popup", 1, 2, popReq, 2, popEv};
    }
};
} // namespace fuse::app::wl
