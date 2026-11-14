// See dialog_driver_macos.hpp. Objective-C++, ARC.
#include "dialog_driver_macos.hpp"

#import <Cocoa/Cocoa.h>

#include <cstdio>

namespace fuse::editor::test {
namespace {
NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
/// The button titled `title` somewhere under `v` (the colour panel's OK / Cancel accessory row).
NSButton* findButton(NSView* v, NSString* title) {
    if (!v) return nil;
    if ([v isKindOfClass:[NSButton class]] && [((NSButton*)v).title isEqualToString:title]) return (NSButton*)v;
    for (NSView* c in v.subviews)
        if (NSButton* b = findButton(c, title)) return b;
    return nil;
}
} // namespace

std::shared_ptr<MacDialogState> macAnswerNextDialog(MacDialogAnswer a) {
    auto st = std::make_shared<MacDialogState>();
    __block int ticks = 0, stage = 0; // stage: ticks since the expected panel came up
    NSTimer* timer = [NSTimer timerWithTimeInterval:0.2
                                            repeats:YES
                                              block:^(NSTimer* t) {
        using K = MacDialogAnswer::Kind;
        auto done = [&](int code, const char* what) {
            st->answered = code;
            st->what = what;
            [t invalidate];
        };
        ++ticks;
        NSWindow* m = NSApp.modalWindow;
        if (!m) // a panel whose modal session AppKit does not report through modalWindow: the visible one
            for (NSWindow* w in NSApp.windows)
                if (w.visible && ([w isKindOfClass:[NSSavePanel class]] || [w isKindOfClass:[NSColorPanel class]])) m = w;
        if (ticks % 25 == 1) { // what the driver sees (the gate's log, while a dialog may block the test)
            std::string seen;
            for (NSWindow* w in NSApp.windows)
                seen += std::string(" ") + NSStringFromClass([w class]).UTF8String + (w.visible ? "(visible)" : "");
            std::fprintf(stderr, "dialog driver tick %d: modal %s, stage %d, windows:%s\n", ticks,
                         NSApp.modalWindow ? NSStringFromClass([NSApp.modalWindow class]).UTF8String : "none", stage, seen.c_str());
        }
        if (!m) {
            if (st->okSent) return; // accepted; macDialogFinish settles it
            if (ticks > 300) done(0, "no modal dialog came up");
            return;
        }
        if ([m isKindOfClass:[NSColorPanel class]]) {
            NSColorPanel* p = (NSColorPanel*)m;
            if (a.kind == K::Colour) {
                p.color = [NSColor colorWithSRGBRed:a.r green:a.g blue:a.b alpha:1.0];
                if (NSButton* ok = findButton(p.accessoryView, @"OK")) {
                    [ok performClick:nil];
                    done(1, "colour panel: OK");
                } else {
                    [NSApp stopModalWithCode:NSModalResponseCancel];
                    done(-3, "colour panel without an OK button");
                }
            } else if (NSButton* cancel = findButton(p.accessoryView, @"Cancel")) {
                [cancel performClick:nil];
                done(a.kind == K::Cancel ? 1 : -4, "colour panel: Cancel");
            } else {
                [NSApp stopModalWithCode:NSModalResponseCancel];
                done(-3, "colour panel without a Cancel button");
            }
            return;
        }
        if (![m isKindOfClass:[NSSavePanel class]]) {
            [NSApp stopModalWithCode:NSModalResponseCancel];
            done(-5, ("unexpected modal window " + std::string(NSStringFromClass([m class]).UTF8String)).c_str());
            return;
        }
        NSSavePanel* p = (NSSavePanel*)m;
        const bool open = [p isKindOfClass:[NSOpenPanel class]];
        if (a.kind == K::Cancel) {
            if (++stage < 6) return; // let the panel come up first (see below)
            std::fprintf(stderr, "dialog driver: cancel:\n");
            [p cancel:nil];
            done(1, open ? "open panel: Cancel" : "save panel: Cancel");
            return;
        }
        if (a.kind == K::Colour || (a.kind == K::Save) == open) {
            [p cancel:nil];
            done(-4, open ? "an open panel where a save panel was expected" : "a save panel where an open panel was expected");
            return;
        }
        // Wait ~1 s for the panel (a remote view on recent macOS) to come up, then go to the folder / select the file
        // and name it on separate ticks, then OK until the modal session ends. Each step is logged: a call into the
        // remote panel that blocks shows up as the last line.
        ++stage;
        const int sg = stage; // (a __block variable cannot be captured by a lambda)
        auto step = [sg](const char* what) { std::fprintf(stderr, "dialog driver stage %d: %s\n", sg, what); };
        if (stage < 6) return;
        if (stage == 6) {
            step(a.kind == K::Open ? "directoryURL = the file" : "directoryURL = the folder");
            if (a.kind == K::Open) p.directoryURL = [NSURL fileURLWithPath:ns(a.file) isDirectory:NO]; // selects the file
            else p.directoryURL = [NSURL fileURLWithPath:ns(a.folder) isDirectory:YES];
            step("directoryURL set");
            return;
        }
        if (stage == 8 && a.kind == K::Save) {
            step("nameFieldStringValue");
            p.nameFieldStringValue = ns(a.name);
            step("nameFieldStringValue set");
            return;
        }
        if (stage < 11) return;
        if (stage < 40) {
            if (stage == 11 || stage % 10 == 0) step("ok:");
            [p ok:nil]; // ignored while OK is not possible yet (nothing selected): tried again next tick
            st->okSent = true;
            st->what = open ? "open panel: OK" : "save panel: OK";
            return;
        }
        [p cancel:nil];
        done(-2, "the panel did not accept OK");
    }];
    st->timer = (__bridge_retained void*)timer;
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes]; // fires in the modal panel mode too
    return st;
}

void macDialogFinish(MacDialogState& st) {
    if (!st.timer) return;
    NSTimer* timer = (__bridge_transfer NSTimer*)st.timer;
    st.timer = nullptr;
    if (timer.valid) {
        [timer invalidate];
        if (st.okSent && st.answered == 0) st.answered = 1; // the dialog call returned after OK: the panel accepted it
    }
}

} // namespace fuse::editor::test
