#include <fuse/scene/wire_stub.hpp>

namespace fuse::scene {

namespace {

constexpr const char* kWirePrefix = "__fuse.wire|";

} // namespace

bool isWireStubEntityName(const std::string& entityName) {
    return entityName.rfind(kWirePrefix, 0) == 0;
}

WireStubRef parseWireStubEntityName(const std::string& entityName) {
    WireStubRef ref{};
    if (!isWireStubEntityName(entityName)) {
        return ref;
    }

    const std::string payload = entityName.substr(std::char_traits<char>::length(kWirePrefix));
    const std::size_t first = payload.find('|');
    if (first == std::string::npos) {
        return ref;
    }

    const std::size_t second = payload.find('|', first + 1);
    if (second == std::string::npos) {
        ref.kind = payload.substr(0, first);
        ref.owner = payload.substr(first + 1);
        ref.value = ref.owner;
        ref.valid = !ref.kind.empty() && !ref.owner.empty();
        return ref;
    }

    ref.kind = payload.substr(0, first);
    ref.owner = payload.substr(first + 1, second - first - 1);
    ref.value = payload.substr(second + 1);
    ref.valid = !ref.kind.empty() && !ref.owner.empty() && !ref.value.empty();
    return ref;
}

} // namespace fuse::scene
