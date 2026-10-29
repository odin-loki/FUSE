// UI1 platform gate: the wheel is its own event (no pointer jump or mouse delta), committed text and IME pre-edit
// flow through EventPump as UTF-8, InputState collects per-frame wheel and text, events v2 round-trips them, and the
// clipboard holds UTF-8 text.
#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/events_v2.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/platform/text_services.hpp>
#include <fuse/platform/window.hpp>

#include <cstdio>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#endif
#include <cstring>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
} // namespace

int main() {
    using namespace fuse;
    using namespace fuse::platform;

    // ---- wheel: own event, position kept, no delta ----
    {
        EventPump pump;
        InputState in;
        pump.pushMouseMove(100, 50);
        pump.pushMouseWheel(100, 50, 0, -240);
        pump.pushMouseWheel(100, 50, 120, 0);
        in.beginFrame();
        in.applyPump(pump);
        expect(in.wheelY() == -240 && in.wheelX() == 120, "wheel accumulates in 1/120 units");
        expect(in.mouseX() == 100 && in.mouseY() == 50, "wheel does not move the pointer");
        expect(in.mouseDeltaX() == 0 && in.mouseDeltaY() == 0, "wheel produces no mouse delta");
        in.beginFrame();
        expect(in.wheelX() == 0 && in.wheelY() == 0, "beginFrame clears the wheel");
    }

    // ---- text input: split at code points, collected per frame ----
    {
        EventPump pump;
        // 4 x "日本" (3 bytes each) + "é" = 26 bytes -> chunks of <= 15 bytes on code-point boundaries
        const std::string text = "\xE6\x97\xA5\xE6\x9C\xAC\xE6\x97\xA5\xE6\x9C\xAC\xE6\x97\xA5\xE6\x9C\xAC\xE6\x97\xA5\xE6\x9C\xAC\xC3\xA9";
        pump.pushTextInput(text);
        std::vector<PlatformEvent> events;
        pump.drainEvents(events);
        expect(events.size() == 2u, "26 bytes -> two TextInput events");
        std::string joined;
        for (const PlatformEvent& e : events) {
            expect(e.type == PlatformEventType::TextInput, "TextInput type");
            const usize n = ::strnlen(e.text, sizeof(e.text));
            expect(n <= 15u && (n == 0 || (static_cast<unsigned char>(e.text[n - 1]) & 0xC0u) != 0xC0u),
                   "chunks end on a code point");
            joined.append(e.text, n);
        }
        expect(joined == text, "chunks reassemble");
        InputState in;
        in.beginFrame();
        for (const PlatformEvent& e : events) in.apply(e);
        expect(in.textInput() == text, "InputState collects the frame's text");
        in.beginFrame();
        expect(in.textInput().empty(), "beginFrame clears text");
    }

    // ---- IME composition ----
    {
        EventPump pump;
        pump.pushComposition("\xE3\x81\x8B\xE3\x82\x93", 3); // "かん", caret after the first kana
        PlatformEvent e;
        expect(pump.pollEvent(e) && e.type == PlatformEventType::TextComposition, "TextComposition event");
        expect(pump.composition() == "\xE3\x81\x8B\xE3\x82\x93" && pump.compositionCursor() == 3, "pre-edit + caret");
        pump.pushComposition({}, 0);
        expect(pump.pollEvent(e) && pump.composition().empty(), "empty composition ends pre-edit");
        std::string longText(400, 'a');
        pump.pushComposition(longText, 999);
        expect(pump.composition().size() == EventPump::kMaxComposition &&
                   pump.compositionCursor() == static_cast<i32>(EventPump::kMaxComposition),
               "long pre-edit truncated, caret clamped");
    }

    // ---- events v2 ----
    {
        PlatformEvent w{};
        w.type = PlatformEventType::MouseWheel;
        w.mouseX = 7;
        w.mouseY = 9;
        w.wheelY = 240;
        const EventV2 v = fromLegacy(w);
        expect(v.type == EventV2Type::Wheel && v.wheelY == 2.f && v.x == 7.f && v.y == 9.f, "v1 MouseWheel -> v2 Wheel");
        const auto back = toLegacy(v);
        expect(back && back->type == PlatformEventType::MouseWheel && back->wheelY == 240 && back->mouseX == 7,
               "v2 Wheel -> v1 MouseWheel");
        PlatformEvent t{};
        t.type = PlatformEventType::TextInput;
        std::memcpy(t.text, "\xC3\xA9", 2);
        const EventV2 tv = fromLegacy(t);
        expect(tv.type == EventV2Type::TextInput && tv.textView() == "\xC3\xA9", "v1 TextInput -> v2");
        const auto tb = toLegacy(tv);
        expect(tb && tb->type == PlatformEventType::TextInput && std::strcmp(tb->text, "\xC3\xA9") == 0, "v2 TextInput -> v1");
    }

    // ---- clipboard + text-input session ----
    {
        expect(setClipboardText("FUSE \xE2\x9C\x93"), "clipboard set");
        std::string out;
        expect(clipboardText(out) && out == "FUSE \xE2\x9C\x93", "clipboard round trip");
        Window window;
        expect(!textInputActive(window), "no session by default");
        beginTextInput(window);
        expect(textInputActive(window), "session active");
        setTextInputArea(window, 10, 20, 2, 16);
        const TextInputArea a = lastTextInputArea();
        expect(a.x == 10 && a.y == 20 && a.width == 2 && a.height == 16, "caret area recorded");
        endTextInput(window);
        expect(!textInputActive(window), "session ended");
    }

#if defined(_WIN32)
    // ---- Win32 owned window: WM_CHAR surrogate pairs, WM_MOUSEWHEEL, IMM32 pre-edit, CF_UNICODETEXT ----
    {
        WindowDesc desc{};
        desc.title = "FUSE UI1 Win32 text";
        desc.width = 320;
        desc.height = 200;
        desc.createNative = true;
        Window window(desc);
        const HWND hwnd = static_cast<HWND>(window.nativeHandle().value);
        expect(hwnd != nullptr, "Win32 owned window created");
        if (hwnd != nullptr) {
            EventPump pump;
            pump.processOsEvents();
            std::vector<PlatformEvent> drop;
            pump.drainEvents(drop);

            PostMessageW(hwnd, WM_CHAR, 0xD83D, 1);
            PostMessageW(hwnd, WM_CHAR, 0xDE00, 1);
            PostMessageW(hwnd, WM_CHAR, 0x00E9, 1);
            pump.processOsEvents();
            std::vector<PlatformEvent> events;
            pump.drainEvents(events);
            std::string text;
            for (const PlatformEvent& e : events) {
                if (e.type == PlatformEventType::TextInput) text += e.text;
            }
            expect(text == "\xF0\x9F\x98\x80\xC3\xA9", "WM_CHAR surrogate pair + BMP char -> UTF-8 TextInput");

            POINT origin{0, 0};
            ClientToScreen(hwnd, &origin);
            PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-120)),
                         MAKELPARAM(origin.x + 30, origin.y + 40));
            pump.processOsEvents();
            events.clear();
            pump.drainEvents(events);
            bool wheel = false;
            for (const PlatformEvent& e : events) {
                wheel = wheel || (e.type == PlatformEventType::MouseWheel && e.wheelY == -120 && e.mouseX == 30 &&
                                  e.mouseY == 40);
            }
            expect(wheel, "WM_MOUSEWHEEL -> MouseWheel (-120, client coordinates)");

            beginTextInput(window);
            bool imeTested = false;
            if (HIMC imc = ImmGetContext(hwnd)) {
                const wchar_t comp[] = L"\u304B\u306A";
                if (ImmSetCompositionStringW(imc, SCS_SETSTR, const_cast<wchar_t*>(comp), sizeof(comp) - sizeof(wchar_t),
                                             nullptr, 0) != FALSE) {
                    imeTested = true;
                    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR | GCS_CURSORPOS);
                    PostMessageW(hwnd, WM_NULL, 0, 0);
                    pump.processOsEvents();
                    events.clear();
                    pump.drainEvents(events);
                    bool sawComposition = false;
                    for (const PlatformEvent& e : events) sawComposition = sawComposition || e.type == PlatformEventType::TextComposition;
                    std::printf("    IMM pre-edit: '%.*s' caret %d\n", int(pump.composition().size()), pump.composition().data(),
                                pump.compositionCursor());
                    expect(sawComposition && pump.composition() == "\xE3\x81\x8B\xE3\x81\xAA",
                           "IMM32 composition string -> TextComposition (UTF-8)");
                    ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
                    PostMessageW(hwnd, WM_NULL, 0, 0);
                    pump.processOsEvents();
                    pump.drainEvents(events);
                    expect(pump.composition().empty(), "WM_IME_ENDCOMPOSITION ends pre-edit");
                }
                ImmReleaseContext(hwnd, imc);
            }
            if (!imeTested) std::printf("    note: no IMM context / ImmSetCompositionStringW refused; IME section skipped\n");
            setTextInputArea(window, 12, 24, 2, 16);
            endTextInput(window);

            // Clipboard through the raw Win32 API: what FUSE writes is CF_UNICODETEXT for other apps.
            expect(setClipboardText("Gr\xC3\xBC\xC3\x9F \xF0\x9F\x98\x80"), "clipboard set (Win32)");
            bool raw = false;
            if (OpenClipboard(nullptr)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    const auto* w = static_cast<const wchar_t*>(GlobalLock(h));
                    raw = w != nullptr && std::wcscmp(w, L"Gr\u00FC\u00DF \U0001F600") == 0;
                    GlobalUnlock(h);
                }
                CloseClipboard();
            }
            expect(raw, "CF_UNICODETEXT holds the UTF-16 text");
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                const wchar_t other[] = L"from another app \u2192";
                HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(other));
                std::memcpy(GlobalLock(mem), other, sizeof(other));
                GlobalUnlock(mem);
                SetClipboardData(CF_UNICODETEXT, mem);
                CloseClipboard();
            }
            std::string got;
            expect(clipboardText(got) && got == "from another app \xE2\x86\x92", "clipboardText reads CF_UNICODETEXT");
        }
    }
#endif

    if (g_failures == 0) std::printf("fuse_core_ui1_text_input: OK\n");
    return g_failures == 0 ? 0 : 1;
}
