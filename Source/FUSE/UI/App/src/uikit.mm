// UIKit backend (iOS). UNVERIFIED: never compiled - see fuse/app/uikit.hpp.
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include "fuse/app/uikit.hpp"
#include "android_map.hpp" // fuse::app::uikit::traits (platform-neutral, tested on Linux)
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
};
State& g() { static State s; return s; }
NSString* str(std::string_view s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }
}

@interface FuseUIView : UIView <UIKeyInput>
@property(nonatomic, strong) NSArray* fuseA11y;
@end

@implementation FuseUIView
- (BOOL)canBecomeFirstResponder { return YES; }
- (BOOL)hasText { return YES; }
- (void)insertText:(NSString*)t {
    if (!g().win) return;
    const char* c = t.UTF8String;
    if ([t isEqualToString:@"\n"]) g().win->ui().keyDown(Key::Enter, 0), g().win->ui().keyUp(Key::Enter, 0);
    else if (c) g().win->ui().textInput(c);
    g().win->requestRedraw();
}
- (void)deleteBackward {
    if (!g().win) return;
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
    if (g().win) g().win->resize(u32(self.bounds.size.width), u32(self.bounds.size.height));
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

static bool presentFn(void* user, fa::AppWindow&, Rect) {
    FuseUIView* v = (__bridge FuseUIView*)user;
    g().win->rasterizeCpu({});
    [v presentImage];
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
    st.win->setPresenter({(__bridge void*)self.view, &presentFn, nullptr});
    if (st.setup) st.setup(*st.app, *st.win);
    st.win->requestRedraw();
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)];
    [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)tick {
    if (g().app && !g().app->step(0.f)) { [self.link invalidate]; g().win = nullptr; }
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

int fa::uikitMain(int argc, char** argv, AppDesc desc, std::function<void(Application&, AppWindow&)> setup) {
    g().desc = std::move(desc);
    g().setup = std::move(setup);
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([FuseAppDelegate class]));
    }
}
