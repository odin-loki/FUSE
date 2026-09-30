// UNI-U7-ASSET-1: asset payload decode, the CPU-only sink and the MPSC render-upload queue.

#include <fuse/asset/render_upload.hpp>

#include <string>
#include <utility>

namespace fuse::asset {

namespace {

bool ends_with_nocase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (usize i = 0; i < suffix.size(); ++i) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != suffix[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

const char* asset_type_name(AssetType type) {
    switch (type) {
    case AssetType::Unknown:
        return "unknown";
    case AssetType::Mesh:
        return "mesh";
    case AssetType::Texture:
        return "texture";
    case AssetType::Material:
        return "material";
    }
    return "unknown";
}

AssetType asset_type_from_path(std::string_view virtualPath) {
    if (ends_with_nocase(virtualPath, ".fusemesh")) {
        return AssetType::Mesh;
    }
    if (ends_with_nocase(virtualPath, ".fusetex")) {
        return AssetType::Texture;
    }
    if (ends_with_nocase(virtualPath, ".fusemat")) {
        return AssetType::Material;
    }
    return AssetType::Unknown;
}

AssetType AssetPayload::type() const {
    if (mesh() != nullptr) {
        return AssetType::Mesh;
    }
    if (texture() != nullptr) {
        return AssetType::Texture;
    }
    if (material() != nullptr) {
        return AssetType::Material;
    }
    return AssetType::Unknown;
}

bool decode_asset_payload(AssetType type, const u8* data, usize size, AssetPayload& out, std::string* error) {
    switch (type) {
    case AssetType::Mesh: {
        CookedMesh mesh;
        if (!deserialize_cooked_mesh(data, size, mesh, error)) {
            return false;
        }
        out.data = std::move(mesh);
        return true;
    }
    case AssetType::Texture: {
        CookedTexture texture;
        if (!parse_cooked_texture(data, size, texture, error)) {
            return false;
        }
        out.data = std::move(texture);
        return true;
    }
    case AssetType::Material: {
        CookedMaterial material;
        if (!read_cooked_material(data, size, material, error)) {
            return false;
        }
        out.data = std::move(material);
        return true;
    }
    case AssetType::Unknown:
        break;
    }
    if (error != nullptr) {
        *error = "unknown asset type";
    }
    return false;
}

RenderUploadResult CpuOnlyUploadSink::upload(const RenderUploadCommand& command) {
    RenderUploadResult result;
    result.ok = command.payload != nullptr;
    if (!result.ok) {
        result.error = "no payload";
    }
    return result;
}

void CpuOnlyUploadSink::release(const RenderUploadCommand& command) {
    (void)command;
}

// ---- MPSC queue ---------------------------------------------------------------------------------

struct RenderUploadQueue::Node {
    std::atomic<Node*> next{nullptr};
    RenderUploadCommand value;
};

RenderUploadQueue::RenderUploadQueue() {
    Node* stub = new Node(); // fuse-lint-allow(ownership): intrusive queue node, freed by tryPop / ~RenderUploadQueue
    m_head.store(stub, std::memory_order_relaxed);
    m_tail = stub;
}

RenderUploadQueue::~RenderUploadQueue() {
    Node* node = m_tail;
    while (node != nullptr) {
        Node* next = node->next.load(std::memory_order_acquire);
        delete node;
        node = next;
    }
}

void RenderUploadQueue::push(RenderUploadCommand&& command) {
    Node* node = new Node(); // fuse-lint-allow(ownership): see the constructor
    node->value = std::move(command);
    // Publish order: count first so pushedCount() >= poppedCount() always holds.
    m_pushed.fetch_add(1u, std::memory_order_acq_rel);
    Node* prev = m_head.exchange(node, std::memory_order_acq_rel);
    prev->next.store(node, std::memory_order_release);
}

bool RenderUploadQueue::tryPop(RenderUploadCommand& out) {
    Node* tail = m_tail;
    Node* next = tail->next.load(std::memory_order_acquire);
    if (next == nullptr) {
        return false;
    }
    out = std::move(next->value);
    next->value = RenderUploadCommand{};
    m_tail = next;
    delete tail;
    m_popped.fetch_add(1u, std::memory_order_acq_rel);
    return true;
}

} // namespace fuse::asset
