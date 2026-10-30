// UIKit backend (iOS). UNVERIFIED: never compiled - see fuse/app/uikit.hpp.
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <QuartzCore/CAMetalLayer.h>
#include "fuse/app/uikit.hpp"
#include "android_map.hpp" // fuse::app::uikit::traits (platform-neutral, tested on Linux)
#include "native_internal.hpp"
#include <memory>
#include <vector>

using namespace fuse::ui;
namespace fa = fuse::app;

namespace {
struct State {
    fa::AppDesc desc;
    std::function<void(fa::Application&, fa::AppWindow&)> setup;
    std::unique_ptr<fa::Application> app;
    fa::AppWindow* win = nullptr;
    fa::NativeOptions opts;
    fa::detail::VkCtx* vk = nullptr;
    fa::detail::VkWin* vkWin = nullptr;
    bool vkTried = false;
    std::string vkError;
};
State& g() { static State s; return s; }
NSString* str(std::string_view s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }
}

// UITextInput positions: UTF-16 offsets into the marked text (the only "document" FUSE exposes to UIKit).
@interface FusePos : UITextPosition
@property(nonatomic) NSInteger i;
+ (instancetype)at:(NSInteger)i;
@end
@implementation FusePos
+ (instancetype)at:(NSInteger)i { FusePos* p = [FusePos new]; p.i = i; return p; }
@end
@interface FuseRange : UITextRange
@property(nonatomic) NSInteger lo, hi;
+ (instancetype)from:(NSInteger)lo to:(NSInteger)hi;
@end
@implementation FuseRange
+ (instancetype)from:(NSInteger)lo to:(NSInteger)hi { FuseRange* r = [FuseRange new]; r.lo = MIN(lo, hi); r.hi = MAX(lo, hi); return r; }
- (UITextPosition*)start { return [FusePos at:_lo]; }
- (UITextPosition*)end { return [FusePos at:_hi]; }
- (BOOL)isEmpty { return _lo == _hi; }
@end

@interface FuseUIView : UIView <UITextInput>
@property(nonatomic, strong) NSArray* fuseA11y;
@property(nonatomic, strong) CAMetalLayer* metal;
@property(nonatomic, weak) id<UITextInputDelegate> inputDelegate;
@property(nonatomic, readonly) id<UITextInputTokenizer> tokenizer;
@property(nonatomic, copy) NSDictionary<NSAttributedStringKey, id>* markedTextStyle;
@end

@implementation FuseUIView {
    fa::uikit::MarkedText _mt;
    NSInteger _sel; ///< caret inside the marked text (UTF-16)
    UITextInputStringTokenizer* _tok;
}
- (BOOL)canBecomeFirstResponder { return YES; }
- (BOOL)hasText { return YES; }
- (void)apply:(const fa::uikit::MarkedText::Step&)st {
    UiContext& ui = g().win->ui();
    if (st.setComposition) ui.textComposition(st.composition, st.caret);
    if (!st.commit.empty()) ui.textInput(st.commit);
    g().win->requestRedraw();
}
- (void)insertText:(NSString*)t {
    if (!g().win) return;
    if ([t isEqualToString:@"\n"] && !_mt.active()) {
        g().win->ui().keyDown(Key::Enter, 0), g().win->ui().keyUp(Key::Enter, 0);
        g().win->requestRedraw();
        return;
    }
    const char* c = t.UTF8String;
    _sel = 0;
    [self apply:_mt.insert(c ? c : "")];
}
// ---- UITextInput: marked text (IME) ----
- (void)setMarkedText:(NSString*)t selectedRange:(NSRange)r {
    if (!g().win) return;
    const char* c = t.UTF8String;
    _sel = NSInteger(MIN(r.location, t.length));
    [self apply:_mt.setMarked(c ? c : "", int32_t(_sel))];
}
- (void)unmarkText {
    if (!g().win) return;
    _sel = 0;
    [self apply:_mt.unmark()];
}
- (UITextRange*)markedTextRange { return _mt.active() ? [FuseRange from:0 to:_mt.length16()] : nil; }
- (UITextRange*)selectedTextRange { return [FuseRange from:_sel to:_sel]; }
- (void)setSelectedTextRange:(UITextRange*)r { if ([r isKindOfClass:[FuseRange class]]) _sel = ((FuseRange*)r).lo; }
- (NSString*)textInRange:(UITextRange*)r {
    if (![r isKindOfClass:[FuseRange class]] || !_mt.active()) return @"";
    NSString* m = str(_mt.marked);
    const NSInteger lo = MIN(((FuseRange*)r).lo, NSInteger(m.length)), hi = MIN(((FuseRange*)r).hi, NSInteger(m.length));
    return [m substringWithRange:NSMakeRange(NSUInteger(lo), NSUInteger(hi - lo))];
}
- (void)replaceRange:(UITextRange*)r withText:(NSString*)t { [self insertText:t]; }
- (UITextPosition*)beginningOfDocument { return [FusePos at:0]; }
- (UITextPosition*)endOfDocument { return [FusePos at:_mt.length16()]; }
- (UITextRange*)textRangeFromPosition:(UITextPosition*)a toPosition:(UITextPosition*)b {
    return [FuseRange from:((FusePos*)a).i to:((FusePos*)b).i];
}
- (UITextPosition*)positionFromPosition:(UITextPosition*)p offset:(NSInteger)o {
    const NSInteger i = ((FusePos*)p).i + o;
    return i < 0 || i > _mt.length16() ? nil : [FusePos at:i];
}
- (UITextPosition*)positionFromPosition:(UITextPosition*)p inDirection:(UITextLayoutDirection)d offset:(NSInteger)o {
    const bool back = d == UITextLayoutDirectionLeft || d == UITextLayoutDirectionUp;
    return [self positionFromPosition:p offset:back ? -o : o];
}
- (NSComparisonResult)comparePosition:(UITextPosition*)a toPosition:(UITextPosition*)b {
    const NSInteger x = ((FusePos*)a).i, y = ((FusePos*)b).i;
    return x < y ? NSOrderedAscending : x > y ? NSOrderedDescending : NSOrderedSame;
}
- (NSInteger)offsetFromPosition:(UITextPosition*)a toPosition:(UITextPosition*)b { return ((FusePos*)b).i - ((FusePos*)a).i; }
- (UITextPosition*)positionWithinRange:(UITextRange*)r farthestInDirection:(UITextLayoutDirection)d {
    const bool back = d == UITextLayoutDirectionLeft || d == UITextLayoutDirectionUp;
    return back ? r.start : r.end;
}
- (UITextRange*)characterRangeByExtendingPosition:(UITextPosition*)p inDirection:(UITextLayoutDirection)d {
    const bool back = d == UITextLayoutDirectionLeft || d == UITextLayoutDirectionUp;
    return back ? [FuseRange from:0 to:((FusePos*)p).i] : [FuseRange from:((FusePos*)p).i to:_mt.length16()];
}
- (NSWritingDirection)baseWritingDirectionForPosition:(UITextPosition*)p inDirection:(UITextStorageDirection)d {
    return NSWritingDirectionNatural;
}
- (void)setBaseWritingDirection:(NSWritingDirection)w forRange:(UITextRange*)r {}
- (CGRect)caretRect {
    const Rect c = g().win ? g().win->ui().textCaret() : Rect{};
    return CGRectMake(c.x, c.y, MAX(c.w, 2.f), MAX(c.h, 1.f));
}
- (CGRect)firstRectForRange:(UITextRange*)r { return [self caretRect]; } // candidate bar anchors under the caret
- (CGRect)caretRectForPosition:(UITextPosition*)p { return [self caretRect]; }
- (NSArray<UITextSelectionRect*>*)selectionRectsForRange:(UITextRange*)r { return @[]; }
- (UITextPosition*)closestPositionToPoint:(CGPoint)p { return [FusePos at:_sel]; }
- (UITextPosition*)closestPositionToPoint:(CGPoint)p withinRange:(UITextRange*)r { return r.start; }
- (UITextRange*)characterRangeAtPoint:(CGPoint)p { return [FuseRange from:_sel to:_sel]; }
- (id<UITextInputTokenizer>)tokenizer {
    if (!_tok) _tok = [[UITextInputStringTokenizer alloc] initWithTextInput:self];
    return _tok;
}
- (void)deleteBackward {
    if (!g().win) return;
    if (_mt.active()) { [self unmarkText]; } // IME normally edits marked text itself; never strand a composition
    g().win->ui().keyDown(Key::Backspace, 0);
    g().win->ui().keyUp(Key::Backspace, 0);
    g().win->requestRedraw();
}
- (Vec2)pos:(NSSet<UITouch*>*)touches {
    CGPoint p = [touches.anyObject locationInView:self];
    return {f32(p.x), f32(p.y)};
}
- (void)touchesBegan:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {
    if (!g().win) return;
    UiContext& ui = g().win->ui();
    ui.pointerMove([self pos:t]);
    ui.pointerDown([self pos:t], MouseButton::Left);
    g().win->requestRedraw();
}
- (void)touchesMoved:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {
    if (g().win) { g().win->ui().pointerMove([self pos:t]); g().win->requestRedraw(); }
}
- (void)touchesEnded:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e {
    if (g().win) { g().win->ui().pointerUp([self pos:t]); g().win->requestRedraw(); }
}
- (void)touchesCancelled:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { [self touchesEnded:t withEvent:e]; }
- (void)layoutSubviews {
    [super layoutSubviews];
    const CGFloat sc = self.window ? self.window.screen.scale : UIScreen.mainScreen.scale;
    self.contentScaleFactor = sc;
    if (self.metal) {
        self.metal.frame = self.bounds;
        self.metal.contentsScale = sc;
        self.metal.drawableSize = CGSizeMake(self.bounds.size.width * sc, self.bounds.size.height * sc);
    }
    if (g().win) {
        g().win->setScale(f32(sc)); // points layout, pixel image/swapchain (HiDPI, UNVERIFIED)
        g().win->resize(u32(self.bounds.size.width), u32(self.bounds.size.height));
    }
}
// Presenter: CPU raster -> layer contents.
- (void)presentImage {
    const Image& img = g().win->image();
    if (img.width <= 0 || img.height <= 0) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(img.pixels.data()), CFIndex(img.pixels.size() * 4));
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    CGImageRef ci = CGImageCreate(size_t(img.width), size_t(img.height), 8, 32, size_t(img.width) * 4, cs,
                                  kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault, dp, nullptr, false,
                                  kCGRenderingIntentDefault);
    self.layer.contents = (__bridge id)ci;
    self.layer.contentsScale = CGFloat(img.width) / self.bounds.size.width;
    CGImageRelease(ci);
    CGDataProviderRelease(dp);
    CFRelease(data);
}
- (void)syncIme {
    const bool want = g().win->ui().wantsTextInput();
    if (want && !self.isFirstResponder) [self becomeFirstResponder];
    else if (!want && self.isFirstResponder) [self resignFirstResponder];
}
- (void)rebuildA11y {
    std::vector<A11ySnapshotNode> snap;
    g().win->ui().snapshotA11y(snap);
    NSMutableArray* els = [NSMutableArray arrayWithCapacity:snap.size()];
    for (const auto& n : snap) {
        if (n.name.empty() && !n.focusable) continue;
        UIAccessibilityElement* el = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
        el.accessibilityLabel = str(n.name);
        // password fields: no value is ever exposed, and VoiceOver announces "secure text field" from the hint
        if (n.protectedText) el.accessibilityHint = @"secure text field";
        el.accessibilityTraits = UIAccessibilityTraits(fa::uikit::traits(n.role, n.disabled, n.checked));
        el.accessibilityFrameInContainerSpace = CGRectMake(n.bounds.x, n.bounds.y, n.bounds.w, n.bounds.h);
        if (n.hi > n.lo) el.accessibilityValue = [NSString stringWithFormat:@"%g", double(n.value)];
        [els addObject:el];
    }
    self.fuseA11y = els;
}
- (BOOL)isAccessibilityElement { return NO; }
- (NSInteger)accessibilityElementCount { return NSInteger(self.fuseA11y.count); }
- (id)accessibilityElementAtIndex:(NSInteger)i { return self.fuseA11y[NSUInteger(i)]; }
- (NSInteger)indexOfAccessibilityElement:(id)e { return NSInteger([self.fuseA11y indexOfObject:e]); }
@end

@interface FuseSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) FuseUIView* view;
@property(nonatomic, strong) CADisplayLink* link;
@end

/// CAMetalLayer sublayer + swapchain on first present (UNVERIFIED). Failure keeps the CGImage path for good.
static fa::detail::VkWin* vulkanFor(FuseUIView* v) {
    State& st = g();
    if (st.vkWin || st.vkTried || !st.opts.vulkan) return st.vkWin;
    st.vkTried = true;
    namespace d = fa::detail;
    st.vk = st.opts.adoptDevice.instance && st.opts.adoptDevice.device
                ? d::vkCtxAdopt(d::SurfaceSource::Metal, st.opts.adoptDevice, st.vkError)
                : d::vkCtxCreate(d::SurfaceSource::Metal, st.vkError);
    if (!st.vk) return nullptr;
    CAMetalLayer* layer = [CAMetalLayer layer];
    const CGFloat sc = v.contentScaleFactor;
    layer.contentsScale = sc; // AppWindow is laid out in points, swapchain is pixelSize() (setScale)
    layer.frame = v.bounds;
    layer.drawableSize = CGSizeMake(v.bounds.size.width * sc, v.bounds.size.height * sc);
    layer.opaque = YES;
    [v.layer addSublayer:layer];
    d::SurfaceSource src;
    src.kind = d::SurfaceSource::Metal;
    src.surface = (__bridge void*)layer;
    st.vkWin = d::vkWinCreate(st.vk, src, st.opts, st.vkError);
    if (!st.vkWin) { [layer removeFromSuperlayer]; return nullptr; }
    v.metal = layer;
    NSLog(@"fuse_ui: Vulkan present on %s", d::vkCtxDeviceName(st.vk));
    return st.vkWin;
}

static bool presentFn(void* user, fa::AppWindow& aw, Rect damage) {
    FuseUIView* v = (__bridge FuseUIView*)user;
    if (fa::detail::VkWin* vw = vulkanFor(v)) {
        if (!fa::detail::vkWinPresent(vw, aw, damage)) return false;
    } else {
        g().win->rasterizeCpu({});
        [v presentImage];
    }
    [v syncIme];
    [v rebuildA11y];
    UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, nil);
    return true;
}

@implementation FuseSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)s options:(UISceneConnectionOptions*)o {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    UIViewController* vc = [[UIViewController alloc] init];
    self.view = [[FuseUIView alloc] initWithFrame:self.window.bounds];
    vc.view = self.view;
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    State& st = g();
    st.app = std::make_unique<fa::Application>(st.desc);
    const CGSize sz = self.window.bounds.size;
    st.win = &st.app->createWindow({.title = st.desc.name, .width = u32(sz.width), .height = u32(sz.height),
                                          .dpiScale = f32(self.window.screen.scale)});
    st.win->setPresenter({(__bridge void*)self.view, &presentFn, nullptr, true}); // CADisplayLink paces
    if (st.setup) st.setup(*st.app, *st.win);
    st.win->requestRedraw();
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)];
    [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)tick {
    if (g().app && !g().app->step(0.f)) {
        [self.link invalidate];
        g().win = nullptr;
        if (g().vkWin) fa::detail::vkWinDestroy(g().vkWin);
        if (g().vk) fa::detail::vkCtxDestroy(g().vk);
        g().vkWin = nullptr;
        g().vk = nullptr;
    }
}
@end

@interface FuseAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation FuseAppDelegate
- (UISceneConfiguration*)application:(UIApplication*)a configurationForConnectingSceneSession:(UISceneSession*)s
                             options:(UISceneConnectionOptions*)o {
    UISceneConfiguration* c = [[UISceneConfiguration alloc] initWithName:@"fuse" sessionRole:s.role];
    c.delegateClass = [FuseSceneDelegate class];
    return c;
}
@end

int fa::uikitMain(int argc, char** argv, AppDesc desc, std::function<void(Application&, AppWindow&)> setup,
                  const NativeOptions& o) {
    g().opts = o;
    g().desc = std::move(desc);
    g().setup = std::move(setup);
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([FuseAppDelegate class]));
    }
}
