// fuse_app native platform layer for X11 and Win32, second part (see native_services.inl for the first): OS tool
// windows for floating docks, clipboard formats (images, MIME types, lazy items, INCR, PRIMARY), OS drag and drop
// (XDND v5 / OLE; in and out), client-side window chrome and the window-less native dialogs. Included by native.cpp at
// file scope. X11 goes through the runtime-loaded function table only (no X headers); every optional symbol is
// checked. The Win32 half is in native_platform_win32.inl.

namespace fuse::app {

// ---- OS tool windows (both platforms) --------------------------------------------------------------------------------
void NativePlatform::Impl::syncTools(Rec& r) {
    if (!opts.osWindows || !r.handle) return;
    UiContext& ui = r.win->ui();
    int ox = 0, oy = 0;
    Rect work;
    if (!screenInfo(r, ox, oy, work)) return;
    r.originX = ox;
    r.originY = oy;
    const f32 s = r.scale;
    const auto& surf = ui.windowSurfaces();
    for (std::size_t i = r.tools.size(); i-- > 0;) { // windows whose node is gone (docked again, closed)
        bool live = false;
        for (const auto& ps : surf) live = live || ps.node == r.tools[i].node;
        if (!live) {
            destroyPopupOs(r.tools[i]);
            r.tools.erase(r.tools.begin() + std::ptrdiff_t(i));
        }
    }
    bool reordered = false;
    for (std::size_t si = 0; si < surf.size(); ++si) {
        const auto& ps = surf[si];
        std::size_t k = 0;
        while (k < r.tools.size() && r.tools[k].node != ps.node) ++k;
        if (k == r.tools.size()) {
            r.tools.emplace_back();
            r.tools.back().node = ps.node;
        }
        if (k != si && si < r.tools.size()) { // keep the surfaces' order (back to front)
            std::swap(r.tools[k], r.tools[si]);
            reordered = true;
            k = si;
        }
        Rec::Popup& p = r.tools[k];
        const Rect scr{std::round(f32(ox) + ps.rect.x * s), std::round(f32(oy) + ps.rect.y * s), std::max(1.f, std::ceil(ps.rect.w * s)),
                       std::max(1.f, std::ceil(ps.rect.h * s))};
        const bool moved = scr.x != p.screen.x || scr.y != p.screen.y || scr.w != p.screen.w || scr.h != p.screen.h;
        p.rect = ps.rect;
        p.screen = scr;
        if (!p.handle) {
            if (!createToolOs(r, p)) {
                r.tools.erase(r.tools.begin() + std::ptrdiff_t(k));
                return; // no window system resources: the rest stays unshown this frame
            }
            reordered = true;
        } else if (moved) {
            movePopupOs(p);
        }
        if (moved || p.revision != ps.revision) {
            p.revision = ps.revision;
            u32 clear = 0xFF000000u; // the panel's own fill shows in the corners its radius leaves
            for (const DrawCmd& c : ps.draw.cmds())
                if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
            p.img.resize(int(scr.w), int(scr.h), clear);
            CpuRasterizer rz = r.win->rasterizer();
            rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
            rz.render(ps.draw, p.img);
            blitPopupOs(p);
        }
    }
    if (reordered)
        for (Rec::Popup& p : r.tools) raiseToolOs(p); // back to front
}

// Wayland: no tool windows (a client cannot place a toplevel, so a floating dock could not follow its title drag or
// the main window); NativeOptions::osWindows is ignored there and floating docks stay in-window panels.
std::size_t NativePlatform::toolWindowCount(const AppWindow& w) const {
    FUSE_APP_WL_NONE(0);
    const Impl::Rec* r = m->find(w);
    return r ? r->tools.size() : 0;
}
unsigned long long NativePlatform::toolWindow(const AppWindow& w, std::size_t i) const {
    FUSE_APP_WL_NONE(0);
    const Impl::Rec* r = m->find(w);
    return r && i < r->tools.size() ? r->tools[i].handle : 0;
}
Rect NativePlatform::toolWindowScreenRect(const AppWindow& w, std::size_t i) const {
    FUSE_APP_WL_NONE(Rect{});
    const Impl::Rec* r = m->find(w);
    return r && i < r->tools.size() ? r->tools[i].screen : Rect{};
}
bool NativePlatform::captureToolWindow(const AppWindow& w, std::size_t i, Image& out) {
    FUSE_APP_WL_NONE(false);
    const Impl::Rec* r = m->find(w);
    return r && i < r->tools.size() && m->capturePopupOs(r->tools[i], out);
}
bool NativePlatform::postPointerMove(unsigned long long nativeWin, Vec2 at) {
    FUSE_APP_WL(postInput(nativeWin, at, 0));
    return nativeWin && m->postInputOs(nativeWin, at, 0);
}
bool NativePlatform::postPointerButton(unsigned long long nativeWin, Vec2 at, bool down) {
    FUSE_APP_WL(postInput(nativeWin, at, down ? 1 : 2));
    return nativeWin && m->postInputOs(nativeWin, at, down ? 1 : 2);
}
bool NativePlatform::postClose(unsigned long long nativeWin) {
    FUSE_APP_WL(postInput(nativeWin, {}, 3));
    return nativeWin && m->postInputOs(nativeWin, {}, 3);
}
bool NativePlatform::setClipboardImage(const Image& image) {
    std::vector<ClipboardItem> items(1);
    items[0].mime = kClipboardImage;
    items[0].image = image;
    return setClipboard(std::move(items));
}
bool NativePlatform::clipboardImage(Image& out) {
    std::vector<u8> png;
    return clipboardData(kClipboardImage, png) && decodePng(png, out);
}

#if !defined(_WIN32)
// ====================================================== X11 ==========================================================
namespace {
constexpr std::size_t kIncrChunk = 200000; ///< larger properties go through INCR in chunks of this size
bool isTextMime(std::string_view mime) { return mime == kClipboardText || mime == "text/plain" || mime == "UTF8_STRING"; }
double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void NativePlatform::Impl::initPlatformAtoms() {
    auto a = [&](const char* name) { return x.InternAtom(dpy, name, 0); };
    aImagePng = a("image/png"); aTextBare = a("text/plain"); aMotifHints = a("_MOTIF_WM_HINTS"); aMoveResize = a("_NET_WM_MOVERESIZE");
    dnd.aware = a("XdndAware"); dnd.enter = a("XdndEnter"); dnd.position = a("XdndPosition"); dnd.status = a("XdndStatus");
    dnd.leave = a("XdndLeave"); dnd.drop = a("XdndDrop"); dnd.finished = a("XdndFinished"); dnd.selection = a("XdndSelection");
    dnd.actionCopy = a("XdndActionCopy"); dnd.actionMove = a("XdndActionMove"); dnd.typeList = a("XdndTypeList");
    dnd.uriList = a("text/uri-list"); dnd.prop = a("FUSE_XDND");
}
std::string NativePlatform::Impl::atomName(x11::Atom a) const {
    std::string s;
    if (a && x.GetAtomName)
        if (char* n = x.GetAtomName(dpy, a)) {
            s = n;
            x.Free(n);
        }
    return s;
}

// ---- X11: XSETTINGS ----------------------------------------------------------------------------------------------------
/// Reads the settings manager's property and applies DPI, theme and cursor theme. `initial`: from the constructor
/// (finds the manager; no windows yet); later calls come from the manager's PropertyNotify and re-scale the windows.
void NativePlatform::Impl::readXSettings(bool initial) {
    if (initial) {
        const std::string sel = "_XSETTINGS_S" + std::to_string(x.DefaultScreen(dpy));
        aXsProp = x.InternAtom(dpy, "_XSETTINGS_SETTINGS", 0);
        xsOwner = x.GetSelectionOwner ? x.GetSelectionOwner(dpy, x.InternAtom(dpy, sel.c_str(), 0)) : 0;
        if (!xsOwner) return; // no settings manager (a bare X server): the environment / RESOURCE_MANAGER values stay
        x.SelectInput(dpy, xsOwner, 1L << 22 /*PropertyChange*/);
    }
    if (!xsOwner) return;
    x11::Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    if (x.GetWindowProperty(dpy, xsOwner, aXsProp, 0, 1L << 22, 0, 0 /*AnyPropertyType*/, &type, &format, &n, &after, &data) != 0 || !data) return;
    std::vector<XSetting> settings;
    if (format == 8) settings = parseXSettings({data, std::size_t(n)});
    x.Free(data);
    int dpi1024 = 0, cursorSize = 0, preferDark = -1;
    std::string themeName, cursorTheme;
    for (const XSetting& s : settings) {
        if (s.name == "Xft/DPI" && s.type == 0) dpi1024 = s.integer;
        else if (s.name == "Net/ThemeName" && s.type == 1) themeName = s.string;
        else if (s.name == "Gtk/ApplicationPreferDarkTheme" && s.type == 0) preferDark = s.integer;
        else if (s.name == "Gtk/CursorThemeName" && s.type == 1) cursorTheme = s.string;
        else if (s.name == "Gtk/CursorThemeSize" && s.type == 0) cursorSize = s.integer;
    }
    // theme: an explicit "prefer dark" wins, else the theme's name says it ("Adwaita-dark")
    const ColorScheme cs = preferDark > 0 ? ColorScheme::Dark : schemeFromGtkTheme(themeName);
    if (cs != ColorScheme::Unknown) {
        SystemTheme t = app.systemTheme();
        t.scheme = cs;
        app.setSystemTheme(t); // windows get ThemeChanged when it differs
    }
    // cursor theme / size: cursors made so far are stale
    bool cursorsChanged = false;
    if (cursorSize > 0 && x.CursorSetDefaultSize && x.CursorGetDefaultSize && x.CursorGetDefaultSize(dpy) != cursorSize) {
        x.CursorSetDefaultSize(dpy, cursorSize);
        cursorsChanged = true;
    }
    if (!cursorTheme.empty() && x.CursorSetTheme) {
        x.CursorSetTheme(dpy, cursorTheme.c_str());
        cursorsChanged = true;
    }
    if (cursorsChanged) {
        for (unsigned long& c : cursors) c = 0;
        for (Rec* r : recs) r->cursorApplied = false;
    }
    // DPI: below $FUSE_UI_SCALE, above RESOURCE_MANAGER's Xft.dpi
    if (dpi1024 > 0) {
        const char* gdk = std::getenv("GDK_SCALE");
        const char* own = std::getenv("FUSE_UI_SCALE");
        char line[64];
        std::snprintf(line, sizeof line, "Xft.dpi:\t%.3f\n", double(dpi1024) / 1024.0);
        detectedScale = dpi::x11Scale(line, gdk ? gdk : "", own ? own : "");
        for (Rec* r : recs) { // every shown window keeps its logical size at the new scale
            const f32 want = dpi::resolveScale(opts.scale, detectedScale);
            if (!r->handle || !r->win || want == r->scale) continue;
            const Size logical = r->win->size();
            r->scale = want;
            const Size px = dpi::toPixels(logical, want);
            r->win->resizePixels(u32(px.w), u32(px.h), want); // the ConfigureNotify of the resize below confirms it
            x.ResizeWindow(dpy, x11::Window(r->handle), unsigned(px.w), unsigned(px.h));
            if (r->win->minimumSize().w > 0.f || r->win->maximumSize().w > 0.f) windowRequest(r, *r->win, WindowRequest::SizeLimits);
        }
        x.Flush(dpy);
    }
}
int NativePlatform::cursorThemeSize() const {
    FUSE_APP_WL(cursorThemeSize());
    return m->dpy && m->x.CursorGetDefaultSize ? m->x.CursorGetDefaultSize(m->dpy) : 0;
}

// ---- X11: selections (CLIPBOARD / PRIMARY formats, INCR) -------------------------------------------------------------
bool NativePlatform::Impl::ownSelection(x11::Atom sel, Selection& s, std::vector<ClipboardItem> items) {
    s.items = std::move(items);
    x.SetSelectionOwner(dpy, sel, helper, 0 /*CurrentTime*/);
    s.owner = x.GetSelectionOwner(dpy, sel) == helper;
    x.Flush(dpy);
    if (!s.owner) s.items.clear();
    return s.owner;
}
std::vector<x11::Atom> NativePlatform::Impl::selectionTargets(const Selection& s) {
    std::vector<x11::Atom> t{aTargets};
    for (const ClipboardItem& it : s.items) {
        if (isTextMime(it.mime)) {
            t.push_back(aUtf8);
            t.push_back(aTextPlain);
            t.push_back(aTextBare);
        } else if (!it.mime.empty()) {
            t.push_back(x.InternAtom(dpy, it.mime.c_str(), 0));
        }
    }
    return t;
}
bool NativePlatform::Impl::selectionBytes(Selection& s, x11::Atom target, std::string& out) {
    const bool text = target == aUtf8 || target == aTextPlain || target == aTextBare;
    const std::string name = text ? std::string() : atomName(target);
    for (ClipboardItem& it : s.items) {
        if (it.mime.empty() || (text ? !isTextMime(it.mime) : it.mime != name)) continue;
        if (it.provide) { // lazy: made when first pasted
            const auto fn = it.provide;
            it.provide = nullptr;
            if (!fn(it.user, it)) {
                it.mime.clear(); // refused: no longer offered
                return false;
            }
        }
        if (text) {
            out = it.text;
        } else {
            if (it.mime == kClipboardImage && it.bytes.empty() && !encodePng(it.image, it.bytes)) return false; // encoded once
            out.assign(reinterpret_cast<const char*>(it.bytes.data()), it.bytes.size());
        }
        return true;
    }
    return false;
}
void NativePlatform::Impl::sendProperty(x11::Window requestor, x11::Atom property, x11::Atom target, std::string bytes) {
    if (bytes.size() <= kIncrChunk) {
        x.ChangeProperty(dpy, requestor, property, target, 8, 0, reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()));
        return;
    }
    // INCR: announce the size, then one chunk each time the requestor deletes the property (continueIncr)
    std::size_t idx = 0;
    if (!find(requestor) && requestor != helper && !findTool(requestor, idx) && !findPopup(requestor, idx))
        x.SelectInput(dpy, requestor, 1L << 22 /*PropertyChange*/);
    const long size = long(bytes.size());
    x.ChangeProperty(dpy, requestor, property, aIncr, 32, 0, reinterpret_cast<const unsigned char*>(&size), 1);
    std::erase_if(incrOut, [&](const IncrOut& t) { return t.requestor == requestor && t.property == property; });
    incrOut.push_back({requestor, property, target, std::move(bytes), 0});
}
bool NativePlatform::Impl::continueIncr(const x11::PropertyEv& e) {
    if (e.state != 1 /*PropertyDelete*/) return false;
    for (std::size_t i = 0; i < incrOut.size(); ++i) {
        IncrOut& t = incrOut[i];
        if (t.requestor != e.window || t.property != e.atom) continue;
        const std::size_t n = std::min(kIncrChunk, t.bytes.size() - t.offset); // 0 = the end marker
        x.ChangeProperty(dpy, t.requestor, t.property, t.target, 8, 0, reinterpret_cast<const unsigned char*>(t.bytes.data() + t.offset), int(n));
        t.offset += n;
        x.Flush(dpy);
        if (n == 0) incrOut.erase(incrOut.begin() + std::ptrdiff_t(i));
        return true;
    }
    return false;
}
void NativePlatform::Impl::serveSelection(const x11::SelectionRequestEv& rq) {
    x11::Event out{};
    out.selection.type = x11::SelectionNotify;
    out.selection.display = dpy;
    out.selection.requestor = rq.requestor;
    out.selection.selection = rq.selection;
    out.selection.target = rq.target;
    out.selection.time = rq.time;
    out.selection.property = 0; // refused unless filled below
    const x11::Atom prop = rq.property ? rq.property : rq.target; // obsolete clients send None
    std::string bytes;
    if (rq.selection == dnd.selection) { // the data of a drag of ours (XDND source)
        for (const auto& [atom, data] : dnd.types)
            if (atom == rq.target) {
                sendProperty(rq.requestor, prop, rq.target, data);
                out.selection.property = prop;
                break;
            }
    } else if (Selection* s = selectionOf(rq.selection); s && s->owner) {
        if (rq.target == aTargets) {
            const std::vector<x11::Atom> list = selectionTargets(*s);
            x.ChangeProperty(dpy, rq.requestor, prop, 4 /*XA_ATOM*/, 32, 0, reinterpret_cast<const unsigned char*>(list.data()), int(list.size()));
            out.selection.property = prop;
        } else if (selectionBytes(*s, rq.target, bytes)) {
            sendProperty(rq.requestor, prop, rq.target, std::move(bytes));
            out.selection.property = prop;
        }
    }
    x.SendEvent(dpy, rq.requestor, 0, 0, &out);
    x.Flush(dpy);
}
/// Asks the owner of `selection` for `target` and waits for the answer (INCR included). Blocks up to a second without
/// progress; false when there is no owner, it refuses the target or does not answer.
bool NativePlatform::Impl::convert(x11::Atom selection, x11::Atom target, std::string& out, x11::Atom* typeOut, int* formatOut) {
    out.clear();
    if (!x.GetSelectionOwner(dpy, selection)) return false; // nothing copied anywhere
    if (x.DeleteProperty) x.DeleteProperty(dpy, helper, aClipProp);
    x.ConvertSelection(dpy, selection, target, aClipProp, helper, 0);
    x.Flush(dpy);
    x11::Event ev{};
    auto waitFor = [&](int type, auto&& match) {
        const double deadline = steadySeconds() + 1.0;
        for (;;) {
            while (x.CheckTypedWindowEvent(dpy, helper, type, &ev))
                if (match(ev)) return true;
            if (steadySeconds() > deadline) return false;
            pollfd p{x.ConnectionNumber(dpy), POLLIN, 0};
            ::poll(&p, 1, 5);
        }
    };
    auto readProp = [&](std::string& bytes, x11::Atom& type, int& format) {
        unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        type = 0;
        format = 0;
        if (x.GetWindowProperty(dpy, helper, aClipProp, 0, 1L << 28, 1 /*delete*/, 0 /*AnyPropertyType*/, &type, &format, &n, &after, &data) != 0)
            return false;
        bytes.clear();
        if (data) {
            bytes.assign(reinterpret_cast<const char*>(data), std::size_t(n) * (format == 32 ? sizeof(long) : std::size_t(format / 8)));
            x.Free(data);
        }
        return true;
    };
    if (!waitFor(x11::SelectionNotify, [&](const x11::Event& e) { return e.selection.selection == selection; }) || !ev.selection.property)
        return false; // no answer / the owner does not have the target
    x11::Atom type = 0;
    int format = 0;
    std::string first;
    if (!readProp(first, type, format) || !type) return false;
    if (type == aIncr) { // the delete above started the transfer: one chunk per PropertyNotify (NewValue), empty = done
        type = 0;
        for (;;) {
            if (!waitFor(x11::PropertyNotify, [&](const x11::Event& e) { return e.property.atom == aClipProp && e.property.state == 0; })) return false;
            std::string chunk;
            x11::Atom t = 0;
            int f = 0;
            if (!readProp(chunk, t, f)) return false;
            if (!t) continue; // the notification of a chunk already taken
            if (chunk.empty()) break;
            out += chunk;
            type = t;
            format = f;
        }
        if (!type) return false;
    } else {
        out = std::move(first);
    }
    if (typeOut) *typeOut = type;
    if (formatOut) *formatOut = format;
    return true;
}

bool NativePlatform::setClipboard(std::vector<ClipboardItem> items) {
    FUSE_APP_WL(setClipboard(std::move(items)));
    return m->clipboardUsable() && m->ownSelection(m->aClipboard, m->clip, std::move(items));
}
bool NativePlatform::clipboardData(std::string_view mime, std::vector<u8>& out) {
    FUSE_APP_WL(clipboardData(mime, out));
    out.clear();
    if (!m->clipboardUsable() || mime.empty()) return false;
    const x11::Atom target = isTextMime(mime) ? m->aUtf8 : m->x.InternAtom(m->dpy, std::string(mime).c_str(), 0);
    std::string bytes;
    if (m->clip.owner ? !m->selectionBytes(m->clip, target, bytes) : !m->convert(m->aClipboard, target, bytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}
std::vector<std::string> NativePlatform::clipboardFormats() {
    FUSE_APP_WL(clipboardFormats());
    std::vector<std::string> out;
    if (!m->clipboardUsable()) return out;
    auto add = [&](std::string name) {
        if (isTextMime(name) || name.starts_with("text/plain;")) name = kClipboardText;
        if (name.find('/') == std::string::npos) return; // TARGETS, TIMESTAMP, MULTIPLE, STRING ...
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    };
    if (m->clip.owner) {
        for (const ClipboardItem& it : m->clip.items) add(it.mime);
        return out;
    }
    std::string bytes;
    x11::Atom type = 0;
    int format = 0;
    if (m->convert(m->aClipboard, m->aTargets, bytes, &type, &format) && format == 32)
        for (std::size_t i = 0; i + sizeof(long) <= bytes.size(); i += sizeof(long)) {
            unsigned long atom = 0;
            std::memcpy(&atom, bytes.data() + i, sizeof atom);
            add(m->atomName(atom));
        }
    return out;
}
bool NativePlatform::clipboardOwned() const {
    FUSE_APP_WL(clipboardOwned());
    return m->clip.owner;
}
bool NativePlatform::setPrimarySelectionText(std::string_view utf8) {
    FUSE_APP_WL(setPrimarySelectionText(utf8));
    if (!m->clipboardUsable()) return false;
    std::vector<ClipboardItem> items(1);
    items[0].mime = kClipboardText;
    items[0].text = utf8;
    return m->ownSelection(m->aPrimary, m->primary, std::move(items));
}
bool NativePlatform::primarySelectionText(std::string& out) {
    FUSE_APP_WL(primarySelectionText(out));
    if (!m->clipboardUsable()) return false;
    return m->primary.owner ? m->selectionBytes(m->primary, m->aUtf8, out) : m->convert(m->aPrimary, m->aUtf8, out);
}

// ---- X11: XDND v5 ------------------------------------------------------------------------------------------------------
void NativePlatform::Impl::dndSend(x11::Window to, x11::Atom type, long l1, long l2, long l3, long l4, x11::Window self) {
    x11::Event e{};
    e.client.type = x11::ClientMessage;
    e.client.display = dpy;
    e.client.window = to;
    e.client.message_type = type;
    e.client.format = 32;
    e.client.l[0] = long(self);
    e.client.l[1] = l1; e.client.l[2] = l2; e.client.l[3] = l3; e.client.l[4] = l4;
    x.SendEvent(dpy, to, 0, 0, &e);
    x.Flush(dpy);
}
/// One XDND client message for window `r`: as the target (Enter / Position / Leave / Drop from a drag of another
/// client) or as the source (Status / Finished answering a drag of ours). False when it is not an XDND message.
bool NativePlatform::Impl::dndClient(Rec& r, const x11::ClientEv& c) {
    if (!opts.dragAndDrop) return false;
    const x11::Window self = x11::Window(r.handle);
    UiContext& ui = r.win->ui();
    auto hover = [&] { // what is known while the drag hovers: the kind of data, not yet its content
        DragData d;
        d.type = dnd.type == dnd.uriList ? kDragFiles : kDragText;
        return d;
    };
    if (c.message_type == dnd.enter) {
        dnd.source = x11::Window(c.l[0]);
        dnd.window = self;
        dnd.type = 0;
        dnd.dropping = false;
        auto consider = [&](x11::Atom t) { // files win, then UTF-8 text, then the other text names
            if (t == dnd.uriList) dnd.type = t;
            else if (dnd.type != dnd.uriList && (t == aUtf8 || ((t == aTextPlain || t == aTextBare) && !dnd.type))) dnd.type = t;
        };
        if (c.l[1] & 1) { // more than three types: XdndTypeList on the source
            std::vector<unsigned long> list;
            if (windowLongs(dnd.source, dnd.typeList, 4 /*XA_ATOM*/, list))
                for (unsigned long t : list) consider(t);
        } else {
            for (int i = 2; i < 5; ++i)
                if (c.l[i]) consider(x11::Atom(c.l[i]));
        }
        return true;
    }
    if (c.message_type == dnd.position) {
        if (x11::Window(c.l[0]) != dnd.source) return true;
        const int rx = short((static_cast<unsigned long>(c.l[2]) >> 16) & 0xFFFF), ry = short(c.l[2] & 0xFFFF);
        int wx = 0, wy = 0;
        x11::Window child = 0;
        x.TranslateCoordinates(dpy, x.RootWindow(dpy, x.DefaultScreen(dpy)), self, rx, ry, &wx, &wy, &child);
        dnd.window = self;
        dnd.pos = toUi(r, f32(wx), f32(wy));
        const bool ok = dnd.type && ui.dragExternalMove(dnd.pos, hover());
        // always "copy": the data stays the other application's (a file manager must not delete what was dropped)
        dndSend(dnd.source, dnd.status, ok ? 1 : 0, 0, 0, ok ? long(dnd.actionCopy) : 0, self);
        r.win->requestRedraw();
        return true;
    }
    if (c.message_type == dnd.leave) {
        if (x11::Window(c.l[0]) == dnd.source) {
            ui.dragExternalLeave();
            dnd.source = 0;
            r.win->requestRedraw();
        }
        return true;
    }
    if (c.message_type == dnd.drop) {
        if (x11::Window(c.l[0]) != dnd.source) return true;
        if (dnd.type && x.ConvertSelection) { // the data arrives as a SelectionNotify (dndSelectionNotify)
            dnd.dropping = true;
            dnd.window = self;
            if (x.DeleteProperty) x.DeleteProperty(dpy, self, dnd.prop);
            x.ConvertSelection(dpy, dnd.selection, dnd.type, dnd.prop, self, static_cast<unsigned long>(c.l[2]));
            x.Flush(dpy);
        } else {
            ui.dragExternalLeave();
            dndSend(dnd.source, dnd.finished, 0, 0, 0, 0, self);
            dnd.source = 0;
        }
        return true;
    }
    if (c.message_type == dnd.status) {
        if (dnd.rec && x11::Window(c.l[0]) == dnd.target) dnd.accepted = (c.l[1] & 1) != 0;
        return true;
    }
    if (c.message_type == dnd.finished) {
        if (dnd.rec && dnd.dropped)
            dndOutEnd((c.l[1] & 1) ? (x11::Atom(c.l[2]) == dnd.actionMove ? DropAction::Move : DropAction::Copy) : DropAction::None);
        return true;
    }
    return false;
}
/// The data of a drop on one of our windows: hands it to the UI's drop target and answers the source.
void NativePlatform::Impl::dndSelectionNotify(const x11::SelectionEv& e) {
    if (!dnd.dropping || !dnd.source) return;
    dnd.dropping = false;
    const x11::Window self = dnd.window;
    Rec* r = find(self);
    DropAction action = DropAction::None;
    if (r && r->win && e.property) {
        x11::Atom type = 0;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        if (x.GetWindowProperty(dpy, self, dnd.prop, 0, 1L << 24, 1 /*delete*/, 0 /*AnyPropertyType*/, &type, &format, &n, &after, &data) == 0 &&
            data) {
            if (format == 8 && type != aIncr) { // INCR drops (very large lists) are refused
                const std::string_view bytes(reinterpret_cast<const char*>(data), std::size_t(n));
                DragData d;
                if (dnd.type == dnd.uriList) {
                    d.type = kDragFiles;
                    d.text = pathsFromUriList(bytes); // local files only
                } else {
                    d.type = kDragText;
                    d.text = bytes;
                }
                if (d.type == kDragText || !d.text.empty()) action = r->win->ui().dropExternal(dnd.pos, d);
            }
            x.Free(data);
        }
    }
    if (r && r->win) {
        if (action == DropAction::None) r->win->ui().dragExternalLeave();
        r->win->requestRedraw();
    }
    dndSend(dnd.source, dnd.finished, action != DropAction::None ? 1 : 0, action != DropAction::None ? long(dnd.actionCopy) : 0, 0, 0, self);
    dnd.source = 0;
}
/// The top-most XdndAware window of another client under a screen position (0: none, or one of ours).
x11::Window NativePlatform::Impl::dndTargetAt(int rx, int ry) {
    const x11::Window root = x.RootWindow(dpy, x.DefaultScreen(dpy));
    x11::Window cur = root;
    for (int depth = 0; depth < 16; ++depth) {
        if (cur != root) {
            x11::Atom actual = 0;
            int fmt = 0;
            unsigned long n = 0, after = 0;
            unsigned char* data = nullptr;
            if (x.GetWindowProperty(dpy, cur, dnd.aware, 0, 1, 0, 4 /*XA_ATOM*/, &actual, &fmt, &n, &after, &data) == 0 && data) {
                x.Free(data);
                if (n) return find(cur) ? 0 : cur; // our own windows take the drag through the UI
            }
        }
        int wx = 0, wy = 0;
        x11::Window child = 0;
        if (!x.TranslateCoordinates(dpy, root, cur, rx, ry, &wx, &wy, &child) || !child) return 0;
        cur = child;
    }
    return 0;
}
/// A pointer move while a drag of the UI is outside the app's windows (`outside`) or was before: offers the data to
/// the application under the pointer.
void NativePlatform::Impl::dndOutMotion(Rec& r, const x11::InputEv& in, bool outside) {
    UiContext& ui = r.win->ui();
    const x11::Window self = x11::Window(r.handle);
    if (dnd.dropped) return; // waiting for the target's answer
    if (!ui.dragging()) { // the UI ended the drag (Escape, a cancelled touch)
        if (dnd.rec == &r) {
            if (dnd.target) dndSend(dnd.target, dnd.leave, 0, 0, 0, 0, self);
            dnd.rec = nullptr;
            dnd.target = 0;
        }
        return;
    }
    if (!dnd.rec) { // the drag leaves for the first time: own XdndSelection with what it carries
        const DragData* d = ui.dragData();
        if (!d || !ui.alive(d->source) || (d->type != kDragText && d->type != kDragFiles) || !x.SetSelectionOwner) return;
        dnd.types.clear();
        if (d->type == kDragFiles) dnd.types.emplace_back(dnd.uriList, uriListFromPaths(d->text));
        dnd.types.emplace_back(aUtf8, d->text);
        dnd.types.emplace_back(aTextPlain, d->text);
        dnd.types.emplace_back(aTextBare, d->text);
        std::vector<unsigned long> list;
        for (const auto& t : dnd.types) list.push_back(t.first);
        x.ChangeProperty(dpy, self, dnd.typeList, 4 /*XA_ATOM*/, 32, 0, reinterpret_cast<const unsigned char*>(list.data()), int(list.size()));
        x.SetSelectionOwner(dpy, dnd.selection, self, in.time);
        dnd.rec = &r;
        dnd.target = 0;
        dnd.accepted = dnd.dropped = false;
    }
    if (dnd.rec != &r) return;
    const x11::Window t = outside ? dndTargetAt(in.x_root, in.y_root) : 0;
    if (t != dnd.target) {
        if (dnd.target) dndSend(dnd.target, dnd.leave, 0, 0, 0, 0, self);
        dnd.target = t;
        dnd.accepted = false;
        if (t) {
            long inl[3] = {0, 0, 0};
            for (std::size_t i = 0; i < 3 && i < dnd.types.size(); ++i) inl[i] = long(dnd.types[i].first);
            dndSend(t, dnd.enter, (5L << 24) | (dnd.types.size() > 3 ? 1 : 0), inl[0], inl[1], inl[2], self);
        }
    }
    if (t) dndSend(t, dnd.position, 0, (long(in.x_root) << 16) | (in.y_root & 0xFFFF), long(in.time), long(dnd.actionCopy), self);
}
/// The button went up during an outgoing drag. True: dropped on another application (the UI drag ends when it
/// answers); false: an ordinary release the UI handles.
bool NativePlatform::Impl::dndOutRelease(Rec& r, const x11::InputEv& in) {
    const x11::Window self = x11::Window(r.handle);
    if (dnd.dropped) return true;
    if (dnd.target && dnd.accepted && r.win->ui().dragging()) {
        dnd.dropped = true;
        dnd.deadline = app.now() + 2.0;
        dndSend(dnd.target, dnd.drop, 0, long(in.time), 0, 0, self);
        return true;
    }
    if (dnd.target) dndSend(dnd.target, dnd.leave, 0, 0, 0, 0, self);
    dnd.rec = nullptr;
    dnd.target = 0;
    return false;
}
void NativePlatform::Impl::dndOutEnd(DropAction result) {
    Rec* r = dnd.rec;
    dnd.rec = nullptr;
    dnd.target = 0;
    dnd.dropped = dnd.accepted = false;
    if (r && r->win) {
        r->win->ui().dragFinishExternal(result);
        r->win->requestRedraw();
    }
}
bool NativePlatform::externalDragActive() const {
    FUSE_APP_WL_NONE(false); // Wayland: drops in only, a drag of the UI never leaves the app (no wl_data_device.start_drag)
    return m->dnd.rec != nullptr && (m->dnd.target != 0 || m->dnd.dropped);
}

// ---- X11: client-side window chrome ------------------------------------------------------------------------------------
/// A left press on a Borderless / Custom window: true when it starts a move / resize (caption, edge) or toggled
/// maximise (caption double click) and must not reach the UI.
bool NativePlatform::Impl::chromePress(Rec& r, const x11::InputEv& in, Vec2 pos) {
    AppWindow& w = *r.win;
    if (w.chrome().frame == WindowFrame::System) return false;
    const HitArea a = chromeHitTest(w, pos);
    if (a == HitArea::Client || a == HitArea::Minimize || a == HitArea::Maximize || a == HitArea::Close) return false; // widgets
    if (a == HitArea::Caption) {
        const bool twice = r.captionClickTime != 0 && in.time - r.captionClickTime < 400;
        r.captionClickTime = twice ? 0 : in.time;
        if (twice) {
            w.command(WindowCommand::ToggleMaximize);
            return true;
        }
    }
    const x11::Window win = x11::Window(r.handle);
    if (hasWm()) { // _NET_WM_MOVERESIZE: the window manager takes the pointer and does the drag
        static constexpr long kDirection[] = {8, 8, 7, 3, 1, 5, 0, 2, 6, 4}; // by HitArea: Client, Caption, L, R, T, B, TL, TR, BL, BR
        x.UngrabPointer(dpy, 0);
        x11::Event ev{};
        ev.client.type = x11::ClientMessage;
        ev.client.display = dpy;
        ev.client.window = win;
        ev.client.message_type = aMoveResize;
        ev.client.format = 32;
        ev.client.l[0] = in.x_root; ev.client.l[1] = in.y_root; ev.client.l[2] = kDirection[int(a)]; ev.client.l[3] = 1 /*button*/;
        ev.client.l[4] = 1 /*source: application*/;
        x.SendEvent(dpy, x.RootWindow(dpy, x.DefaultScreen(dpy)), 0, (1L << 19) | (1L << 20), &ev);
        x.Flush(dpy);
        return true;
    }
    // no window manager: the backend moves / resizes while the button is held (chromeDragMove)
    int ox = 0, oy = 0;
    Rect work;
    if (!screenInfo(r, ox, oy, work)) return true;
    const Size px = w.pixelSize();
    r.chromeDrag = {true, a, in.x_root, in.y_root, ox, oy, int(px.w), int(px.h)};
    return true;
}
void NativePlatform::Impl::chromeDragMove(Rec& r, const x11::InputEv& in) {
    const Rec::ChromeDrag& d = r.chromeDrag;
    const int dx = in.x_root - d.rootX, dy = in.y_root - d.rootY;
    const HitArea a = d.area;
    const bool left = a == HitArea::Left || a == HitArea::TopLeft || a == HitArea::BottomLeft;
    const bool right = a == HitArea::Right || a == HitArea::TopRight || a == HitArea::BottomRight;
    const bool top = a == HitArea::Top || a == HitArea::TopLeft || a == HitArea::TopRight;
    const bool bottom = a == HitArea::Bottom || a == HitArea::BottomLeft || a == HitArea::BottomRight;
    int nx = d.x, ny = d.y, nw = d.w, nh = d.h;
    if (a == HitArea::Caption) { nx += dx; ny += dy; }
    if (left) nw -= dx;
    if (right) nw += dx;
    if (top) nh -= dy;
    if (bottom) nh += dy;
    // the window's size limits (logical units), and never smaller than a title bar's worth
    const Size lim = r.win->clampSize({f32(std::max(nw, 64)) / r.scale, f32(std::max(nh, 32)) / r.scale});
    nw = std::max(1, int(std::lround(lim.w * r.scale)));
    nh = std::max(1, int(std::lround(lim.h * r.scale)));
    if (left) nx = d.x + d.w - nw; // the opposite edge stays put
    if (top) ny = d.y + d.h - nh;
    x.MoveResizeWindow(dpy, x11::Window(r.handle), nx, ny, unsigned(nw), unsigned(nh));
    x.Flush(dpy);
}

// ---- X11: tool windows, synthetic input ---------------------------------------------------------------------------------
bool NativePlatform::Impl::createToolOs(Rec& r, Rec::Popup& p) {
    const int scr = x.DefaultScreen(dpy);
    const x11::Window w = x.CreateSimpleWindow(dpy, x.RootWindow(dpy, scr), int(p.screen.x), int(p.screen.y), unsigned(p.screen.w),
                                               unsigned(p.screen.h), 0, 0, 0);
    if (!w) return false;
    x11::SetWindowAttributes a{};
    a.override_redirect = 1; // placed by the app (the panel's own title bar drags it); no WM frame, no focus of its own
    x.ChangeWindowAttributes(dpy, w, x11::kCWOverrideRedirect, &a);
    const x11::Atom type = x.InternAtom(dpy, "_NET_WM_WINDOW_TYPE", 0), utility = x.InternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", 0);
    x.ChangeProperty(dpy, w, type, 4 /*XA_ATOM*/, 32, 0, reinterpret_cast<const unsigned char*>(&utility), 1);
    const long owner = long(r.handle);
    x.ChangeProperty(dpy, w, 68 /*XA_WM_TRANSIENT_FOR*/, 33 /*XA_WINDOW*/, 32, 0, reinterpret_cast<const unsigned char*>(&owner), 1);
    x.SelectInput(dpy, w, x11::kPopupMask | 1 | 2); // + keys: a window manager may still give it the focus
    if (x.DefineCursor)
        if (const unsigned long arrow = cursorFor(Cursor::Arrow)) x.DefineCursor(dpy, w, arrow);
    x.MapWindow(dpy, w);
    x.Flush(dpy);
    p.handle = w;
    return true;
}
void NativePlatform::Impl::raiseToolOs(Rec::Popup& p) {
    if (p.handle && x.RaiseWindow) x.RaiseWindow(dpy, x11::Window(p.handle));
}
bool NativePlatform::Impl::postInputOs(unsigned long long h, Vec2 at, int kind) {
    if (!dpy) return false;
    x11::Event ev{};
    const x11::Window root = x.RootWindow(dpy, x.DefaultScreen(dpy));
    if (kind == 3) { // what the window manager sends for its close button
        ev.client.type = x11::ClientMessage;
        ev.client.display = dpy;
        ev.client.window = x11::Window(h);
        ev.client.message_type = wmProtocols;
        ev.client.format = 32;
        ev.client.l[0] = long(wmDelete);
    } else {
        int rx = 0, ry = 0;
        x11::Window child = 0;
        x.TranslateCoordinates(dpy, x11::Window(h), root, int(at.x), int(at.y), &rx, &ry, &child);
        ev.input.type = kind == 0 ? x11::MotionNotify : kind == 1 ? x11::ButtonPress : x11::ButtonRelease;
        ev.input.display = dpy;
        ev.input.window = x11::Window(h);
        ev.input.root = root;
        ev.input.x = int(at.x);
        ev.input.y = int(at.y);
        ev.input.x_root = rx;
        ev.input.y_root = ry;
        ev.input.detail = kind == 0 ? 0 : 1;
        ev.input.same_screen = 1;
    }
    if (!x.SendEvent(dpy, x11::Window(h), 0, 0, &ev)) return false;
    x.Flush(dpy);
    return true;
}

// ---- POSIX: window-less dialogs through the desktop's helper programs ----------------------------------------------------
namespace {
bool haveDisplay() {
    const char* xd = std::getenv("DISPLAY");
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    return (xd && *xd) || (wd && *wd);
}
/// Runs the first installed helper of `choices` and returns its trimmed stdout.
std::expected<std::string, PlatformError> runDialog(std::initializer_list<std::vector<std::string>> choices, int* exitCode = nullptr) {
    if (!haveDisplay()) return std::unexpected(PlatformError::Unavailable);
    for (const std::vector<std::string>& args : choices) {
        if (args.empty() || !findInPath(args[0])) continue;
        std::string out;
        int code = -1;
        if (!runCapture(args, out, code)) continue;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (exitCode) *exitCode = code;
        else if (code != 0 || out.empty()) return std::unexpected(PlatformError::Cancelled);
        return out;
    }
    return std::unexpected(PlatformError::Unavailable);
}
} // namespace

std::expected<std::vector<std::string>, PlatformError> openFileDialog(const FileDialogDesc& d) {
    auto r = runDialog({zenityFileArgs(d, DialogKind::Open), kdialogFileArgs(d, DialogKind::Open)});
    if (!r) return std::unexpected(r.error());
    std::vector<std::string> paths = d.multiple ? splitDialogPaths(*r) : std::vector<std::string>{*r};
    if (paths.empty()) return std::unexpected(PlatformError::Cancelled);
    return paths;
}
std::expected<std::string, PlatformError> saveFileDialog(const FileDialogDesc& d) {
    return runDialog({zenityFileArgs(d, DialogKind::Save), kdialogFileArgs(d, DialogKind::Save)});
}
std::expected<std::string, PlatformError> pickFolderDialog(const FileDialogDesc& d) {
    return runDialog({zenityFileArgs(d, DialogKind::Folder), kdialogFileArgs(d, DialogKind::Folder)});
}
std::expected<Color, PlatformError> colorDialog(const ColorDialogDesc& d) {
    auto r = runDialog({zenityColorArgs(d), kdialogColorArgs(d)});
    if (!r) return std::unexpected(r.error());
    Color c;
    if (!parseDialogColor(*r, d.initial.a, c)) return std::unexpected(PlatformError::Failed);
    return c;
}
std::expected<MessageResult, PlatformError> messageBox(std::string_view title, std::string_view text, MessageIcon icon, MessageButtons buttons,
                                                       unsigned long long) {
    int code = -1;
    auto r = runDialog({zenityMessageArgs(title, text, icon, buttons)}, &code);
    if (!r) return std::unexpected(r.error());
    const bool yesNo = buttons == MessageButtons::YesNo || buttons == MessageButtons::YesNoCancel;
    if (code == 0) return yesNo ? MessageResult::Yes : MessageResult::Ok;
    if (buttons == MessageButtons::Ok) return MessageResult::Ok; // closed: the only answer there is
    if (buttons == MessageButtons::YesNoCancel) return *r == "Cancel" ? MessageResult::Cancel : MessageResult::No; // the extra button prints its label
    return yesNo ? MessageResult::No : MessageResult::Cancel;
}
std::expected<void, PlatformError> revealInFileManager(std::string_view path) {
    if (path.empty()) return std::unexpected(PlatformError::InvalidArgument);
    if (::access(std::string(path).c_str(), F_OK) != 0) return std::unexpected(PlatformError::Failed);
    if (!revealFile(path)) return std::unexpected(PlatformError::Unavailable);
    return {};
}
#endif
} // namespace fuse::app

#if defined(_WIN32)
#include "native_platform_win32.inl"
#endif
