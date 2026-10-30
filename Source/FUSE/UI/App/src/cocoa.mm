// Cocoa backend (G3). UNVERIFIED: never compiled — see fuse/app/cocoa.hpp.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CVDisplayLink.h>
#include "fuse/app/cocoa.hpp"
#include "cocoa_map.hpp"
#include "native_internal.hpp"
#include <vector>

using namespace fuse::ui;
namespace fa = fuse::app;
namespace cm = fuse::app::cocoa;

struct fa::CocoaPlatform::Impl;
@class FuseView;

namespace {
struct Win {
    fa::CocoaPlatform::Impl* impl = nullptr;
    fa::AppWindow* win = nullptr;
    NSWindow* window = nil;
    FuseView* view = nil;
    CAMetalLayer* metal = nil;           ///< layer-hosting backing when Vulkan presents
    fa::detail::VkWin* vk = nullptr;
    unsigned long long titleRev = 0; ///< AppWindow::titleRevision() shown in NSWindow.title
    CVDisplayLinkRef link = nullptr;     ///< CPU presenter pacing: one frame per display refresh (UNVERIFIED)
};
}

// ---- accessibility element per snapshot node ----
@interface FuseA11yElement : NSAccessibilityElement
@property(nonatomic) NSUInteger index;
@property(nonatomic, assign) Win* owner;
@end

@interface FuseView : NSView <NSTextInputClient>
@property(nonatomic, assign) Win* owner;
@property(nonatomic, strong) NSMutableArray<FuseA11yElement*>* a11y;
@property(nonatomic) uint64_t a11yStamp;
- (void)rebuildA11y;
@end

@implementation FuseA11yElement

@end

namespace {
std::vector<A11ySnapshotNode>& snapshotFor(Win* w) {
    static std::vector<A11ySnapshotNode> s; // main thread only
    w->win->ui().snapshotA11y(s);
    return s;
}
NSString* str(std::string_view s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }
}

@implementation FuseView {
    std::string _marked;
}
- (BOOL)isFlipped { return YES; } // top-left origin like fuse (cm::flipY for window/screen conversions)
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer {
    if (!_owner || _owner->vk) return; // the swapchain owns the CAMetalLayer's contents
    const Image& img = _owner->win->image();
    if (img.width <= 0 || img.height <= 0) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, img.pixels.data(), img.pixels.size() * 4, nullptr);
    // fuse::ui::Image is 0xAABBGGRR == RGBA bytes little-endian
    CGImageRef ci = CGImageCreate(size_t(img.width), size_t(img.height), 8, 32, size_t(img.width) * 4, cs,
                                  kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault, dp, nullptr, false,
                                  kCGRenderingIntentDefault);
    self.layer.contents = (__bridge id)ci;
    CGImageRelease(ci);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}
- (Vec2)pos:(NSEvent*)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    return {f32(p.x), f32(p.y)};
}
- (void)mouseMoved:(NSEvent*)e { _owner->win->ui().pointerMove([self pos:e]); _owner->win->requestRedraw(); }
- (void)mouseDragged:(NSEvent*)e {
    [self mouseMoved:e];
    if (auto drag = _owner->win->takeExternalDrag()) {
        NSMutableArray* items = [NSMutableArray array];
        for (auto& p : drag->paths) {
            NSURL* u = [NSURL fileURLWithPath:str(p)];
            NSDraggingItem* it = [[NSDraggingItem alloc] initWithPasteboardWriter:u];
            [it setDraggingFrame:NSMakeRect([self pos:e].x, [self pos:e].y, 32, 32) contents:nil];
            [items addObject:it];
        }
        if (items.count) [self beginDraggingSessionWithItems:items event:e source:(id<NSDraggingSource>)self];
        else _owner->win->deliverExternalDragEnd(false);
    }
}
- (NSDragOperation)draggingSession:(NSDraggingSession*)s sourceOperationMaskForDraggingContext:(NSDraggingContext)c {
    return NSDragOperationCopy;
}
- (void)draggingSession:(NSDraggingSession*)s endedAtPoint:(NSPoint)p operation:(NSDragOperation)op {
    _owner->win->deliverExternalDragEnd(op != NSDragOperationNone);
}
// NSDraggingDestination (drop in, UNVERIFIED): file URLs -> paths, else plain text.
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    if (!_owner || !_owner->win->acceptsExternalDrops()) return NSDragOperationNone;
    NSPasteboard* pb = info.draggingPasteboard;
    return [pb canReadObjectForClasses:@[ [NSURL class], [NSString class] ] options:nil] ? NSDragOperationCopy
                                                                                       : NSDragOperationNone;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info { return [self draggingEntered:info]; }
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info { return [self draggingEntered:info] != NSDragOperationNone; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    if (!_owner || !_owner->win->acceptsExternalDrops()) return NO;
    NSPasteboard* pb = info.draggingPasteboard;
    AppWindow::ExternalDrop d;
    NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
    d.pos = {f32(p.x), f32(p.y)};
    NSArray<NSURL*>* urls = [pb readObjectsForClasses:@[ [NSURL class] ]
                                              options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* u in urls) if (u.fileURL && u.path) d.paths.emplace_back(u.path.UTF8String);
    if (d.paths.empty()) {
        NSString* t = [pb stringForType:NSPasteboardTypeString];
        if (!t) return NO;
        d.mime = "text/plain";
        d.data = t.UTF8String;
    }
    _owner->win->deliverExternalDrop(d);
    return YES;
}
// HiDPI (UNVERIFIED): logical layout, backing-scale pixels; layer + swapchain follow the screen.
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (!_owner) return;
    const CGFloat sc = self.window ? self.window.backingScaleFactor : 1.0;
    self.layer.contentsScale = sc;
    if (_owner->metal) {
        const NSSize b = self.bounds.size;
        _owner->metal.drawableSize = CGSizeMake(b.width * sc, b.height * sc);
    }
    _owner->win->setScale(f32(sc));
    _owner->win->requestRedraw();
}
- (void)rightMouseDragged:(NSEvent*)e { [self mouseMoved:e]; }
- (void)otherMouseDragged:(NSEvent*)e { [self mouseMoved:e]; }
- (void)down:(NSEvent*)e button:(MouseButton)b {
    UiContext& ui = _owner->win->ui();
    ui.setModifiers(cm::modsFromFlags(e.modifierFlags));
    ui.pointerDown([self pos:e], b);
    _owner->win->requestRedraw();
}
- (void)mouseDown:(NSEvent*)e { [self down:e button:MouseButton::Left]; }
- (void)rightMouseDown:(NSEvent*)e { [self down:e button:MouseButton::Right]; }
- (void)otherMouseDown:(NSEvent*)e { [self down:e button:MouseButton::Middle]; }
- (void)mouseUp:(NSEvent*)e { _owner->win->ui().pointerUp([self pos:e]); _owner->win->requestRedraw(); }
- (void)rightMouseUp:(NSEvent*)e { [self mouseUp:e]; }
- (void)otherMouseUp:(NSEvent*)e { [self mouseUp:e]; }
- (void)scrollWheel:(NSEvent*)e {
    const f32 k = e.hasPreciseScrollingDeltas ? 1.f / 10.f : 1.f;
    _owner->win->ui().wheel([self pos:e], {f32(-e.scrollingDeltaX) * k, f32(e.scrollingDeltaY) * k});
    _owner->win->requestRedraw();
}
- (void)keyDown:(NSEvent*)e {
    UiContext& ui = _owner->win->ui();
    const u8 mods = cm::modsFromFlags(e.modifierFlags);
    const Key k = cm::keyFromVk(e.keyCode);
    // Let the input method see the key first (marked text, dead keys); it calls insertText / setMarkedText.
    if (!(mods & (kModCtrl | kModSuper))) [self interpretKeyEvents:@[ e ]];
    if (k != Key::Other && _marked.empty()) ui.keyDown(k, mods);
    _owner->win->requestRedraw();
}
- (void)keyUp:(NSEvent*)e {
    const Key k = cm::keyFromVk(e.keyCode);
    if (k != Key::Other) _owner->win->ui().keyUp(k, cm::modsFromFlags(e.modifierFlags));
}
- (void)flagsChanged:(NSEvent*)e { _owner->win->ui().setModifiers(cm::modsFromFlags(e.modifierFlags)); }
- (void)doCommandBySelector:(SEL)sel {} // keys are delivered by keyDown: as fuse Keys

// ---- NSTextInputClient ----
static std::string utf8Of(id s) {
    NSString* ns = [s isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)s string] : (NSString*)s;
    const char* c = ns.UTF8String;
    return c ? std::string(c) : std::string();
}
- (void)insertText:(id)s replacementRange:(NSRange)r {
    _marked.clear();
    UiContext& ui = _owner->win->ui();
    ui.textComposition({}, 0);
    const std::string t = utf8Of(s);
    if (!t.empty() && u8(t[0]) >= 0x20 && t[0] != 0x7F) ui.textInput(t);
}
- (void)setMarkedText:(id)s selectedRange:(NSRange)sel replacementRange:(NSRange)r {
    _marked = utf8Of(s);
    NSString* ns = [s isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)s string] : (NSString*)s;
    const i32 caret = i32([[ns substringToIndex:MIN(sel.location, ns.length)] lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    _owner->win->ui().textComposition(_marked, caret);
    _owner->win->requestRedraw();
}
- (void)unmarkText { _marked.clear(); _owner->win->ui().textComposition({}, 0); }
- (BOOL)hasMarkedText { return !_marked.empty(); }
- (NSRange)markedRange { return _marked.empty() ? NSMakeRange(NSNotFound, 0) : NSMakeRange(0, str(_marked).length); }
- (NSRange)selectedRange { return NSMakeRange(0, 0); }
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)a { return nil; }
- (NSUInteger)characterIndexForPoint:(NSPoint)p { return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)a {
    const Rect c = _owner->win->ui().textCaret(); // candidate window goes under the caret
    NSRect v = [self convertRect:NSMakeRect(c.x, c.y, MAX(c.w, 1.f), c.h) toView:nil];
    return [self.window convertRectToScreen:v];
}

// ---- NSAccessibility: one element per snapshot node, parents before children ----
- (void)rebuildA11y {
    auto& s = snapshotFor(_owner);
    _a11y = [NSMutableArray arrayWithCapacity:s.size()];
    for (NSUInteger i = 0; i < s.size(); ++i) {
        const A11ySnapshotNode& n = s[i];
        FuseA11yElement* el = [[FuseA11yElement alloc] init];
        el.index = i;
        el.owner = _owner;
        [el setAccessibilityRole:@(cm::nsRole(n.role))];
        if (const char* sr = cm::nsSubrole(n.role)) [el setAccessibilitySubrole:@(sr)];
        [el setAccessibilityLabel:str(n.name)];
        [el setAccessibilityEnabled:!n.disabled];
        [el setAccessibilityFocused:n.focused];
        if (n.role == A11yRole::CheckBox || n.role == A11yRole::Switch || n.role == A11yRole::RadioButton)
            [el setAccessibilityValue:@(n.checked ? 1 : 0)];
        else if (n.hi > n.lo) {
            [el setAccessibilityValue:@(n.value)];
            [el setAccessibilityMinValue:@(n.lo)];
            [el setAccessibilityMaxValue:@(n.hi)];
        }
        NSRect local = NSMakeRect(n.bounds.x, n.bounds.y, n.bounds.w, n.bounds.h);
        [el setAccessibilityFrame:[self.window convertRectToScreen:[self convertRect:local toView:nil]]];
        [_a11y addObject:el];
    }
    for (NSUInteger i = 0; i < s.size(); ++i) {
        const u32 p = s[i].parent;
        id parent = p == ~0u ? (id)self : (id)_a11y[p];
        [_a11y[i] setAccessibilityParent:parent];
    }
    NSMutableArray* top = [NSMutableArray array];
    std::vector<NSMutableArray*> kids(s.size(), nil);
    for (NSUInteger i = 0; i < s.size(); ++i) {
        const u32 p = s[i].parent;
        if (p == ~0u) { [top addObject:_a11y[i]]; continue; }
        if (!kids[p]) kids[p] = [NSMutableArray array];
        [kids[p] addObject:_a11y[i]];
    }
    for (NSUInteger i = 0; i < s.size(); ++i) if (kids[i]) [_a11y[i] setAccessibilityChildren:kids[i]];
    [self setAccessibilityChildren:top];
    NSAccessibilityPostNotification(self, NSAccessibilityLayoutChangedNotification);
}
- (BOOL)isAccessibilityElement { return NO; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityGroupRole; }
@end

@interface FuseWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) Win* owner;
@end
@implementation FuseWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)w { _owner->win->requestClose(); return NO; }
- (void)windowDidResize:(NSNotification*)n {
    NSSize s = _owner->view.bounds.size;
    _owner->win->resize(u32(s.width), u32(s.height));
    if (_owner->metal) {
        const CGFloat sc = _owner->window.backingScaleFactor;
        _owner->metal.drawableSize = CGSizeMake(s.width * sc, s.height * sc);
    }
    _owner->win->requestRedraw();
}
@end

struct fa::CocoaPlatform::Impl {
    Application& app;
    EventSource previous;
    std::vector<std::unique_ptr<Win>> wins;
    std::vector<FuseWindowDelegate*> delegates;
    NativeOptions opts;
    detail::VkCtx* vk = nullptr;
    bool vkTried = false;
    std::string vkError;
    explicit Impl(Application& a, const NativeOptions& o) : app(a), opts(o) {}
    ~Impl() { if (vk) detail::vkCtxDestroy(vk); }

    /// Turns `w.view` into a layer-hosting view over a CAMetalLayer and builds the swapchain on it (UNVERIFIED).
    bool attachVulkan(Win& w) {
        if (!opts.vulkan) return false;
        if (!vkTried) {
            vkTried = true;
            vk = opts.adoptDevice.instance && opts.adoptDevice.device
                     ? detail::vkCtxAdopt(detail::SurfaceSource::Metal, opts.adoptDevice, vkError)
                     : detail::vkCtxCreate(detail::SurfaceSource::Metal, vkError);
        }
        if (!vk) return false;
        CAMetalLayer* layer = [CAMetalLayer layer];
        const CGFloat sc = w.window ? w.window.backingScaleFactor : 1.0;
        layer.contentsScale = sc; // logical points, physical pixels (AppWindow::setScale)
        layer.drawableSize = CGSizeMake(w.view.bounds.size.width * sc, w.view.bounds.size.height * sc);
        layer.frame = w.view.bounds;
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        w.view.layer = layer; // set before wantsLayer: layer-hosting, AppKit won't draw into it
        w.view.wantsLayer = YES;
        detail::SurfaceSource src;
        src.kind = detail::SurfaceSource::Metal;
        src.surface = (__bridge void*)layer;
        w.vk = detail::vkWinCreate(vk, src, opts, vkError);
        if (!w.vk) { w.view.layer = nil; return false; } // back to an AppKit-owned layer + CGImage contents
        w.metal = layer;
        return true;
    }

    static bool pump(void* user, Application&, f32 wait) {
        @autoreleasepool {
            NSDate* until = wait > 0 ? [NSDate dateWithTimeIntervalSinceNow:wait] : [NSDate distantPast];
            bool any = false;
            while (NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES]) {
                [NSApp sendEvent:e];
                any = true;
                until = [NSDate distantPast];
            }
            return any;
        }
    }
    // CVDisplayLink pacing (UNVERIFIED, no macOS host): after each CPU present the window is frame-pending until the
    // display's next refresh; the link's thread hops to the main queue, clears it and posts an app-defined event so a
    // blocked pump() returns. Blocks re-check the window is still alive (it may close between refresh and dispatch).
    static CVReturn displayTick(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*,
                                void* ctx) {
        Win* w = static_cast<Win*>(ctx);
        Impl* impl = w->impl;
        dispatch_async(dispatch_get_main_queue(), ^{
            bool alive = false;
            for (auto& o : impl->wins) alive |= o.get() == w;
            if (!alive || !w->win || !w->win->framePending()) return;
            w->win->setFramePending(false);
            NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
                                              timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
            [NSApp postEvent:wake atStart:NO];
        });
        return kCVReturnSuccess;
    }
    static bool startDisplayLink(Win& w) {
        if (CVDisplayLinkCreateWithActiveCGDisplays(&w.link) != kCVReturnSuccess || !w.link) return false;
        CVDisplayLinkSetOutputCallback(w.link, &displayTick, &w);
        if (CVDisplayLinkStart(w.link) != kCVReturnSuccess) {
            CVDisplayLinkRelease(w.link);
            w.link = nullptr;
            return false;
        }
        return true;
    }
    static void stopDisplayLink(Win& w) {
        if (!w.link) return;
        CVDisplayLinkStop(w.link); // waits for a running callback; queued main-queue blocks re-check liveness
        CVDisplayLinkRelease(w.link);
        w.link = nullptr;
    }
    static bool present(void* user, AppWindow& aw, Rect damage) {
        Win& w = *static_cast<Win*>(user);
        if (w.window && w.titleRev != aw.titleRevision()) { w.titleRev = aw.titleRevision(); w.window.title = str(aw.title()); }
        if (w.vk) {
            if (!detail::vkWinPresent(w.vk, aw, damage)) return false;
        } else {
            aw.rasterizeCpu(damage);
            w.view.needsDisplay = YES;
            if (w.link) aw.setFramePending(true); // cleared by the next display refresh
        }
        if (aw.ui().wantsTextInput()) [w.view.inputContext invalidateCharacterCoordinates];
        [w.view rebuildA11y]; // TODO: only when the a11y version changed
        return true;
    }
    static void destroy(void* user) {
        Win* w = static_cast<Win*>(user);
        stopDisplayLink(*w);
        if (w->vk) detail::vkWinDestroy(w->vk); // before the layer goes away
        w->vk = nullptr;
        [w->window orderOut:nil];
        w->window = nil;
    }
};

fa::CocoaPlatform::CocoaPlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    m->previous = app.eventSource();
    app.setEventSource({m.get(), &Impl::pump});
}
fa::CocoaPlatform::~CocoaPlatform() {
    for (auto& w : m->wins) {
        Impl::stopDisplayLink(*w);
        if (w->win) w->win->setPresenter({});
        if (w->vk) detail::vkWinDestroy(w->vk);
        w->vk = nullptr;
    }
    m->app.setEventSource(m->previous);
}
bool fa::CocoaPlatform::available() const { return NSApp != nil; }
bool fa::CocoaPlatform::show(AppWindow& aw) {
    auto w = std::make_unique<Win>();
    w->impl = m.get();
    w->win = &aw;
    const Size s = aw.size();
    w->window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, s.w, s.h)
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
    w->window.title = str(aw.title());
    w->window.releasedWhenClosed = NO;
    w->window.acceptsMouseMovedEvents = YES;
    w->view = [[FuseView alloc] initWithFrame:NSMakeRect(0, 0, s.w, s.h)];
    aw.setScale(f32(w->window.backingScaleFactor)); // before the swapchain is sized from pixelSize()
    if (!m->attachVulkan(*w)) w->view.wantsLayer = YES;
    w->view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
    w->view.owner = w.get();
    if (aw.acceptsExternalDrops())
        [w->view registerForDraggedTypes:@[ NSPasteboardTypeFileURL, NSPasteboardTypeString ]];
    w->view.layer.contentsScale = w->window.backingScaleFactor; // CGImage fallback holds pixelSize() pixels
    w->window.contentView = w->view;
    FuseWindowDelegate* d = [[FuseWindowDelegate alloc] init];
    d.owner = w.get();
    w->window.delegate = d;
    m->delegates.push_back(d);
    [w->window makeFirstResponder:w->view];
    [w->window center];
    [w->window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    // Vulkan FIFO blocks on the display (vsynced); the CPU path is paced by the display link when one starts,
    // else by AppWindow::maxFrameRate
    bool vsynced = w->vk && m->opts.vsync;
    if (!w->vk && Impl::startDisplayLink(*w)) vsynced = true;
    Presenter pr{w.get(), &Impl::present, &Impl::destroy};
    pr.vsynced = vsynced;
    aw.setPresenter(pr);
    m->wins.push_back(std::move(w));
    return true;
}
void* fa::CocoaPlatform::nativeWindow(const AppWindow& aw) const {
    for (auto& w : m->wins) if (w->win == &aw) return (__bridge void*)w->window;
    return nullptr;
}
bool fa::CocoaPlatform::gpuPresents(const AppWindow& aw) const {
    for (auto& w : m->wins) if (w->win == &aw) return w->vk != nullptr;
    return false;
}
const char* fa::CocoaPlatform::gpuName() const { return m->vk ? detail::vkCtxDeviceName(m->vk) : ""; }
// G3 (UNVERIFIED, no macOS host): NSDraggingDestination drop-in and backingScaleFactor HiDPI above.
