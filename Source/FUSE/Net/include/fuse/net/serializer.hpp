#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/net/transport.hpp>
#include <fuse/types.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace fuse::net {

/// Compact binary message serialisation — no schema overhead (B7.4).
class NetSerializer {
public:
    std::vector<byte> buffer;

    void clear();
    void reset_read();

    void write_u8(u8 v);
    void write_u16(u16 v);
    void write_u32(u32 v);
    void write_f32(f32 v);
    void write_vec3(const ecs::vec3& v);
    void write_quat(const ecs::quat& q);
    void write_str(const char* s);

    [[nodiscard]] u8 read_u8();
    [[nodiscard]] u16 read_u16();
    [[nodiscard]] u32 read_u32();
    [[nodiscard]] f32 read_f32();
    [[nodiscard]] ecs::vec3 read_vec3();
    [[nodiscard]] ecs::quat read_quat();
    void read_str(char* buf, u32 max_len);

    [[nodiscard]] bool read_complete() const { return m_read_pos >= buffer.size(); }
    [[nodiscard]] usize bytes_remaining() const;

private:
    usize m_read_pos = 0;

    void ensure_write(usize additional);
    void ensure_read(usize additional);
};

} // namespace fuse::net
