#include <fuse/core/service_name.hpp>
#include <fuse/core/temp_path.hpp>
#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/handle.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/locale/string_table.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/object.hpp>
#include <fuse/gameplay/waypoint_follow.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testServiceNameOk() {
    expectTrue(fuse::core::service_name_ok("Audio"), "Audio is a valid service name");
    expectTrue(!fuse::core::service_name_ok(""), "empty service name is invalid");
    expectTrue(!fuse::core::service_name_ok("1Audio"), "leading digit is an invalid service name");
    expectTrue(!fuse::core::service_name_ok("Audio Bus"), "space is an invalid service name character");
    expectTrue(fuse::core::service_name_ok("A"), "single letter is a valid service name");
    expectTrue(fuse::core::service_name_ok("Audio_Bus2"), "underscore and digit are valid after the first letter");
    expectTrue(fuse::core::service_name_ok(std::string(32, 'A')), "32-character service name is valid");
    expectTrue(!fuse::core::service_name_ok(std::string(33, 'A')), "33-character service name is invalid");
}

void testLoggerSink() {
    std::string lastMessage;
    fuse::log::Logger::instance().setMinLevel(fuse::log::Level::Info);
    fuse::log::Logger::instance().setSink(
        [](fuse::log::Level /*level*/, const char* message, void* userData) {
            *static_cast<std::string*>(userData) = message;
        },
        &lastMessage);

    fuse::log::info("hello %s", "fuse");
#if defined(FUSE_NO_LOGGING) && FUSE_NO_LOGGING
    // Shipping strips sub-Fatal log records: the sink must never see the message.
    expectTrue(lastMessage.empty(), "shipping: info log never reaches sink");
#else
    expectTrue(lastMessage == "hello fuse", "logger routes through sink");
#endif
    fuse::log::Logger::instance().setSink(nullptr, nullptr);
}

void testHandleGeneration() {
    fuse::Handle<fuse::Object> first(1u, 10u);
    fuse::Handle<fuse::Object> stale(1u, 9u);
    expectTrue(first.isValid(), "handle is valid");
    expectTrue(first != stale, "generation mismatch detected");
}

void testFrameAllocator() {
    fuse::alloc::FrameAllocator frame(256u);
    auto* a = frame.allocate<fuse::u8>(32u);
    auto* b = frame.allocate<fuse::u8>(32u);
    expectTrue(a != nullptr && b != nullptr, "frame allocator returns storage");
    frame.reset();
    auto* c = frame.allocate<fuse::u8>(32u);
    expectTrue(c == a, "frame allocator resets bump pointer");
}

void testVfsResolve() {
    auto& vfs = fuse::io::VirtualFileSystem::instance();
    vfs.mount(fuse::io::MountKind::Game, fuse::test::tempPath("fuse_game"), "/game");
    std::string resolved;
    expectTrue(vfs.resolve("/game/textures/foo.png", resolved), "vfs resolves mounted prefix");
    expectTrue(resolved == fuse::test::tempPath("fuse_game/textures/foo.png"), "vfs maps suffix to physical path");
}

void testObjectHierarchy() {
    fuse::Object root("root");
    fuse::Object child("child");
    root.addChild(&child);
    expectTrue(child.parent() == &root, "child links to parent");
    expectTrue(root.children().size() == 1u, "parent tracks child");

    fuse::Object group("group");
    root.addChild(&group);
    child.reparent(&group);
    expectTrue(child.parent() == &group, "reparent updates parent link");
    expectTrue(root.children().size() == 1u, "reparent removes child from old parent");
}

void testLocaleStringTable() {
    fuse::locale::StringTable table;
    table.set("en", "hello", "Hello {0}");
    table.set("fr", "hello", "Bonjour {0}");
    table.setLocale("fr");
    expectTrue(table.get("hello") == "Bonjour {0}", "active locale is used when present");

    table.setLocale("de");
    expectTrue(table.get("hello") == "Hello {0}", "missing translation falls back to en");
    expectTrue(table.get("missing") == "missing", "missing key returns the key");
    expectTrue(table.format("hello", {"fuse"}) == "Hello fuse", "format replaces {0}");
    expectTrue(table.format("hello", {static_cast<const char*>(nullptr)}) == "Hello ",
               "null format arg is empty");
    expectTrue(table.format("hello", {""}) == "Hello ", "empty format arg is empty");

    table.set("en", "pair", "{0}:{1}");
    expectTrue(table.format("pair", {"a"}) == "a:{1}", "extra format placeholders stay");
}

void testWaypointPath() {
    fuse::gameplay::WaypointPath path;
    const fuse::math::Vec3 origin{0.f, 0.f, 0.f};
    const fuse::math::Vec3 empty = path.current();
    expectTrue(path.count() == 0u, "empty path has no waypoints");
    expectTrue(empty.x == 0.f && empty.y == 0.f && empty.z == 0.f, "empty path current is zero");
    expectTrue(!path.finished(), "empty path is not finished");

    path.add(origin);
    path.add({10.f, 0.f, 0.f});
    expectTrue(path.count() == 2u, "path stores both waypoints");
    expectTrue(!path.advance_if_close(origin, 0.f), "non-positive radius does not advance");
    expectTrue(path.current().x == 0.f && path.current().y == 0.f && path.current().z == 0.f,
               "path starts on the first waypoint");

    expectTrue(path.advance_if_close(origin, 1.f), "close to the first waypoint advances");
    const fuse::math::Vec3 second = path.current();
    expectTrue(second.x == 10.f && second.y == 0.f && second.z == 0.f,
               "advance moves onto the second waypoint");
    expectTrue(!path.finished(), "standing on the last waypoint is not finished yet");

    expectTrue(!path.advance_if_close({10.f, 0.f, 0.f}, 1.f), "last waypoint does not wrap");
    expectTrue(path.finished(), "reaching the last waypoint marks the path finished");
}

} // namespace

int main() {
    testServiceNameOk();
    testLoggerSink();
    testHandleGeneration();
    testFrameAllocator();
    testVfsResolve();
    testObjectHierarchy();
    testLocaleStringTable();
    testWaypointPath();

    if (g_failures == 0) {
        std::printf("fuse_core services tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core services tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
