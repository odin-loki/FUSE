#include <fuse/ecs/registry_serialiser.hpp>

#include <fuse/ecs/component_types.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fuse::ecs {

/// One archetype block as read from disk, with component types resolved.
struct ArchetypeImage {
    std::vector<const ComponentTypeInfo*> components; // file order (sorted by name)
    std::vector<EntityID> entities;
    std::vector<std::vector<std::byte>> columns; // parallel to `components`
};

struct RegistryImage {
    std::vector<u32> generations;
    std::vector<u8> alive;
    std::vector<u32> freeList;
    std::vector<ArchetypeImage> archetypes;
    usize aliveCount = 0;
};

namespace {

constexpr usize kHeaderBytes = 64;

class Writer {
public:
    template <typename T>
    void put(const T& value) {
        const auto* p = reinterpret_cast<const char*>(&value);
        m_bytes.insert(m_bytes.end(), p, p + sizeof(T));
    }
    void bytes(const void* data, usize size) {
        const auto* p = static_cast<const char*>(data);
        m_bytes.insert(m_bytes.end(), p, p + size);
    }
    std::vector<char>& data() { return m_bytes; }

private:
    std::vector<char> m_bytes;
};

class Reader {
public:
    explicit Reader(const std::vector<char>& bytes) : m_bytes(bytes) {}
    template <typename T>
    bool get(T& value) {
        return bytes(&value, sizeof(T));
    }
    bool bytes(void* out, usize size) {
        if (m_offset + size > m_bytes.size()) {
            return false;
        }
        std::memcpy(out, m_bytes.data() + m_offset, size);
        m_offset += size;
        return true;
    }
    bool skip(usize size) {
        if (m_offset + size > m_bytes.size()) {
            return false;
        }
        m_offset += size;
        return true;
    }

private:
    const std::vector<char>& m_bytes;
    usize m_offset = 0;
};

RegistrySerialiseResult fail(std::string message) {
    RegistrySerialiseResult result;
    result.error = std::move(message);
    return result;
}

} // namespace

RegistrySerialiseResult RegistrySerialiser::save(const Registry& registry, const std::string& path) {
    register_builtin_components();

    // Resolve every non-empty archetype's component names up front; unknown types abort the save.
    struct Block {
        const Archetype* archetype;
        std::vector<const ComponentTypeInfo*> components;
    };
    std::vector<Block> blocks;
    for (const Archetype& archetype : registry.m_archetypes) {
        if (archetype.count() == 0u) {
            continue;
        }
        Block block{&archetype, {}};
        for (const std::type_index& type : archetype.component_types) {
            const ComponentTypeInfo* info = ComponentTypes::find(type);
            if (info == nullptr) {
                return fail(std::string("component type not registered for serialisation: ") + type.name());
            }
            block.components.push_back(info);
        }
        std::sort(block.components.begin(), block.components.end(),
                  [](const ComponentTypeInfo* a, const ComponentTypeInfo* b) { return std::strcmp(a->name, b->name) < 0; });
        blocks.push_back(std::move(block));
    }

    Writer w;
    w.put(kMagic);
    w.put(kVersion);
    w.put(static_cast<u32>(registry.m_alive_count));
    w.put(static_cast<u32>(blocks.size()));
    w.put(static_cast<u32>(registry.m_records.size()));
    w.put(static_cast<u32>(registry.m_free_list.size()));
    w.data().resize(kHeaderBytes, 0);

    for (const auto& rec : registry.m_records) {
        w.put(rec.generation);
        w.put(static_cast<u8>(rec.alive ? 1u : 0u));
    }
    for (const u32 index : registry.m_free_list) {
        w.put(index);
    }

    for (const Block& block : blocks) {
        const Archetype& archetype = *block.archetype;
        w.put(static_cast<u32>(block.components.size()));
        w.put(static_cast<u32>(archetype.count()));
        for (const ComponentTypeInfo* info : block.components) {
            const u16 nameLength = static_cast<u16>(std::strlen(info->name));
            w.put(nameLength);
            w.bytes(info->name, nameLength);
            w.put(static_cast<u32>(info->size));
        }
        w.bytes(archetype.entities.data(), archetype.entities.size() * sizeof(EntityID));
        for (const ComponentTypeInfo* info : block.components) {
            const ComponentColumn* column = archetype.find_column(info->type);
            if (column == nullptr || column->storage.size() != archetype.count() * info->size) {
                return fail(std::string("column size mismatch for ") + info->name);
            }
            w.bytes(column->storage.data(), column->storage.size());
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail("cannot open for write: " + path);
    }
    out.write(w.data().data(), static_cast<std::streamsize>(w.data().size()));
    if (!out) {
        return fail("write failed: " + path);
    }

    RegistrySerialiseResult result;
    result.ok = true;
    result.entityCount = registry.m_alive_count;
    result.archetypeCount = blocks.size();
    return result;
}

RegistrySerialiseResult RegistrySerialiser::parse(const std::string& path, RegistryImage& image) {
    register_builtin_components();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail("cannot open: " + path);
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Reader r(bytes);

    u32 magic = 0, version = 0, aliveCount = 0, archetypeCount = 0, recordCount = 0, freeCount = 0;
    if (!r.get(magic) || !r.get(version) || !r.get(aliveCount) || !r.get(archetypeCount) || !r.get(recordCount) ||
        !r.get(freeCount) || !r.skip(kHeaderBytes - 6u * sizeof(u32))) {
        return fail("truncated header");
    }
    if (magic != kMagic) {
        return fail("bad magic (not a FUSE ECS scene)");
    }
    if (version != kVersion) {
        return fail("unsupported version " + std::to_string(version));
    }

    image = {};
    image.aliveCount = aliveCount;
    image.generations.resize(recordCount);
    image.alive.resize(recordCount);
    for (u32 i = 0; i < recordCount; ++i) {
        if (!r.get(image.generations[i]) || !r.get(image.alive[i])) {
            return fail("truncated entity records");
        }
    }
    image.freeList.resize(freeCount);
    for (u32 i = 0; i < freeCount; ++i) {
        if (!r.get(image.freeList[i]) || image.freeList[i] >= recordCount) {
            return fail("bad free list");
        }
    }

    usize entitiesSeen = 0;
    image.archetypes.resize(archetypeCount);
    for (ArchetypeImage& block : image.archetypes) {
        u32 componentCount = 0, rows = 0;
        if (!r.get(componentCount) || !r.get(rows)) {
            return fail("truncated archetype header");
        }
        for (u32 c = 0; c < componentCount; ++c) {
            u16 nameLength = 0;
            if (!r.get(nameLength)) {
                return fail("truncated component name");
            }
            std::string name(nameLength, '\0');
            u32 size = 0;
            if (!r.bytes(name.data(), nameLength) || !r.get(size)) {
                return fail("truncated component entry");
            }
            const ComponentTypeInfo* info = ComponentTypes::find(name);
            if (info == nullptr) {
                return fail("unknown component type in file: " + name);
            }
            if (info->size != size) {
                return fail("component size changed since save: " + name);
            }
            block.components.push_back(info);
        }
        block.entities.resize(rows);
        if (!r.bytes(block.entities.data(), rows * sizeof(EntityID))) {
            return fail("truncated entity ids");
        }
        for (const EntityID id : block.entities) {
            if (id.index >= recordCount || image.alive[id.index] == 0u || image.generations[id.index] != id.generation) {
                return fail("archetype entity does not match the record table");
            }
        }
        for (const ComponentTypeInfo* info : block.components) {
            std::vector<std::byte>& column = block.columns.emplace_back(rows * info->size);
            if (!r.bytes(column.data(), column.size())) {
                return fail(std::string("truncated column ") + info->name);
            }
        }
        entitiesSeen += rows;
    }
    // Entities with no components live in the empty archetype, which is not stored.
    RegistrySerialiseResult result;
    result.ok = entitiesSeen <= aliveCount;
    result.entityCount = aliveCount;
    result.archetypeCount = archetypeCount;
    if (!result.ok) {
        result.error = "more archetype rows than live entities";
    }
    return result;
}

RegistrySerialiseResult RegistrySerialiser::apply(const RegistryImage& image, Registry& registry) {
    registry.init(std::max<usize>(kMaxEntities, image.generations.size()));
    registry.m_records.resize(image.generations.size());
    for (usize i = 0; i < image.generations.size(); ++i) {
        auto& rec = registry.m_records[i];
        rec.generation = image.generations[i];
        rec.alive = false;
        rec.reserved = false;
        rec.archetype_index = 0;
        rec.row = 0;
    }
    registry.m_free_list = image.freeList;
    // Dead slots that are not on the free list were reserved (destroy_entity_reserved) when saved;
    // keep them reserved so create() still cannot reuse them and create_at can revive them.
    std::vector<u8> onFreeList(image.generations.size(), 0u);
    for (const u32 index : image.freeList) {
        onFreeList[index] = 1u;
    }
    for (usize i = 0; i < image.generations.size(); ++i) {
        registry.m_records[i].reserved = image.alive[i] == 0u && onFreeList[i] == 0u && image.generations[i] != 0u;
    }

    usize placed = 0;
    for (const ArchetypeImage& block : image.archetypes) {
        std::vector<std::type_index> types;
        for (const ComponentTypeInfo* info : block.components) {
            types.push_back(info->type);
        }
        std::sort(types.begin(), types.end());
        const u32 archetypeIndex = registry.find_or_create_archetype(types);
        Archetype& archetype = registry.m_archetypes[archetypeIndex];
        const u32 firstRow = static_cast<u32>(archetype.count());
        for (usize c = 0; c < block.components.size(); ++c) {
            ComponentColumn& column = archetype.ensure_column(block.components[c]->type, block.components[c]->size);
            column.storage.insert(column.storage.end(), block.columns[c].begin(), block.columns[c].end());
        }
        for (usize row = 0; row < block.entities.size(); ++row) {
            const EntityID id = block.entities[row];
            archetype.entities.push_back(id);
            auto& rec = registry.m_records[id.index];
            rec.alive = true;
            rec.archetype_index = archetypeIndex;
            rec.row = firstRow + static_cast<u32>(row);
        }
        placed += block.entities.size();
    }

    // Remaining live records carry no components: they belong to the empty archetype (index 0).
    for (usize i = 0; i < image.generations.size(); ++i) {
        auto& rec = registry.m_records[i];
        if (image.alive[i] != 0u && !rec.alive) {
            rec.alive = true;
            rec.archetype_index = 0;
            rec.row = static_cast<u32>(registry.m_archetypes[0].append_entity(EntityID{static_cast<u32>(i), rec.generation}));
            ++placed;
        }
    }
    registry.m_alive_count = placed;

    RegistrySerialiseResult result;
    result.ok = placed == image.aliveCount;
    result.entityCount = placed;
    result.archetypeCount = image.archetypes.size();
    if (!result.ok) {
        result.error = "restored entity count does not match header";
    }
    return result;
}

RegistrySerialiseResult RegistrySerialiser::load(const std::string& path, Registry& registry) {
    RegistryImage image;
    const RegistrySerialiseResult parsed = parse(path, image);
    if (!parsed.ok) {
        return parsed;
    }
    return apply(image, registry);
}

struct RegistryLoadQueue::Request {
    std::string path;
    Callback callback;
    RegistryImage image;
    RegistrySerialiseResult parsed;
    fuse::jobs::JobCounter done{1};
};

RegistryLoadQueue::RegistryLoadQueue() = default;

RegistryLoadQueue::~RegistryLoadQueue() {
    std::vector<std::shared_ptr<Request>> requests;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        requests = m_requests;
    }
    for (const auto& request : requests) {
        request->done.wait();
    }
}

void RegistryLoadQueue::load_async(const std::string& path, Callback onComplete) {
    auto request = std::make_shared<Request>();
    request->path = path;
    request->callback = std::move(onComplete);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_requests.push_back(request);
    }
    // Parse off-thread; the job only touches its own Request (shared ownership keeps it alive).
    fuse::jobs::JobScheduler::instance().submit([request]() {
        request->parsed = RegistrySerialiser::parse(request->path, request->image);
        request->done.signal();
    });
}

u32 RegistryLoadQueue::pump(Registry& registry) {
    std::vector<std::shared_ptr<Request>> finished;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto split = std::stable_partition(m_requests.begin(), m_requests.end(),
                                           [](const std::shared_ptr<Request>& r) { return !r->done.isComplete(); });
        finished.assign(split, m_requests.end());
        m_requests.erase(split, m_requests.end());
    }
    for (const auto& request : finished) {
        request->done.wait(); // completion is already published; this orders the parse results
        RegistrySerialiseResult result =
            request->parsed.ok ? RegistrySerialiser::apply(request->image, registry) : request->parsed;
        if (request->callback) {
            request->callback(result);
        }
    }
    return static_cast<u32>(finished.size());
}

u32 RegistryLoadQueue::pending() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<u32>(m_requests.size());
}

} // namespace fuse::ecs
