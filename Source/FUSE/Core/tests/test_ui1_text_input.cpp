// UI1 platform gate: the wheel is its own event (no pointer jump or mouse delta), committed text and IME pre-edit
// flow through EventPump as UTF-8, InputState collects per-frame wheel and text, events v2 round-trips them, and the
// clipboard holds UTF-8 text.
#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/events_v2.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/platform/text_services.hpp>
#include <fuse/platform/window.hpp>

#include <cstdio>
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

    if (g_failures == 0) std::printf("fuse_core_ui1_text_input: OK\n");
    return g_failures == 0 ? 0 : 1;
}
