#pragma once

#include <fuse/ecs/registry.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::ecs {

struct RegistryImage; // parsed file contents, applied to a Registry on the owning thread

struct RegistrySerialiseResult {
    bool ok = false;
    usize entityCount = 0;
    usize archetypeCount = 0;
    std::string error;
};

/// Versioned binary ECS scene format (B3.7): header, entity-record table (generations + free
/// list, so EntityIDs and parent links survive), then one block per archetype holding component
/// names, element sizes, entity ids and raw column bytes. Components are identified by
/// `component_name` via ComponentTypes, so files stay valid across builds. Dead slots missing from
/// the free list load as reserved (`Registry::destroy_entity_reserved`), so `create_at` still works.
class RegistrySerialiser {
public:
    static constexpr u32 kMagic = 0x53434546u; // 'FECS'
    static constexpr u32 kVersion = 1u;

    static RegistrySerialiseResult save(const Registry& registry, const std::string& path);
    /// Replaces `registry` contents with the file's (exact entity ids and component bytes).
    static RegistrySerialiseResult load(const std::string& path, Registry& registry);

private:
    friend class RegistryLoadQueue;
    static RegistrySerialiseResult parse(const std::string& path, RegistryImage& image);
    static RegistrySerialiseResult apply(const RegistryImage& image, Registry& registry);
};

/// Async load: file I/O and parsing run on a JobScheduler worker; the registry is only touched,
/// and callbacks only run, inside `pump()` on the thread that owns the registry (main thread).
class RegistryLoadQueue {
public:
    using Callback = std::function<void(const RegistrySerialiseResult&)>;

    RegistryLoadQueue();
    ~RegistryLoadQueue(); ///< waits for in-flight parse jobs

    RegistryLoadQueue(const RegistryLoadQueue&) = delete;
    RegistryLoadQueue& operator=(const RegistryLoadQueue&) = delete;

    void load_async(const std::string& path, Callback onComplete);
    /// Apply finished loads into `registry` and invoke their callbacks. Returns loads completed.
    u32 pump(Registry& registry);
    [[nodiscard]] u32 pending() const;

private:
    struct Request;
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Request>> m_requests;
};

} // namespace fuse::ecs
