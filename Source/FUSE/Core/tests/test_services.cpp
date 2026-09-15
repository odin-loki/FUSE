#include <fuse/alloc/frame_allocator.hpp>
#include <fuse/handle.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/object.hpp>

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

void testLoggerSink() {
    std::string lastMessage;
    fuse::log::Logger::instance().setMinLevel(fuse::log::Level::Info);
    fuse::log::Logger::instance().setSink(
        [](fuse::log::Level /*level*/, const char* message, void* userData) {
            *static_cast<std::string*>(userData) = message;
        },
        &lastMessage);

    fuse::log::info("hello %s", "fuse");
    expectTrue(lastMessage == "hello fuse", "logger routes through sink");
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
    vfs.mount(fuse::io::MountKind::Game, "/tmp/fuse_game", "/game");
    std::string resolved;
    expectTrue(vfs.resolve("/game/textures/foo.png", resolved), "vfs resolves mounted prefix");
    expectTrue(resolved == "/tmp/fuse_game/textures/foo.png", "vfs maps suffix to physical path");
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

} // namespace

int main() {
    testLoggerSink();
    testHandleGeneration();
    testFrameAllocator();
    testVfsResolve();
    testObjectHierarchy();

    if (g_failures == 0) {
        std::printf("fuse_core services tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core services tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
