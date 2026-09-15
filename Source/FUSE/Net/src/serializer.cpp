#include <fuse/net/serializer.hpp>

#include <algorithm>
#include <cstring>

namespace fuse::net {

void NetSerializer::clear() {
    buffer.clear();
    m_read_pos = 0;
}

void NetSerializer::reset_read() { m_read_pos = 0; }

void NetSerializer::ensure_write(usize additional) {
    if (m_read_pos > buffer.size()) {
        m_read_pos = buffer.size();
    }
    buffer.resize(buffer.size() + additional);
}

void NetSerializer::ensure_read(usize additional) {
    if (m_read_pos + additional > buffer.size()) {
        buffer.resize(m_read_pos + additional);
    }
}

void NetSerializer::write_u8(u8 v) {
    ensure_write(1);
    buffer[buffer.size() - 1] = v;
}

void NetSerializer::write_u16(u16 v) {
    ensure_write(2);
    const usize offset = buffer.size() - 2;
    std::memcpy(buffer.data() + offset, &v, sizeof(v));
}

void NetSerializer::write_u32(u32 v) {
    ensure_write(4);
    const usize offset = buffer.size() - 4;
    std::memcpy(buffer.data() + offset, &v, sizeof(v));
}

void NetSerializer::write_f32(f32 v) {
    ensure_write(4);
    const usize offset = buffer.size() - 4;
    std::memcpy(buffer.data() + offset, &v, sizeof(v));
}

void NetSerializer::write_vec3(const ecs::vec3& v) {
    write_f32(v.x);
    write_f32(v.y);
    write_f32(v.z);
}

void NetSerializer::write_quat(const ecs::quat& q) {
    write_f32(q.x);
    write_f32(q.y);
    write_f32(q.z);
    write_f32(q.w);
}

void NetSerializer::write_str(const char* s) {
    const u32 len = s != nullptr ? static_cast<u32>(std::strlen(s)) : 0u;
    write_u32(len);
    if (len == 0) {
        return;
    }
    ensure_write(len);
    const usize offset = buffer.size() - len;
    std::memcpy(buffer.data() + offset, s, len);
}

u8 NetSerializer::read_u8() {
    ensure_read(1);
    const u8 v = buffer[m_read_pos];
    m_read_pos += 1;
    return v;
}

u16 NetSerializer::read_u16() {
    ensure_read(2);
    u16 v = 0;
    std::memcpy(&v, buffer.data() + m_read_pos, sizeof(v));
    m_read_pos += 2;
    return v;
}

u32 NetSerializer::read_u32() {
    ensure_read(4);
    u32 v = 0;
    std::memcpy(&v, buffer.data() + m_read_pos, sizeof(v));
    m_read_pos += 4;
    return v;
}

f32 NetSerializer::read_f32() {
    ensure_read(4);
    f32 v = 0.f;
    std::memcpy(&v, buffer.data() + m_read_pos, sizeof(v));
    m_read_pos += 4;
    return v;
}

ecs::vec3 NetSerializer::read_vec3() {
    ecs::vec3 v{};
    v.x = read_f32();
    v.y = read_f32();
    v.z = read_f32();
    return v;
}

ecs::quat NetSerializer::read_quat() {
    ecs::quat q{};
    q.x = read_f32();
    q.y = read_f32();
    q.z = read_f32();
    q.w = read_f32();
    return q;
}

void NetSerializer::read_str(char* buf, u32 max_len) {
    if (buf == nullptr || max_len == 0) {
        return;
    }
    buf[0] = '\0';

    const u32 len = read_u32();
    const u32 copy_len = std::min(len, max_len > 0 ? max_len - 1 : 0u);
    if (copy_len > 0) {
        ensure_read(copy_len);
        std::memcpy(buf, buffer.data() + m_read_pos, copy_len);
        m_read_pos += copy_len;
    }
    buf[copy_len] = '\0';

    if (len > copy_len) {
        m_read_pos += len - copy_len;
    }
}

usize NetSerializer::bytes_remaining() const {
    if (m_read_pos >= buffer.size()) {
        return 0;
    }
    return buffer.size() - m_read_pos;
}

} // namespace fuse::net
