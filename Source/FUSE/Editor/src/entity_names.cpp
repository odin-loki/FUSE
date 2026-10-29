#include <fuse/editor/entity_names.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fuse::editor {

namespace {
std::string clean(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(' ');
    return s.substr(b, e - b + 1);
}
} // namespace

const std::string* EntityNames::find(ecs::EntityID id) const {
    const auto it = m_names.find(key(id));
    return it == m_names.end() ? nullptr : &it->second;
}

std::string EntityNames::get(ecs::EntityID id) const {
    const std::string* n = find(id);
    return n != nullptr ? *n : std::string();
}

void EntityNames::set(ecs::EntityID id, std::string name) {
    name = clean(std::move(name));
    if (name.empty() || !id.valid()) {
        m_names.erase(key(id));
    } else {
        m_names[key(id)] = std::move(name);
    }
}

bool EntityNames::save(const std::string& path) const {
    std::error_code ec;
    if (m_names.empty()) {
        std::filesystem::remove(path, ec);
        return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "fuse-names 1\n";
    for (const auto& [k, name] : m_names) {
        out << static_cast<u32>(k & 0xffffffffu) << ' ' << static_cast<u32>(k >> 32) << ' ' << name << '\n';
    }
    return static_cast<bool>(out);
}

bool EntityNames::load(const std::string& path) {
    m_names.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return true; // optional sidecar
    std::string line;
    if (!std::getline(in, line) || line.rfind("fuse-names 1", 0) != 0) return false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        ecs::EntityID id;
        if (!(ls >> id.index >> id.generation)) continue;
        std::string name;
        std::getline(ls, name);
        set(id, name);
    }
    return true;
}

} // namespace fuse::editor
