// fuse_app native backend for macOS (Cocoa) and iOS (UIKit) (plan UI1 / UI5). Objective-C++, ARC.
//
// Windows: an NSWindow with a layer-backed FuseView on macOS; on iOS the screen's UIWindow with a FuseView under a
// plain view controller (one OS window per process: the first AppWindow shown owns it). The Application's event
// source pumps the platform run loop for at most the wait it is given (NSApp nextEventMatchingMask on macOS,
// CFRunLoopRunInMode on iOS), so the on-demand loop sleeps exactly as on the other platforms.
//
// Presentation: the shared Vulkan swapchain presenter over a CAMetalLayer sublayer (VK_EXT_metal_surface, MoltenVK)
// when the build has a Vulkan loader and a device can present; otherwise the CPU raster becomes the view layer's
// contents (a CGImage). The UI lays out in logical units (points unless NativeOptions::scale says otherwise) and the
// frame is rendered at the display's pixel size (points x backing scale; AppWindow::resizePixels), so it is shown 1:1.
//
// iOS entry: UIApplicationMain never returns, so the app's main (fuse_app_main, see fuse/app/main.hpp) runs from the
// app delegate after launch and pumps the run loop itself; when it returns the process exits with its code.
//
// Platform services (plan §4.13). macOS: NSCursor through the view's cursor rect, NSWindow management + delegate
// notifications, the native menu bar from AppWindow::setMenu (NSMenu), effectiveAppearance / controlAccentColor,
// NSScreen, NSOpenPanel / NSSavePanel, NSWorkspace. iOS: one fullscreen window (every window request answers
// "cannot"), UITraitCollection for the theme, UIApplication openURL; no cursor, no file dialog.
#include "native_internal.hpp"

#include <TargetConditionals.h>

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#if defined(FUSE_APP_HAS_VULKAN)
#import <QuartzCore/CAMetalLayer.h>
#endif
#if TARGET_OS_OSX
#import <Cocoa/Cocoa.h>
#import <objc/message.h>
#import <objc/runtime.h>
#else
#import <UIKit/UIKit.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#if TARGET_OS_OSX
@class FuseView;
@class FuseWindowDelegate;
@class FuseMenuTarget;
using FuseNativeWindow = NSWindow;
#else
@class FuseView;
@class FuseViewController;
using FuseNativeWindow = UIWindow;
#endif

namespace fuse::app {
using namespace fuse::ui;
using detail::SurfaceSource;

struct NativePlatform::Impl {
    struct Rec {
        Impl* impl = nullptr;
        AppWindow* win = nullptr;
        detail::VkWin* vk = nullptr;
        PresenterKind kind = PresenterKind::Headless;
        FuseNativeWindow* window = nil;
        FuseView* view = nil;
        CALayer* metal = nil; ///< CAMetalLayer sublayer (Vulkan presenter only)
        f32 scale = 1.f;      ///< UI units -> view points is 1 / pointScale; UI units -> pixels is `scale`
        f32 pointScale = 1.f; ///< UI units per view point (1 unless NativeOptions::scale overrides the OS scale)
        bool keyboard = false;
        fuse::ui::Cursor cursor = fuse::ui::Cursor::Arrow; ///< shape the view's cursor rect shows (macOS)
#if TARGET_OS_OSX
        FuseWindowDelegate* delegate = nil;
        FuseMenuTarget* menuTarget = nil; ///< target of this window's native menu items
#else
        FuseViewController* controller = nil;
#endif
    };

    Application& app;
    NativeOptions opts;
    std::vector<Rec*> recs;
    detail::VkCtx* vk = nullptr;
    bool vkTried = false;
    std::string error;

    Impl(Application& a, const NativeOptions& o) : app(a), opts(o) {}

    const Rec* find(const AppWindow& w) const {
        for (const Rec* r : recs)
            if (r->win == &w) return r;
        return nullptr;
    }
    detail::VkCtx* vkContext() {
        if (!vkTried && opts.vulkan) {
            vkTried = true;
            vk = opts.adoptDevice.instance && opts.adoptDevice.device
                     ? detail::vkCtxAdopt(SurfaceSource::Metal, opts.adoptDevice, error)
                     : detail::vkCtxCreate(SurfaceSource::Metal, error);
        }
        return vk;
    }

    static bool present(void* user, AppWindow& w, Rect damage);
    static void destroy(void* user);
    static bool pump(void* user, Application& app, f32 wait);
    bool createOsWindow(Rec& r);
    void destroyOsWindow(Rec& r);
    /// View bounds changed (or first layout): resize the AppWindow and the metal layer.
    void layout(Rec& r, CGSize points);
    void syncKeyboard(Rec& r);
    static bool windowRequest(void* user, AppWindow& w, WindowRequest what);
#if TARGET_OS_OSX
    void applyCursor(Rec& r);
    /// NSApp.mainMenu = the application menu + `r`'s MenuDesc (null / empty: the application menu only).
    void setMainMenu(Rec* r);
#endif
};

namespace {
using Rec = NativePlatform::Impl::Rec;

/// View point -> UI units.
Vec2 toUi(const Rec& r, CGPoint p) { return {f32(p.x) * r.pointScale, f32(p.y) * r.pointScale}; }

/// Shows the CPU raster: the image becomes the layer's contents (Image pixels are R, G, B, A bytes, premultiplied).
void showImage(CALayer* layer, const Image& img) {
    if (!layer || img.width <= 0 || img.height <= 0) return;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(img.pixels.data()),
                                  CFIndex(img.pixels.size() * sizeof(u32)));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef image = CGImageCreate(std::size_t(img.width), std::size_t(img.height), 8, 32, std::size_t(img.width) * 4u, space,
                                     CGBitmapInfo(kCGBitmapByteOrderDefault) | CGBitmapInfo(kCGImageAlphaPremultipliedLast), provider,
                                     nullptr, false, kCGRenderingIntentDefault);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.contents = (__bridge id)image;
    [CATransaction commit];
    CGImageRelease(image);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);
}

std::string resourcePath(std::string_view rel) {
    while (rel.size() > 1 && rel[0] == '.' && rel[1] == '/') rel.remove_prefix(2);
    NSString* base = [[NSBundle mainBundle] resourcePath];
    if (!base || rel.empty() || rel[0] == '/') return {};
    return std::string(base.UTF8String) + "/" + std::string(rel);
}
template <class Out>
bool readFile(const std::string& path, Out& out) {
    if (path.empty()) return false;
    NSData* d = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
    if (!d) return false;
    const auto* bytes = static_cast<const typename Out::value_type*>(d.bytes);
    out.assign(bytes, bytes + d.length);
    return true;
}
bool readBundleResource(void*, std::string_view path, std::string& out) { return readFile(resourcePath(path), out); }
bool readBundleFont(void*, std::string_view name, std::vector<std::uint8_t>& out) {
    return readFile(resourcePath("fonts/" + std::string(name)), out);
}
/// App bundles carry their resources (and optionally the bundled fonts under Resources/fonts): install the readers.
void installBundleReaders() {
    static bool done = false;
    if (done) return;
    done = true;
    setResourceReader({nullptr, &readBundleResource});
    fuse::text::setFontDataSource({nullptr, &readBundleFont});
#if !TARGET_OS_OSX
    NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    if (dirs.count) setDataRoot(dirs[0].UTF8String);
#endif
}

// ---- clipboard -------------------------------------------------------------------------------------------------------
bool clipboardSet(void*, std::string_view utf8) {
    NSString* s = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
    if (!s) return false;
#if TARGET_OS_OSX
    NSPasteboard* pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    return [pb setString:s forType:NSPasteboardTypeString];
#else
    [UIPasteboard generalPasteboard].string = s;
    return true;
#endif
}
bool clipboardGet(void*, std::string& out) {
#if TARGET_OS_OSX
    NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
#else
    NSString* s = [UIPasteboard generalPasteboard].string;
#endif
    if (!s) return false;
    out = s.UTF8String;
    return true;
}

#if TARGET_OS_OSX
// ---- macOS platform services: helpers the Objective-C classes below use ----------------------------------------------
WindowMode macMode(NSWindow* win) {
    if (!win) return WindowMode::Normal;
    if (win.styleMask & NSWindowStyleMaskFullScreen) return WindowMode::Fullscreen;
    if (win.miniaturized) return WindowMode::Minimized;
    return win.zoomed ? WindowMode::Maximized : WindowMode::Normal;
}
/// Top-left of the content area in a top-left-origin screen space, UI units.
Vec2 macOrigin(const Rec& r) {
    if (!r.window) return {};
    const NSRect content = [r.window contentRectForFrameRect:r.window.frame];
    NSScreen* screen = r.window.screen ?: NSScreen.mainScreen;
    const CGFloat top = screen ? NSMaxY(screen.frame) : NSMaxY(content);
    return {f32(NSMinX(content)) * r.pointScale, f32(top - NSMaxY(content)) * r.pointScale};
}
SystemTheme macTheme() {
    SystemTheme t;
    if (@available(macOS 10.14, *)) {
        NSAppearanceName best = [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
        t.scheme = [best isEqualToString:NSAppearanceNameDarkAqua] ? ColorScheme::Dark : ColorScheme::Light;
        NSColor* c = [[NSColor controlAccentColor] colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        if (c) {
            t.hasAccent = true;
            t.accent = {f32(c.redComponent), f32(c.greenComponent), f32(c.blueComponent), 1.f};
        }
    }
    return t;
}
/// A class-method cursor AppKit has but does not declare (diagonal resize, busy); `fallback` when it is gone.
NSCursor* privateCursor(const char* selector, NSCursor* fallback) {
    const SEL sel = sel_registerName(selector);
    if (![NSCursor respondsToSelector:sel]) return fallback;
    const id c = ((id (*)(id, SEL))objc_msgSend)([NSCursor class], sel);
    return [c isKindOfClass:[NSCursor class]] ? (NSCursor*)c : fallback;
}
NSCursor* macCursor(fuse::ui::Cursor c) {
    switch (c) {
    case fuse::ui::Cursor::IBeam: return [NSCursor IBeamCursor];
    case fuse::ui::Cursor::PointingHand: return [NSCursor pointingHandCursor];
    case fuse::ui::Cursor::ResizeH: return [NSCursor resizeLeftRightCursor];
    case fuse::ui::Cursor::ResizeV: return [NSCursor resizeUpDownCursor];
    case fuse::ui::Cursor::ResizeNWSE: return privateCursor("_windowResizeNorthWestSouthEastCursor", [NSCursor crosshairCursor]);
    case fuse::ui::Cursor::ResizeNESW: return privateCursor("_windowResizeNorthEastSouthWestCursor", [NSCursor crosshairCursor]);
    case fuse::ui::Cursor::Move: return [NSCursor openHandCursor];
    case fuse::ui::Cursor::Crosshair: return [NSCursor crosshairCursor];
    case fuse::ui::Cursor::NotAllowed: return [NSCursor operationNotAllowedCursor];
    case fuse::ui::Cursor::Wait: return privateCursor("busyButClickableCursor", [NSCursor arrowCursor]);
    case fuse::ui::Cursor::Hidden: {
        static NSCursor* blank = [[NSCursor alloc] initWithImage:[[NSImage alloc] initWithSize:NSMakeSize(1, 1)] hotSpot:NSZeroPoint];
        return blank;
    }
    default: return [NSCursor arrowCursor];
    }
}
/// UI key -> NSMenuItem key equivalent (nil = none).
NSString* macKeyEquivalent(Key k) {
    auto ch = [](unichar c) { return [NSString stringWithCharacters:&c length:1]; };
    switch (k) {
    case Key::None: case Key::Other: return nil;
    case Key::Enter: return @"\r";
    case Key::Tab: return @"\t";
    case Key::Escape: return ch(0x1B);
    case Key::Space: return @" ";
    case Key::Backspace: return ch(0x08);
    case Key::Delete: return ch(NSDeleteFunctionKey);
    case Key::Left: return ch(NSLeftArrowFunctionKey);
    case Key::Right: return ch(NSRightArrowFunctionKey);
    case Key::Up: return ch(NSUpArrowFunctionKey);
    case Key::Down: return ch(NSDownArrowFunctionKey);
    case Key::Home: return ch(NSHomeFunctionKey);
    case Key::End: return ch(NSEndFunctionKey);
    case Key::PageUp: return ch(NSPageUpFunctionKey);
    case Key::PageDown: return ch(NSPageDownFunctionKey);
    case Key::BracketLeft: return @"[";
    case Key::BracketRight: return @"]";
    default: break;
    }
    const std::string text = shortcutText(k, 0); // "S", "7", "F5"
    if (text.size() == 1) return [[NSString stringWithUTF8String:text.c_str()] lowercaseString];
    if (text.size() >= 2 && text[0] == 'F') return ch(unichar(NSF1FunctionKey + std::atoi(text.c_str() + 1) - 1));
    return nil;
}
#endif
} // namespace
} // namespace fuse::app

using fuse::app::NativePlatform;
using FuseRec = fuse::app::NativePlatform::Impl::Rec;
namespace fui = fuse::ui;

#if TARGET_OS_OSX
// =====================================================================================================================
// macOS
// =====================================================================================================================
namespace {
fui::u8 macMods(NSEventModifierFlags f) {
    // Qt's convention: Command drives the Ctrl shortcuts (copy / paste / undo), Control reports as Super.
    return fui::u8(((f & NSEventModifierFlagShift) ? fui::kModShift : 0) | ((f & NSEventModifierFlagCommand) ? fui::kModCtrl : 0) |
                   ((f & NSEventModifierFlagOption) ? fui::kModAlt : 0) | ((f & NSEventModifierFlagControl) ? fui::kModSuper : 0));
}
/// Virtual key codes (HIToolbox kVK_*; layout independent for these keys).
fui::Key macKey(unsigned short code, NSString* chars) {
    using fui::Key;
    switch (code) {
    case 0x24: case 0x4C: return Key::Enter;
    case 0x30: return Key::Tab;
    case 0x31: return Key::Space;
    case 0x33: return Key::Backspace;
    case 0x35: return Key::Escape;
    case 0x75: return Key::Delete;
    case 0x7B: return Key::Left;
    case 0x7C: return Key::Right;
    case 0x7D: return Key::Down;
    case 0x7E: return Key::Up;
    case 0x73: return Key::Home;
    case 0x77: return Key::End;
    case 0x74: return Key::PageUp;
    case 0x79: return Key::PageDown;
    case 0x7A: return Key::F1;
    case 0x78: return Key::F2;
    case 0x63: return Key::F3;
    case 0x76: return Key::F4;
    case 0x60: return Key::F5;
    case 0x61: return Key::F6;
    case 0x62: return Key::F7;
    case 0x64: return Key::F8;
    case 0x65: return Key::F9;
    case 0x6D: return Key::F10;
    case 0x67: return Key::F11;
    case 0x6F: return Key::F12;
    default: break;
    }
    Key k = Key::Other;
    if (chars.length > 0) fuse::app::detail::asciiKey(k, fui::u32([chars characterAtIndex:0]));
    return k;
}
} // namespace

@interface FuseView : NSView <NSTextInputClient> {
@public
    FuseRec* rec;
    NSTrackingArea* tracking;
    NSString* marked;
}
@end

@interface FuseWindowDelegate : NSObject <NSWindowDelegate> {
@public
    FuseRec* rec;
}
@end

@interface FuseAppTarget : NSObject
+ (void)quit:(id)sender;
+ (void)themeChanged:(NSNotification*)note;
+ (void)applyTheme;
@end

/// Target of the items of one window's native menu (AppWindow::setMenu): the item's tag is the MenuItem id.
@interface FuseMenuTarget : NSObject {
@public
    fuse::app::AppWindow* win;
}
- (void)activate:(NSMenuItem*)item;
@end

static NativePlatform::Impl* g_macImpl = nullptr;

@implementation FuseAppTarget
+ (void)quit:(id)sender {
    (void)sender;
    if (!g_macImpl) return;
    for (FuseRec* r : g_macImpl->recs) r->win->requestClose();
    g_macImpl->app.quit();
}
+ (void)themeChanged:(NSNotification*)note {
    (void)note; // the effective appearance follows a moment later
    [self performSelector:@selector(applyTheme) withObject:nil afterDelay:0.1];
}
+ (void)applyTheme {
    if (g_macImpl) g_macImpl->app.setSystemTheme(fuse::app::macTheme());
}
@end

@implementation FuseMenuTarget
- (void)activate:(NSMenuItem*)item {
    if (!win) return;
    // a checkable item rebuilds the bar: the menu that sent this stays alive until we return
    NSMenu* keep __attribute__((objc_precise_lifetime)) = NSApp.mainMenu;
    win->activateMenuItem(fui::u32(item.tag));
    win->requestRedraw();
    (void)keep;
}
@end

@implementation FuseWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (rec && rec->win) rec->win->tryClose(); // the close guard may veto
    return NO; // the Application drops the window (and its presenter) on the next step
}
- (void)windowDidBecomeKey:(NSNotification*)note {
    (void)note;
    if (!rec || !rec->win) return;
    if (!rec->win->menu().empty()) rec->impl->setMainMenu(rec); // the menu bar shows the key window's menu
    rec->win->notifyActive(true);
}
- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    if (rec && rec->win) rec->win->notifyActive(false);
}
- (void)windowDidMove:(NSNotification*)note {
    (void)note;
    if (rec && rec->win) rec->win->notifyMoved(fuse::app::macOrigin(*rec));
}
- (void)syncMode {
    if (rec && rec->win) rec->win->notifyMode(fuse::app::macMode(rec->window));
}
- (void)windowDidMiniaturize:(NSNotification*)note { (void)note; [self syncMode]; }
- (void)windowDidDeminiaturize:(NSNotification*)note { (void)note; [self syncMode]; }
- (void)windowDidEnterFullScreen:(NSNotification*)note { (void)note; [self syncMode]; }
- (void)windowDidExitFullScreen:(NSNotification*)note { (void)note; [self syncMode]; }
- (void)windowDidResize:(NSNotification*)note { (void)note; [self syncMode]; } // zoomed / unzoomed
- (void)windowDidChangeBackingProperties:(NSNotification*)note {
    (void)note;
    if (rec && rec->view) rec->impl->layout(*rec, rec->view.bounds.size);
}
@end

@implementation FuseView
- (BOOL)isFlipped { return YES; } // top-left origin, like the UI
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (BOOL)isOpaque { return YES; }
- (void)resetCursorRects { // the UI's cursor over the whole view (Impl::applyCursor invalidates it on a change)
    [super resetCursorRects];
    if (rec) [self addCursorRect:self.bounds cursor:fuse::app::macCursor(rec->cursor)];
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (rec) rec->impl->layout(*rec, size);
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (tracking) [self removeTrackingArea:tracking];
    tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                            options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                                                    NSTrackingInVisibleRect
                                              owner:self
                                           userInfo:nil];
    [self addTrackingArea:tracking];
}
- (fui::Vec2)uiPoint:(NSEvent*)event {
    return fuse::app::toUi(*rec, [self convertPoint:event.locationInWindow fromView:nil]);
}
- (void)pointer:(NSEvent*)event down:(BOOL)down up:(BOOL)up button:(fui::MouseButton)button {
    if (!rec || !rec->win) return;
    fui::UiContext& ui = rec->win->ui();
    ui.setModifiers(macMods(event.modifierFlags));
    const fui::Vec2 p = [self uiPoint:event];
    if (down) {
        ui.pointerMove(p);
        ui.pointerDown(p, button);
    } else if (up) {
        ui.pointerUp(p, button);
    } else {
        ui.pointerMove(p);
    }
    rec->win->requestRedraw();
}
- (void)mouseDown:(NSEvent*)e { [self pointer:e down:YES up:NO button:fui::MouseButton::Left]; }
- (void)mouseUp:(NSEvent*)e { [self pointer:e down:NO up:YES button:fui::MouseButton::Left]; }
- (void)rightMouseDown:(NSEvent*)e { [self pointer:e down:YES up:NO button:fui::MouseButton::Right]; }
- (void)rightMouseUp:(NSEvent*)e { [self pointer:e down:NO up:YES button:fui::MouseButton::Right]; }
- (void)otherMouseDown:(NSEvent*)e { [self pointer:e down:YES up:NO button:fui::MouseButton::Middle]; }
- (void)otherMouseUp:(NSEvent*)e { [self pointer:e down:NO up:YES button:fui::MouseButton::Middle]; }
- (void)mouseMoved:(NSEvent*)e { [self pointer:e down:NO up:NO button:fui::MouseButton::Left]; }
- (void)mouseDragged:(NSEvent*)e { [self pointer:e down:NO up:NO button:fui::MouseButton::Left]; }
- (void)rightMouseDragged:(NSEvent*)e { [self pointer:e down:NO up:NO button:fui::MouseButton::Right]; }
- (void)otherMouseDragged:(NSEvent*)e { [self pointer:e down:NO up:NO button:fui::MouseButton::Middle]; }
- (void)mouseExited:(NSEvent*)e {
    (void)e;
    if (!rec || !rec->win) return;
    rec->win->ui().pointerMove({-1.f, -1.f});
    rec->win->requestRedraw();
}
- (void)scrollWheel:(NSEvent*)e {
    if (!rec || !rec->win) return;
    fui::UiContext& ui = rec->win->ui();
    ui.setModifiers(macMods(e.modifierFlags));
    // precise (trackpad) deltas are points: ~10 points per wheel notch; a mouse wheel reports lines
    const fui::f32 k = e.hasPreciseScrollingDeltas ? 0.1f : 1.f;
    ui.wheel([self uiPoint:e], {fui::f32(-e.scrollingDeltaX) * k, fui::f32(e.scrollingDeltaY) * k});
    rec->win->requestRedraw();
}
- (void)keyDown:(NSEvent*)e {
    if (!rec || !rec->win) return;
    fui::UiContext& ui = rec->win->ui();
    const fui::u8 md = macMods(e.modifierFlags);
    const fui::Key k = macKey(e.keyCode, e.charactersIgnoringModifiers);
    if (k != fui::Key::Other && !ui.keyDown(k, md)) rec->win->menuShortcut(k, md); // unconsumed: a menu shortcut
    // text (and IME composition) through the input context; shortcuts are not text
    if (!(e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) [self interpretKeyEvents:@[e]];
    rec->win->requestRedraw();
}
- (void)keyUp:(NSEvent*)e {
    if (!rec || !rec->win) return;
    const fui::Key k = macKey(e.keyCode, e.charactersIgnoringModifiers);
    if (k != fui::Key::Other) rec->win->ui().keyUp(k, macMods(e.modifierFlags));
    rec->win->requestRedraw();
}
- (void)flagsChanged:(NSEvent*)e {
    if (rec && rec->win) rec->win->ui().setModifiers(macMods(e.modifierFlags));
}
// A Command shortcut whose key the UI consumed must not also beep / reach the menu.
- (BOOL)performKeyEquivalent:(NSEvent*)e {
    if (!rec || !rec->win || self.window.firstResponder != self) return [super performKeyEquivalent:e];
    if (!(e.modifierFlags & NSEventModifierFlagCommand)) return [super performKeyEquivalent:e];
    const fui::Key k = macKey(e.keyCode, e.charactersIgnoringModifiers);
    if (k == fui::Key::Other || k == fui::Key::Q) return [super performKeyEquivalent:e]; // Cmd+Q stays the app menu's
    const fui::u8 md = macMods(e.modifierFlags);
    const bool used = rec->win->ui().keyDown(k, md);
    rec->win->ui().keyUp(k, md);
    rec->win->requestRedraw();
    return used ? YES : [super performKeyEquivalent:e];
}

// ---- NSTextInputClient (typed text and IME pre-edit) ---------------------------------------------------------------
- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    if (!rec || !rec->win) return;
    NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : (NSString*)string;
    marked = nil;
    fui::UiContext& ui = rec->win->ui();
    ui.textComposition({}, 0);
    std::string out;
    for (const char* c = s.UTF8String; c && *c; ++c)
        if (static_cast<unsigned char>(*c) >= 0x20 && *c != 0x7F) out += *c; // control characters arrive as keys
    if (!out.empty()) ui.textInput(out);
    rec->win->requestRedraw();
}
- (void)doCommandBySelector:(SEL)selector {
    (void)selector; // navigation / editing commands were already delivered as key events
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)selected replacementRange:(NSRange)range {
    (void)range;
    if (!rec || !rec->win) return;
    NSString* s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : (NSString*)string;
    marked = s.length ? [s copy] : nil;
    const NSUInteger caretUnits = std::min<NSUInteger>(selected.location, s.length);
    const NSUInteger caretBytes = [[s substringToIndex:caretUnits] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    rec->win->ui().textComposition(s.UTF8String ? s.UTF8String : "", fui::i32(caretBytes));
    rec->win->requestRedraw();
}
- (void)unmarkText {
    marked = nil;
    if (rec && rec->win) rec->win->ui().textComposition({}, 0);
}
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange { return marked ? NSMakeRange(0, marked.length) : NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return marked != nil; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)range;
    (void)actual;
    return nil;
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
    (void)range;
    (void)actual;
    if (!rec || !rec->win) return NSZeroRect;
    // the caret in screen coordinates: where the IME puts its candidate window
    const fui::Rect c = rec->win->ui().textCaret();
    const fui::f32 s = rec->pointScale > 0.f ? rec->pointScale : 1.f;
    const NSRect inView = NSMakeRect(c.x / s, c.y / s, std::max(1.f, c.w / s), std::max(1.f, c.h / s));
    return [self.window convertRectToScreen:[self convertRect:inView toView:nil]];
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return NSNotFound;
}
@end

namespace fuse::app {

void NativePlatform::Impl::layout(Rec& r, CGSize points) {
    if (!r.win || points.width < 1 || points.height < 1) return;
    const f32 backing = r.window ? f32(r.window.backingScaleFactor) : 1.f;
    // UI units per point: 1 by default (UI units are points, rendered at the backing scale); an explicit
    // NativeOptions::scale s means UI units = pixels / s.
    r.pointScale = opts.scale > 0.f ? backing / opts.scale : 1.f;
    r.scale = backing / r.pointScale;
    const u32 pw = u32(std::max(1.0, std::round(points.width * backing))), ph = u32(std::max(1.0, std::round(points.height * backing)));
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (r.metal) {
        r.metal.frame = CGRectMake(0, 0, points.width, points.height);
        r.metal.contentsScale = backing; // the surface's natural extent (bounds x contentsScale) is the pixel size
#if defined(FUSE_APP_HAS_VULKAN)
        static_cast<CAMetalLayer*>(r.metal).drawableSize = CGSizeMake(pw, ph); // the swapchain extent = physical pixels
#endif
    }
    [CATransaction commit];
    const Size px = r.win->pixelSize();
    if (u32(px.w) != pw || u32(px.h) != ph || r.win->scale() != r.scale) r.win->resizePixels(pw, ph, r.scale);
    r.win->requestRedraw();
}

bool NativePlatform::Impl::createOsWindow(Rec& r) {
    @autoreleasepool {
        const Size sz = r.win->size();
        const NSRect content = NSMakeRect(0, 0, sz.w, sz.h);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:content
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        if (!window) {
            error = "NSWindow creation failed";
            return false;
        }
        window.releasedWhenClosed = NO;
        window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary; // AppWindow::setFullscreen
        window.title = [NSString stringWithUTF8String:r.win->title().c_str()] ?: @"";
        FuseView* view = [[FuseView alloc] initWithFrame:content];
        view->rec = &r;
        view.wantsLayer = YES;
        view.layer.contentsGravity = kCAGravityResize;
        view.layer.magnificationFilter = kCAFilterLinear;
        view.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        FuseWindowDelegate* delegate = [[FuseWindowDelegate alloc] init];
        delegate->rec = &r;
        window.delegate = delegate;
        window.contentView = view;
        window.acceptsMouseMovedEvents = YES;
        [window makeFirstResponder:view];
        [window center];
        r.window = window;
        r.view = view;
        r.delegate = delegate;
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    return true;
}
void NativePlatform::Impl::destroyOsWindow(Rec& r) {
    @autoreleasepool {
        if (r.view) r.view->rec = nullptr;
        if (r.delegate) r.delegate->rec = nullptr;
        if (r.menuTarget) { // its menu leaves the bar with the window
            r.menuTarget->win = nullptr;
            r.menuTarget = nil;
            setMainMenu(nullptr);
        }
        if (r.window) {
            r.window.delegate = nil;
            [r.window orderOut:nil];
            [r.window close];
        }
        r.metal = nil;
        r.view = nil;
        r.delegate = nil;
        r.window = nil;
    }
}
void NativePlatform::Impl::syncKeyboard(Rec&) {} // the hardware keyboard is always there; the IME follows the caret

bool NativePlatform::Impl::pump(void* user, Application&, f32 wait) {
    Impl& m = *static_cast<Impl*>(user);
    @autoreleasepool {
        NSDate* until = wait <= 0.f ? [NSDate distantPast]
                        : std::isinf(wait) ? [NSDate distantFuture] : [NSDate dateWithTimeIntervalSinceNow:double(wait)];
        for (;;) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e) break;
            [NSApp sendEvent:e];
            until = [NSDate distantPast]; // drain what is queued, then return to the loop
        }
        for (Rec* r : m.recs) m.applyCursor(*r); // what the UI wants after this batch of input
    }
    return true;
}

// ---- macOS platform services -------------------------------------------------------------------------------------------
namespace {
/// The menu bar every app has: the application menu with Quit (Cmd+Q).
NSMenu* baseMenuBar(const std::string& appName) {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    NSString* name = [NSString stringWithUTF8String:appName.c_str()] ?: @"FUSE";
    NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(quit:) keyEquivalent:@"q"];
    quit.target = [FuseAppTarget class];
    [appMenu addItem:quit];
    appItem.submenu = appMenu;
    return bar;
}
void fillMenu(NSMenu* menu, const std::vector<MenuItem>& items, FuseMenuTarget* target) {
    menu.autoenablesItems = NO; // enabled / disabled comes from the description
    for (const MenuItem& it : items) {
        if (it.separator) {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSString* title = [NSString stringWithUTF8String:it.title.c_str()] ?: @"";
        NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
        mi.enabled = it.enabled;
        if (!it.items.empty()) {
            NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
            fillMenu(sub, it.items, target);
            mi.submenu = sub;
        } else {
            mi.target = target;
            mi.action = @selector(activate:);
            mi.tag = NSInteger(it.id);
            mi.state = it.checked ? NSControlStateValueOn : NSControlStateValueOff;
            if (NSString* key = macKeyEquivalent(it.key)) {
                mi.keyEquivalent = key;
                mi.keyEquivalentModifierMask = ((it.mods & kModCtrl) ? NSEventModifierFlagCommand : 0) | ((it.mods & kModShift) ? NSEventModifierFlagShift : 0) |
                                               ((it.mods & kModAlt) ? NSEventModifierFlagOption : 0) | ((it.mods & kModSuper) ? NSEventModifierFlagControl : 0);
            }
        }
        [menu addItem:mi];
    }
}
} // namespace

void NativePlatform::Impl::setMainMenu(Rec* r) {
    @autoreleasepool {
        NSMenu* bar = baseMenuBar(app.name());
        if (r && r->win && !r->win->menu().empty()) {
            if (!r->menuTarget) r->menuTarget = [[FuseMenuTarget alloc] init];
            r->menuTarget->win = r->win;
            for (const MenuItem& top : r->win->menu().menus) {
                NSString* title = [NSString stringWithUTF8String:top.title.c_str()] ?: @"";
                NSMenuItem* holder = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
                NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
                fillMenu(sub, top.items, r->menuTarget);
                holder.submenu = sub;
                holder.enabled = top.enabled;
                [bar addItem:holder];
            }
        }
        NSApp.mainMenu = bar;
    }
}
void NativePlatform::Impl::applyCursor(Rec& r) {
    if (!r.win || !r.window || !r.view) return;
    const fuse::ui::Cursor want = r.win->ui().cursor();
    if (want == r.cursor) return;
    r.cursor = want;
    [r.window invalidateCursorRectsForView:r.view]; // -> FuseView resetCursorRects
}
bool NativePlatform::Impl::windowRequest(void* user, AppWindow& w, WindowRequest what) {
    Rec& r = *static_cast<Rec*>(user);
    NSWindow* win = r.window;
    if (!win) return false;
    const f32 ps = r.pointScale > 0.f ? r.pointScale : 1.f; // UI units per point
    @autoreleasepool {
        switch (what) {
        case WindowRequest::Title: win.title = [NSString stringWithUTF8String:w.title().c_str()] ?: @""; return true;
        case WindowRequest::SizeLimits: {
            const Size mn = w.minimumSize(), mx = w.maximumSize();
            win.contentMinSize = NSMakeSize(mn.w > 0.f ? mn.w / ps : 1.0, mn.h > 0.f ? mn.h / ps : 1.0);
            win.contentMaxSize = NSMakeSize(mx.w > 0.f ? mx.w / ps : CGFLOAT_MAX, mx.h > 0.f ? mx.h / ps : CGFLOAT_MAX);
            return true;
        }
        case WindowRequest::Mode: { // the delegate reports the resulting state (FuseWindowDelegate syncMode)
            const WindowMode want = w.requestedMode();
            if (want == WindowMode::Minimized) {
                [win miniaturize:nil];
                return true;
            }
            if (win.miniaturized) [win deminiaturize:nil];
            const bool full = (win.styleMask & NSWindowStyleMaskFullScreen) != 0;
            if ((want == WindowMode::Fullscreen) != full) [win toggleFullScreen:nil];
            if (want != WindowMode::Fullscreen && (want == WindowMode::Maximized) != bool(win.zoomed)) [win zoom:nil];
            return true;
        }
        case WindowRequest::Visible:
            if (w.requestedVisible()) [win makeKeyAndOrderFront:nil];
            else [win orderOut:nil];
            w.notifyVisible(w.requestedVisible());
            return true;
        case WindowRequest::Position: {
            const Vec2 p = w.requestedPosition(); // top-left of the content area, top-left-origin screen space
            NSScreen* screen = win.screen ?: NSScreen.mainScreen;
            const CGFloat top = screen ? NSMaxY(screen.frame) : 0.0;
            NSRect content = [win contentRectForFrameRect:win.frame];
            content.origin.x = p.x / ps;
            content.origin.y = top - p.y / ps - content.size.height;
            [win setFrameOrigin:[win frameRectForContentRect:content].origin];
            return true;
        }
        case WindowRequest::Activate:
            [NSApp activateIgnoringOtherApps:YES];
            [win makeKeyAndOrderFront:nil];
            return true;
        case WindowRequest::Menu:
            if (w.menu().empty()) { // nothing to show: back to the application menu, no in-window bar either
                if (r.menuTarget) r.impl->setMainMenu(nullptr);
                return false;
            }
            r.impl->setMainMenu(&r);
            return true;
        }
    }
    return false;
}
SystemTheme NativePlatform::systemTheme() {
    m->app.setSystemTheme(macTheme());
    return m->app.systemTheme();
}
std::vector<DisplayInfo> NativePlatform::displays() const {
    std::vector<DisplayInfo> out;
    @autoreleasepool {
        NSArray<NSScreen*>* screens = NSScreen.screens;
        const CGFloat top = screens.count ? NSMaxY(screens[0].frame) : 0.0; // screens[0] has the menu bar
        for (NSScreen* s in screens) {
            DisplayInfo d;
            const f32 k = f32(s.backingScaleFactor);
            auto flip = [&](NSRect r) { // points, bottom-left origin -> pixels, top-left origin
                return Rect{f32(r.origin.x) * k, f32(top - NSMaxY(r)) * k, f32(r.size.width) * k, f32(r.size.height) * k};
            };
            d.bounds = flip(s.frame);
            d.workArea = flip(s.visibleFrame);
            d.scale = k;
            d.primary = s == screens[0];
            if (@available(macOS 10.15, *)) d.name = s.localizedName.UTF8String ?: "";
            if (@available(macOS 12.0, *)) d.refreshHz = f32(s.maximumFramesPerSecond);
            out.push_back(std::move(d));
        }
    }
    return out;
}
FileDialogStatus NativePlatform::fileDialog(const AppWindow*, const FileDialogOptions& o, std::string& path) {
    path.clear();
    @autoreleasepool {
        NSSavePanel* panel = nil;
        if (o.save) {
            panel = [NSSavePanel savePanel];
        } else {
            NSOpenPanel* open = [NSOpenPanel openPanel];
            open.canChooseFiles = !o.pickDirectory;
            open.canChooseDirectories = o.pickDirectory;
            open.allowsMultipleSelection = NO;
            panel = open;
        }
        panel.message = [NSString stringWithUTF8String:o.title.c_str()] ?: @"";
        if (!o.directory.empty())
            if (NSString* dir = [NSString stringWithUTF8String:o.directory.c_str()]) panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
        if (o.save && !o.defaultName.empty())
            if (NSString* name = [NSString stringWithUTF8String:o.defaultName.c_str()]) panel.nameFieldStringValue = name;
        NSMutableArray<NSString*>* exts = [NSMutableArray array]; // "*.png;*.jpg" -> png, jpg (other patterns are ignored)
        for (std::size_t at = 0; at <= o.filter.size();) {
            const std::size_t e = std::min(o.filter.find(';', at), o.filter.size());
            const std::string pat = o.filter.substr(at, e - at);
            if (pat.size() > 2 && pat[0] == '*' && pat[1] == '.' && pat.find_first_of("*?", 2) == std::string::npos)
                if (NSString* ext = [NSString stringWithUTF8String:pat.c_str() + 2]) [exts addObject:ext];
            at = e + 1;
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (exts.count && !o.pickDirectory) panel.allowedFileTypes = exts; // allowedContentTypes needs macOS 11
#pragma clang diagnostic pop
        if ([panel runModal] != NSModalResponseOK || !panel.URL.path) return FileDialogStatus::Cancelled;
        path = panel.URL.path.UTF8String;
        return FileDialogStatus::Accepted;
    }
}
bool openUrl(std::string_view url) {
    @autoreleasepool {
        NSString* s = [[NSString alloc] initWithBytes:url.data() length:url.size() encoding:NSUTF8StringEncoding];
        NSURL* u = s.length ? [NSURL URLWithString:s] : nil;
        return u && [[NSWorkspace sharedWorkspace] openURL:u];
    }
}
bool revealFile(std::string_view path) {
    @autoreleasepool {
        NSString* s = [[NSString alloc] initWithBytes:path.data() length:path.size() encoding:NSUTF8StringEncoding];
        if (!s.length) return false;
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:s]]];
        return true;
    }
}

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    @autoreleasepool {
        installBundleReaders();
        [NSApplication sharedApplication];
        if (NSApp.activationPolicy != NSApplicationActivationPolicyRegular) [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (!NSApp.mainMenu) NSApp.mainMenu = baseMenuBar(app.name()); // the application menu with Quit (Cmd+Q)
        [NSApp finishLaunching];
        // light / dark preference: now, and whenever the user changes it
        [[NSDistributedNotificationCenter defaultCenter] addObserver:[FuseAppTarget class]
                                                            selector:@selector(themeChanged:)
                                                                name:@"AppleInterfaceThemeChangedNotification"
                                                              object:nil];
        app.setSystemTheme(macTheme());
    }
    g_macImpl = m.get();
    app.setEventSource({m.get(), &Impl::pump});
}
NativePlatform::~NativePlatform() {
    while (!m->recs.empty()) m->recs.back()->win->setPresenter({});
    detail::vkCtxDestroy(m->vk);
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:[FuseAppTarget class]];
    if (g_macImpl == m.get()) g_macImpl = nullptr;
    m->app.setEventSource({});
}
bool NativePlatform::available() const { return true; }
void* NativePlatform::nativeDisplay() const { return (__bridge void*)NSApp; }

bool NativePlatform::setIcon(const AppWindow& w, std::span<const Image> sizes) {
    // macOS has one icon per application (the Dock tile), not per window: the largest raster becomes it
    if (!m->find(w)) return false;
    const Image* best = nullptr;
    for (const Image& i : sizes)
        if (i.width > 0 && i.height > 0 && i.pixels.size() == std::size_t(i.width) * std::size_t(i.height) && (!best || i.width > best->width))
            best = &i;
    if (!best) return false;
    @autoreleasepool {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                        pixelsWide:best->width
                                                                        pixelsHigh:best->height
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:4
                                                                          hasAlpha:YES
                                                                          isPlanar:NO
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                       bytesPerRow:best->width * 4
                                                                      bitsPerPixel:32];
        if (!rep || !rep.bitmapData) return false;
        for (int y = 0; y < best->height; ++y)
            std::memcpy(rep.bitmapData + std::size_t(y) * std::size_t(rep.bytesPerRow), best->pixels.data() + std::size_t(y) * std::size_t(best->width),
                        std::size_t(best->width) * 4u);
        NSImage* icon = [[NSImage alloc] initWithSize:NSMakeSize(best->width, best->height)];
        [icon addRepresentation:rep];
        NSApp.applicationIconImage = icon;
    }
    return true;
}
bool NativePlatform::resizeWindow(const AppWindow& w, unsigned width, unsigned height) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->window) return false;
    const f32 ps = r->pointScale > 0.f ? r->pointScale : 1.f;
    [r->window setContentSize:NSMakeSize(f32(width) / ps, f32(height) / ps)];
    return true;
}
std::vector<int> NativePlatform::iconSizes(const AppWindow& w) const {
    std::vector<int> out;
    if (!m->find(w)) return out;
    if (NSImage* icon = NSApp.applicationIconImage)
        for (NSImageRep* rep in icon.representations) out.push_back(int(rep.pixelsWide));
    return out;
}
Vec2 NativePlatform::screenOrigin(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->window || !r->view) return {};
    // top-left of the content view in a top-left-origin screen space (the UI's convention), in points
    const NSRect inScreen = [r->window convertRectToScreen:[r->view convertRect:r->view.bounds toView:nil]];
    const NSScreen* screen = r->window.screen ?: NSScreen.mainScreen;
    const CGFloat top = screen ? NSMaxY(screen.frame) : NSMaxY(inScreen);
    return {f32(NSMinX(inScreen)), f32(top - NSMaxY(inScreen))};
}
Rect NativePlatform::safeArea(const AppWindow& w) const {
    const Size s = w.size();
    return {0.f, 0.f, s.w, s.h};
}

} // namespace fuse::app

#else
// =====================================================================================================================
// iOS
// =====================================================================================================================
namespace fuse::app {
namespace {
SystemTheme iosTheme() {
    SystemTheme t;
    if (@available(iOS 13.0, *)) {
        const UIUserInterfaceStyle style = UIScreen.mainScreen.traitCollection.userInterfaceStyle;
        t.scheme = style == UIUserInterfaceStyleDark ? ColorScheme::Dark : style == UIUserInterfaceStyleLight ? ColorScheme::Light : ColorScheme::Unknown;
    }
    return t;
}
} // namespace
} // namespace fuse::app
namespace {
int (*g_iosMain)(int, char**) = nullptr;
int g_iosArgc = 0;
char** g_iosArgv = nullptr;
bool g_iosActive = true;      ///< false while the app is inactive / in the background
bool g_iosTerminating = false;
NativePlatform::Impl* g_iosImpl = nullptr;
} // namespace

@interface FuseAppDelegate : UIResponder <UIApplicationDelegate>
@end

@interface FuseView : UIView <UIKeyInput> {
@public
    FuseRec* rec;
    UITouch* __weak primary; ///< the finger driving the pointer
}
@end

@interface FuseViewController : UIViewController
@end

@implementation FuseAppDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    (void)application;
    (void)options;
    // run the app's main from the run loop, once UIKit has finished launching
    [self performSelector:@selector(runMain) withObject:nil afterDelay:0.0];
    return YES;
}
- (void)runMain {
    const int rc = g_iosMain ? g_iosMain(g_iosArgc, g_iosArgv) : 0;
    std::exit(rc); // UIApplicationMain never returns
}
- (void)applicationDidBecomeActive:(UIApplication*)application {
    (void)application;
    g_iosActive = true;
    if (g_iosImpl)
        for (FuseRec* r : g_iosImpl->recs) r->win->requestRedraw();
}
- (void)applicationWillResignActive:(UIApplication*)application {
    (void)application;
    g_iosActive = false;
}
- (void)applicationWillTerminate:(UIApplication*)application {
    (void)application;
    g_iosTerminating = true;
    if (g_iosImpl)
        for (FuseRec* r : g_iosImpl->recs) r->win->requestClose();
}
@end

@implementation FuseViewController
- (BOOL)prefersStatusBarHidden { return NO; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeNone; }
@end

@implementation FuseView
- (void)layoutSubviews {
    [super layoutSubviews];
    if (rec) rec->impl->layout(*rec, self.bounds.size);
}
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    if (rec && rec->win) rec->win->requestRedraw();
}
- (void)traitCollectionDidChange:(UITraitCollection*)previous { // light / dark preference
    [super traitCollectionDidChange:previous];
    if (rec && rec->win) rec->win->app().setSystemTheme(fuse::app::iosTheme());
}
- (fui::Vec2)uiPoint:(UITouch*)touch { return fuse::app::toUi(*rec, [touch locationInView:self]); }
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    if (!rec || !rec->win || primary) return;
    UITouch* t = touches.anyObject;
    primary = t;
    fui::UiContext& ui = rec->win->ui();
    const fui::Vec2 p = [self uiPoint:t];
    ui.touchDown(p, t.timestamp);
    rec->win->requestRedraw();
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    UITouch* t = primary;
    if (!rec || !rec->win || !t || ![touches containsObject:t]) return;
    rec->win->ui().touchMove([self uiPoint:t], t.timestamp);
    rec->win->requestRedraw();
}
- (void)endTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled {
    UITouch* t = primary;
    if (!rec || !rec->win || !t || ![touches containsObject:t]) return;
    fui::UiContext& ui = rec->win->ui();
    if (cancelled) ui.touchCancel();
    else ui.touchUp([self uiPoint:t], t.timestamp); // also clears the hover: a lifted finger hovers nothing
    primary = nil;
    rec->win->requestRedraw();
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self endTouches:touches cancelled:NO];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self endTouches:touches cancelled:YES];
}

// ---- UIKeyInput: the software keyboard ------------------------------------------------------------------------------
- (BOOL)canBecomeFirstResponder { return YES; }
- (BOOL)hasText { return YES; }
- (void)insertText:(NSString*)text {
    if (!rec || !rec->win) return;
    fui::UiContext& ui = rec->win->ui();
    if ([text isEqualToString:@"\n"]) {
        ui.keyDown(fui::Key::Enter);
        ui.keyUp(fui::Key::Enter);
    } else if ([text isEqualToString:@"\t"]) {
        ui.keyDown(fui::Key::Tab);
        ui.keyUp(fui::Key::Tab);
    } else if (text.UTF8String) {
        ui.textInput(text.UTF8String);
    }
    rec->win->requestRedraw();
}
- (void)deleteBackward {
    if (!rec || !rec->win) return;
    rec->win->ui().keyDown(fui::Key::Backspace);
    rec->win->ui().keyUp(fui::Key::Backspace);
    rec->win->requestRedraw();
}
- (UITextAutocorrectionType)autocorrectionType { return UITextAutocorrectionTypeNo; }
- (UITextAutocapitalizationType)autocapitalizationType { return UITextAutocapitalizationTypeNone; }
- (UITextSpellCheckingType)spellCheckingType { return UITextSpellCheckingTypeNo; }

// ---- hardware keyboard (arrows, Escape, shortcuts) --------------------------------------------------------------------
- (BOOL)handlePresses:(NSSet<UIPress*>*)presses down:(BOOL)down {
    if (!rec || !rec->win) return NO;
    BOOL handled = NO;
    if (@available(iOS 13.4, *)) {
        for (UIPress* press in presses) {
            UIKey* key = press.key;
            if (!key) continue;
            fui::Key k = fui::Key::Other;
            switch (key.keyCode) {
            case UIKeyboardHIDUsageKeyboardLeftArrow: k = fui::Key::Left; break;
            case UIKeyboardHIDUsageKeyboardRightArrow: k = fui::Key::Right; break;
            case UIKeyboardHIDUsageKeyboardUpArrow: k = fui::Key::Up; break;
            case UIKeyboardHIDUsageKeyboardDownArrow: k = fui::Key::Down; break;
            case UIKeyboardHIDUsageKeyboardEscape: k = fui::Key::Escape; break;
            case UIKeyboardHIDUsageKeyboardHome: k = fui::Key::Home; break;
            case UIKeyboardHIDUsageKeyboardEnd: k = fui::Key::End; break;
            case UIKeyboardHIDUsageKeyboardPageUp: k = fui::Key::PageUp; break;
            case UIKeyboardHIDUsageKeyboardPageDown: k = fui::Key::PageDown; break;
            case UIKeyboardHIDUsageKeyboardDeleteForward: k = fui::Key::Delete; break;
            default: break;
            }
            const UIKeyModifierFlags f = key.modifierFlags;
            const fui::u8 md = fui::u8(((f & UIKeyModifierShift) ? fui::kModShift : 0) | ((f & UIKeyModifierCommand) ? fui::kModCtrl : 0) |
                                       ((f & UIKeyModifierAlternate) ? fui::kModAlt : 0) | ((f & UIKeyModifierControl) ? fui::kModSuper : 0));
            // Command shortcuts (copy / paste / undo ...) by their letter; plain letters arrive through insertText
            if (k == fui::Key::Other && (f & UIKeyModifierCommand) && key.charactersIgnoringModifiers.length > 0)
                fuse::app::detail::asciiKey(k, fui::u32([key.charactersIgnoringModifiers characterAtIndex:0]));
            if (k == fui::Key::Other) continue;
            if (down) {
                if (!rec->win->ui().keyDown(k, md)) rec->win->menuShortcut(k, md); // unconsumed: a menu shortcut
            } else {
                rec->win->ui().keyUp(k, md);
            }
            handled = YES;
        }
    }
    if (handled) rec->win->requestRedraw();
    return handled;
}
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    if (![self handlePresses:presses down:YES]) [super pressesBegan:presses withEvent:event];
}
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    if (![self handlePresses:presses down:NO]) [super pressesEnded:presses withEvent:event];
}
@end

namespace fuse::app {

void NativePlatform::Impl::layout(Rec& r, CGSize points) {
    if (!r.win || points.width < 1 || points.height < 1) return;
    const f32 backing = f32(UIScreen.mainScreen.scale);
    r.pointScale = opts.scale > 0.f ? backing / opts.scale : 1.f;
    r.scale = backing / r.pointScale;
    const u32 pw = u32(std::max(1.0, std::round(points.width * backing))), ph = u32(std::max(1.0, std::round(points.height * backing)));
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (r.metal) {
        r.metal.frame = CGRectMake(0, 0, points.width, points.height);
        r.metal.contentsScale = backing; // the surface's natural extent (bounds x contentsScale) is the pixel size
#if defined(FUSE_APP_HAS_VULKAN)
        static_cast<CAMetalLayer*>(r.metal).drawableSize = CGSizeMake(pw, ph);
#endif
    }
    [CATransaction commit];
    const Size px = r.win->pixelSize();
    if (u32(px.w) != pw || u32(px.h) != ph || r.win->scale() != r.scale) r.win->resizePixels(pw, ph, r.scale);
    r.win->requestRedraw();
}

bool NativePlatform::Impl::createOsWindow(Rec& r) {
    @autoreleasepool {
        UIWindow* window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        if (!window) {
            error = "UIWindow creation failed";
            return false;
        }
        FuseViewController* controller = [[FuseViewController alloc] init];
        FuseView* view = [[FuseView alloc] initWithFrame:window.bounds];
        view->rec = &r;
        view.multipleTouchEnabled = NO;
        view.backgroundColor = UIColor.blackColor;
        view.layer.contentsGravity = kCAGravityResize;
        view.layer.magnificationFilter = kCAFilterLinear;
        view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        controller.view = view;
        window.rootViewController = controller;
        r.window = window;
        r.view = view;
        r.controller = controller;
        [window makeKeyAndVisible];
        layout(r, view.bounds.size);
    }
    return true;
}
void NativePlatform::Impl::destroyOsWindow(Rec& r) {
    @autoreleasepool {
        if (r.view) {
            [r.view resignFirstResponder];
            r.view->rec = nullptr;
        }
        if (r.window) {
            r.window.hidden = YES;
            r.window.rootViewController = nil;
        }
        r.metal = nil;
        r.view = nil;
        r.controller = nil;
        r.window = nil;
    }
}
/// The software keyboard follows the UI's text-input session.
void NativePlatform::Impl::syncKeyboard(Rec& r) {
    if (!r.view || !r.win) return;
    const bool want = r.win->ui().wantsTextInput();
    if (want == r.keyboard) return;
    r.keyboard = want;
    if (want) [r.view becomeFirstResponder];
    else [r.view resignFirstResponder];
}

bool NativePlatform::Impl::pump(void* user, Application&, f32 wait) {
    (void)user;
    @autoreleasepool {
        // inactive / backgrounded: nothing can be shown, so block until UIKit has something for us
        CFTimeInterval seconds = !g_iosActive ? 1.0e9 : wait <= 0.f ? 0.0 : std::isinf(wait) ? 1.0e9 : CFTimeInterval(wait);
        for (;;) {
            const CFRunLoopRunResult r = CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, TRUE);
            if (r != kCFRunLoopRunHandledSource) break;
            seconds = 0.0; // drain what is queued, then return to the loop
        }
    }
    return !g_iosTerminating;
}

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    installBundleReaders();
    if (!available()) {
        m->error = "no UIApplication (the process must enter through UIApplicationMain: include fuse/app/main.hpp)";
        return;
    }
    g_iosImpl = m.get();
    app.setEventSource({m.get(), &Impl::pump});
    app.setSystemTheme(iosTheme());
}

// ---- iOS platform services: one fullscreen window that belongs to the system ------------------------------------------
bool NativePlatform::Impl::windowRequest(void*, AppWindow&, WindowRequest) { return false; }
SystemTheme NativePlatform::systemTheme() {
    m->app.setSystemTheme(iosTheme());
    return m->app.systemTheme();
}
std::vector<DisplayInfo> NativePlatform::displays() const {
    std::vector<DisplayInfo> out;
    UIScreen* s = UIScreen.mainScreen;
    const f32 k = f32(s.scale);
    const Rect all{0.f, 0.f, f32(s.bounds.size.width) * k, f32(s.bounds.size.height) * k};
    out.push_back({"screen", all, all, k, f32(s.maximumFramesPerSecond), true});
    return out;
}
/// No modal file dialog on iOS (UIDocumentPickerViewController is asynchronous): use the toolkit dialog.
FileDialogStatus NativePlatform::fileDialog(const AppWindow*, const FileDialogOptions&, std::string& path) {
    path.clear();
    return FileDialogStatus::Unavailable;
}
bool openUrl(std::string_view url) {
    @autoreleasepool {
        NSString* s = [[NSString alloc] initWithBytes:url.data() length:url.size() encoding:NSUTF8StringEncoding];
        NSURL* u = s.length ? [NSURL URLWithString:s] : nil;
        if (!u || ![UIApplication sharedApplication]) return false;
        [[UIApplication sharedApplication] openURL:u options:@{} completionHandler:nil];
        return true;
    }
}
bool revealFile(std::string_view) { return false; } // no file manager to reveal in
NativePlatform::~NativePlatform() {
    while (!m->recs.empty()) m->recs.back()->win->setPresenter({});
    detail::vkCtxDestroy(m->vk);
    if (g_iosImpl == m.get()) {
        g_iosImpl = nullptr;
        m->app.setEventSource({});
    }
}
bool NativePlatform::available() const { return [UIApplication sharedApplication] != nil; }
void* NativePlatform::nativeDisplay() const { return (__bridge void*)[UIApplication sharedApplication]; }

// One full-screen window whose icon and size belong to the system.
bool NativePlatform::setIcon(const AppWindow&, std::span<const Image>) { return false; }
bool NativePlatform::resizeWindow(const AppWindow&, unsigned, unsigned) { return false; }
std::vector<int> NativePlatform::iconSizes(const AppWindow&) const { return {}; }
Vec2 NativePlatform::screenOrigin(const AppWindow&) const { return {}; }
Rect NativePlatform::safeArea(const AppWindow& w) const {
    const Size s = w.size();
    const Impl::Rec* r = m->find(w);
    if (!r || !r->view) return {0.f, 0.f, s.w, s.h};
    const UIEdgeInsets in = r->view.safeAreaInsets;
    const f32 k = r->pointScale;
    return {f32(in.left) * k, f32(in.top) * k, std::max(0.f, s.w - f32(in.left + in.right) * k), std::max(0.f, s.h - f32(in.top + in.bottom) * k)};
}

namespace detail {
/// iOS process entry: hands the app's main to the app delegate and enters UIKit (never returns).
int iosRunMain(int argc, char** argv, int (*appMain)(int, char**)) {
    g_iosMain = appMain;
    g_iosArgc = argc;
    g_iosArgv = argv;
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([FuseAppDelegate class]));
    }
}
} // namespace detail

} // namespace fuse::app
#endif

// =====================================================================================================================
// shared: presenters and the rest of NativePlatform
// =====================================================================================================================
namespace fuse::app {

bool NativePlatform::Impl::present(void* user, AppWindow& w, Rect damage) {
    Rec& r = *static_cast<Rec*>(user);
    @autoreleasepool {
        r.impl->syncKeyboard(r);
        if (r.kind == PresenterKind::Vulkan) return detail::vkWinPresent(r.vk, w, damage);
        const Image& img = w.rasterize(damage);
        if (r.view) showImage(r.view.layer, img);
    }
    return true;
}

void NativePlatform::Impl::destroy(void* user) {
    Rec* r = static_cast<Rec*>(user);
    Impl& m = *r->impl;
    detail::vkWinDestroy(r->vk);
    r->vk = nullptr;
    m.destroyOsWindow(*r);
    if (r->win) r->win->setWindowBackend({}); // a menu goes back into the window
    std::erase(m.recs, r);
    delete r;
}

PresenterKind NativePlatform::show(AppWindow& w) {
    if (const Impl::Rec* existing = m->find(w)) return existing->kind;
    if (!available()) return PresenterKind::Headless;
#if !TARGET_OS_OSX
    if (!m->recs.empty()) return PresenterKind::Headless; // one OS window: further windows stay headless
#endif
    auto* r = new Impl::Rec{};
    r->impl = m.get();
    r->win = &w;
    m->recs.push_back(r);
    if (!m->createOsWindow(*r)) {
        std::erase(m->recs, r);
        delete r;
        return PresenterKind::Headless;
    }
#if defined(FUSE_APP_HAS_VULKAN)
    if (detail::VkCtx* c = m->vkContext()) {
        @autoreleasepool {
            CAMetalLayer* metal = [CAMetalLayer layer];
            metal.opaque = YES;
            metal.contentsGravity = kCAGravityResize;
            metal.magnificationFilter = kCAFilterLinear;
            metal.contentsScale = 1.0; // Impl::layout sets the backing scale and the drawable size (physical pixels)
            metal.frame = r->view.layer.bounds;
            [r->view.layer addSublayer:metal];
            r->metal = metal;
            m->layout(*r, r->view.bounds.size);
            SurfaceSource s;
            s.kind = SurfaceSource::Metal;
            s.metalLayer = (__bridge void*)metal;
            r->vk = detail::vkWinCreate(c, s, m->opts, m->error);
            if (!r->vk) { // no device can present: fall back to the layer-contents presenter
                [metal removeFromSuperlayer];
                r->metal = nil;
            }
        }
    }
#endif
    r->kind = r->vk ? PresenterKind::Vulkan : PresenterKind::Software;
    m->layout(*r, r->view.bounds.size);
    w.ui().setClipboard({&clipboardSet, &clipboardGet, nullptr});
    w.setPresenter({r, &Impl::present, &Impl::destroy});
    w.setWindowBackend({r, &Impl::windowRequest}); // also moves an existing AppWindow menu into the menu bar (macOS)
#if TARGET_OS_OSX
    // what the app asked for before the window existed
    if (w.minimumSize().w > 0.f || w.minimumSize().h > 0.f || w.maximumSize().w > 0.f || w.maximumSize().h > 0.f)
        Impl::windowRequest(r, w, WindowRequest::SizeLimits);
    if (w.requestedMode() != WindowMode::Normal) Impl::windowRequest(r, w, WindowRequest::Mode);
    if (!w.requestedVisible()) Impl::windowRequest(r, w, WindowRequest::Visible);
#else
    w.notifyMode(WindowMode::Fullscreen); // the one window is the screen
#endif
    return r->kind;
}

PresenterKind NativePlatform::kind(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->kind : PresenterKind::Headless;
}
std::string_view NativePlatform::gpuName() const { return detail::vkCtxDeviceName(m->vk); }
bool NativePlatform::gpuDraws(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinGpuDraws(r->vk);
}
std::string_view NativePlatform::lastError() const { return m->error; }
bool NativePlatform::setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex) {
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinSetExternal(r->vk, slot, tex);
}
bool NativePlatform::capture(const AppWindow& w, Image& out) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->window) return false;
    if (r->vk) return detail::vkWinReadback(r->vk, out);
    out = w.image(); // the frame handed to the layer
    return !out.pixels.empty();
}
unsigned long long NativePlatform::nativeWindow(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && r->window ? static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>((__bridge void*)r->window)) : 0;
}
float NativePlatform::scale(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->scale : 1.f;
}
fuse::ui::Cursor NativePlatform::cursor(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->cursor : fuse::ui::Cursor::Arrow; // always Arrow on iOS (no pointer cursor)
}
bool NativePlatform::setClipboardText(std::string_view utf8) { return clipboardSet(nullptr, utf8); }
bool NativePlatform::clipboardText(std::string& out) { return clipboardGet(nullptr, out); }

// Popups stay overlays inside the window on Apple platforms (NativeOptions::osPopups is X11 / Win32).
std::size_t NativePlatform::popupCount(const AppWindow&) const { return 0; }
unsigned long long NativePlatform::popupWindow(const AppWindow&, std::size_t) const { return 0; }
Rect NativePlatform::popupScreenRect(const AppWindow&, std::size_t) const { return {}; }
bool NativePlatform::capturePopup(const AppWindow&, std::size_t, Image&) { return false; }
bool NativePlatform::postClick(unsigned long long nativeWin, Vec2 at) {
    // no synthetic OS events: feed the click to the window's context (tests, automation)
    for (Impl::Rec* r : m->recs)
        if (r->window && static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>((__bridge void*)r->window)) == nativeWin) {
            r->win->click(at);
            return true;
        }
    return false;
}

} // namespace fuse::app
