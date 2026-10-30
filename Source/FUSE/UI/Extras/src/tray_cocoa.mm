// macOS tray (NSStatusItem + NSMenu) and global menu ([NSApp setMainMenu:]). Written against AppKit; UNVERIFIED:
// no Apple SDK on the Linux build host, never compiled (same status as App/src/cocoa.mm).
#if defined(__APPLE__)
#import <AppKit/AppKit.h>

#include <cctype>
#include <cstring>

#include "tray_internal.hpp"

using fuse::ui::MenuItem;

@interface FuseMenuTarget : NSObject
@property(nonatomic) std::vector<MenuItem>* root;
- (void)fire:(NSMenuItem*)sender;
@end

namespace fuse::ui::tray_detail {
namespace {
NSString* ns(std::string_view s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }

NSString* keyEquivalent(const std::string& sc, NSEventModifierFlags* mods) {
    *mods = 0;
    std::string key;
    std::size_t b = 0;
    while (b <= sc.size()) {
        std::size_t e = sc.find('+', b);
        if (e == std::string::npos) e = sc.size();
        const std::string p = sc.substr(b, e - b);
        if (p == "Ctrl" || p == "Cmd" || p == "Meta") *mods |= NSEventModifierFlagCommand;
        else if (p == "Shift") *mods |= NSEventModifierFlagShift;
        else if (p == "Alt") *mods |= NSEventModifierFlagOption;
        else key = p;
        b = e + 1;
    }
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    return key.size() == 1 ? ns(key) : @"";
}

NSMenu* buildMenu(std::vector<MenuItem>& items, FuseMenuTarget* target, NSString* title) {
    NSMenu* m = [[NSMenu alloc] initWithTitle:title ?: @""];
    m.autoenablesItems = NO;
    for (MenuItem& it : items) {
        if (!it.visible) continue;
        if (it.separator) { [m addItem:[NSMenuItem separatorItem]]; continue; }
        NSEventModifierFlags mods = 0;
        NSString* key = keyEquivalent(it.shortcut, &mods);
        NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:ns(stripMnemonic(it.label)) action:@selector(fire:) keyEquivalent:key];
        mi.keyEquivalentModifierMask = mods;
        mi.target = target;
        mi.enabled = it.enabled;
        mi.state = it.checked ? NSControlStateValueOn : NSControlStateValueOff;
        mi.representedObject = [NSValue valueWithPointer:&it];
        if (!it.children.empty()) mi.submenu = buildMenu(it.children, target, mi.title);
        [m addItem:mi];
    }
    return m;
}

NSImage* imageFrom(const Image& img) {
    if (img.width <= 0 || img.height <= 0) return nil;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:img.width pixelsHigh:img.height
                                                                  bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                                                                 colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:img.width * 4 bitsPerPixel:32];
    std::memcpy(rep.bitmapData, img.pixels.data(), img.pixels.size() * 4); // premultiplied RGBA matches the rep
    NSImage* im = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
    [im addRepresentation:rep];
    return im;
}

class CocoaTray final : public TrayBackend {
public:
    explicit CocoaTray(TrayState& st) : m_st(st) {}
    bool show() override {
        if (m_item) return true;
        m_target = [FuseMenuTarget new];
        m_target.root = &m_st.menu;
        m_item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        changed(~0u);
        return m_item != nil;
    }
    void hide() override {
        if (m_item) [[NSStatusBar systemStatusBar] removeStatusItem:m_item];
        m_item = nil;
    }
    bool visible() const override { return m_item != nil; }
    void changed(u32 what) override {
        if (!m_item) return;
        if (what & kIcon) m_item.button.image = imageFrom(m_st.icon);
        if (what & kTitle) m_item.button.title = m_st.icon.width > 0 ? @"" : ns(m_st.title);
        if (what & kToolTip) m_item.button.toolTip = ns(m_st.ttTitle + (m_st.ttBody.empty() ? "" : "\n" + m_st.ttBody));
        if (what & kStatus) m_item.visible = m_st.status != TrayStatus::Passive;
        if (what & kMenu) m_item.menu = buildMenu(m_st.menu, m_target, nil);
    }
    void poll() override {} // AppKit delivers menu actions from the main run loop
private:
    TrayState& m_st;
    NSStatusItem* m_item = nil;
    FuseMenuTarget* m_target = nil;
};

class CocoaMenuBar final : public MenuBackend {
public:
    bool setMenus(std::vector<MenuItem>& menus) override {
        if (!m_target) m_target = [FuseMenuTarget new];
        m_target.root = &menus;
        NSMenu* bar = [[NSMenu alloc] initWithTitle:@""];
        // first entry is the application menu (titled by the system)
        for (MenuItem& top : menus) {
            NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:ns(stripMnemonic(top.label)) action:nil keyEquivalent:@""];
            mi.submenu = buildMenu(top.children, m_target, mi.title);
            [bar addItem:mi];
        }
        [NSApp setMainMenu:bar];
        return true;
    }
    bool active() const override { return m_target != nil; }
    void poll() override {}
    std::string objectPath() const override { return "NSApp.mainMenu"; }
private:
    FuseMenuTarget* m_target = nil;
};
} // namespace

std::unique_ptr<TrayBackend> makeTrayBackend(TrayState& st) { return std::make_unique<CocoaTray>(st); }
bool trayAvailable() { return true; }
const char* trayBackendName() { return "cocoa"; }
std::unique_ptr<MenuBackend> makeMenuBackend(u64) { return std::make_unique<CocoaMenuBar>(); }
const char* menuBackendName() { return "cocoa"; }
} // namespace fuse::ui::tray_detail

@implementation FuseMenuTarget
- (void)fire:(NSMenuItem*)sender {
    auto* it = static_cast<MenuItem*>([sender.representedObject pointerValue]);
    if (!it) return;
    if (it->checkable) {
        it->checked = it->radio ? true : !it->checked;
        sender.state = it->checked ? NSControlStateValueOn : NSControlStateValueOff;
        if (it->radio)
            for (NSMenuItem* sib in sender.menu.itemArray) {
                auto* o = static_cast<MenuItem*>([sib.representedObject pointerValue]);
                if (o && o != it && o->radio) { o->checked = false; sib.state = NSControlStateValueOff; }
            }
    }
    if (auto fn = it->onTriggered) fn();
}
@end
#endif
